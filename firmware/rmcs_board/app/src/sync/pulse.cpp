#include "firmware/rmcs_board/app/src/sync/pulse.hpp"

#if defined(LIBRMCS_APP_PULSE_TEST) && LIBRMCS_APP_PULSE_TEST

# include <hpm_clock_drv.h>
# include <hpm_gptmr_drv.h>
# include <hpm_iomux.h>
# include <hpm_soc.h>

# include "core/src/utility/assert.hpp"
# include "firmware/rmcs_board/app/src/sync/timebase.hpp"
# include "firmware/rmcs_board/app/src/utility/interrupt_lock.hpp"

namespace librmcs::firmware::sync::pulse {
namespace {

constexpr std::uint8_t kChannel = 1;
constexpr std::uint8_t kComparator = 0;

// How far ahead a pulse must be armed. The compare register has to be written
// before the counter passes it, and the main loop is the writer, so this covers
// one comfortable main-loop period plus the host's scheduling slack.
constexpr std::uint64_t kMinLeadMicroframes = 16;
constexpr std::uint64_t kMaxLeadMicroframes = 8000;

struct HistoryEntry {
    std::uint64_t microframe;
    std::uint32_t ticks;
};

HistoryEntry history[kHistoryDepth];
std::uint32_t history_head = 0;
std::uint32_t history_count = 0;

// Captured pulses waiting for the main loop to convert them. The ISR only
// stores the raw counter value; the conversion needs the history and a division,
// neither of which belongs in an interrupt.
constexpr std::uint32_t kCaptureCapacity = 8;
std::uint32_t capture_ticks[kCaptureCapacity];
std::uint32_t capture_head = 0;
std::uint32_t capture_count = 0;

std::uint32_t counter_now() {
    return gptmr_channel_get_counter(HPM_GPTMR0, kChannel, gptmr_counter_type_normal);
}

} // namespace

void init() {
    // Crystal, not PLL. This is the entire point of using GPTMR here; see the
    // header. Divider 1 keeps the full 24 MHz, giving an exact 3000 ticks per
    // microframe.
    clock_set_source_divider(clock_gptmr0, clk_src_osc24m, 1);
    core::utility::assert_always(clock_get_frequency(clock_gptmr0) == 24'000'000U);

    // Steal the UART0 pins. Deliberate and documented: UART0 does not work while
    // this build option is on.
    HPM_IOC->PAD[IOC_PAD_PB08].FUNC_CTL = IOC_PB08_FUNC_CTL_GPTMR0_COMP_1;
    HPM_IOC->PAD[IOC_PAD_PB09].FUNC_CTL = IOC_PB09_FUNC_CTL_GPTMR0_CAPT_1;

    gptmr_channel_config_t config{};
    gptmr_channel_get_default_config(HPM_GPTMR0, &config);
    // Capture the incoming edge in hardware, and let the counter free-run over
    // its full range: a reload would fold the timeline the history relates to.
    config.mode = gptmr_work_mode_capture_at_rising_edge;
    config.reload = 0xFFFFFFFFU;
    config.enable_cmp_output = true;
    config.cmp_initial_polarity_high = false;
    // Park both comparators out of reach so no pulse is emitted before one is
    // asked for.
    config.cmp[0] = 0xFFFFFFFFU;
    config.cmp[1] = 0xFFFFFFFFU;
    core::utility::assert_always(
        gptmr_channel_config(HPM_GPTMR0, kChannel, &config, false) == status_success);

    gptmr_clear_status(HPM_GPTMR0, GPTMR_CH_CAP_IRQ_MASK(kChannel));
    gptmr_enable_irq(HPM_GPTMR0, GPTMR_CH_CAP_IRQ_MASK(kChannel));
    intc_m_enable_irq_with_priority(IRQn_GPTMR0, 2);

    gptmr_start_counter(HPM_GPTMR0, kChannel);
}

void note_sof(std::uint64_t microframe) {
    // One plain register read, on the same interrupt that already reads FRINDEX
    // and the machine timer. Recorded at full rate so the interpolation span is
    // one microframe.
    const std::uint32_t ticks = counter_now();
    history[(history_head + history_count) % kHistoryDepth] = {microframe, ticks};
    if (history_count < kHistoryDepth)
        history_count++;
    else
        history_head = (history_head + 1U) % kHistoryDepth;
}

void isr_handler() {
    if (!gptmr_check_status(HPM_GPTMR0, GPTMR_CH_CAP_IRQ_MASK(kChannel)))
        return;
    gptmr_clear_status(HPM_GPTMR0, GPTMR_CH_CAP_IRQ_MASK(kChannel));

    // The hardware latched this at the edge, so when the interrupt runs is
    // irrelevant -- which is the whole reason for doing it this way.
    const std::uint32_t ticks =
        gptmr_channel_get_counter(HPM_GPTMR0, kChannel, gptmr_counter_type_rising_edge);
    if (capture_count >= kCaptureCapacity)
        return;
    capture_ticks[(capture_head + capture_count) % kCaptureCapacity] = ticks;
    capture_count++;
}

bool schedule(std::uint64_t microframe) {
    const utility::InterruptLockGuard guard;
    if (history_count < 2)
        return false;

    const HistoryEntry& newest = history[(history_head + history_count - 1U) % kHistoryDepth];
    const auto lead = static_cast<std::int64_t>(microframe - newest.microframe);
    if (lead < static_cast<std::int64_t>(kMinLeadMicroframes)
        || lead > static_cast<std::int64_t>(kMaxLeadMicroframes))
        return false;

    // Exact multiplication, no fit: GPTMR and the microframe axis are both the
    // crystal, so 3000 ticks per microframe is not an estimate.
    const std::uint32_t target =
        newest.ticks + static_cast<std::uint32_t>(lead) * kTicksPerMicroframe;

    // Toggle-on-compare would need the polarity tracked across pulses; instead
    // the comparator is moved to the target and the output edge is what the far
    // side captures.
    gptmr_update_cmp(HPM_GPTMR0, kChannel, kComparator, target);
    return true;
}

bool take_capture(std::uint64_t& microframe_q16) {
    std::uint32_t ticks = 0;
    HistoryEntry older{};
    HistoryEntry newer{};
    {
        const utility::InterruptLockGuard guard;
        if (capture_count == 0 || history_count < 2)
            return false;
        ticks = capture_ticks[capture_head];
        capture_head = (capture_head + 1U) % kCaptureCapacity;
        capture_count--;

        // Bracket the capture with the two history entries either side of it.
        // Signed differences throughout, so a counter wrap costs nothing.
        bool found = false;
        for (std::uint32_t back = 1; back < history_count; back++) {
            const HistoryEntry& candidate_newer =
                history[(history_head + history_count - back) % kHistoryDepth];
            const HistoryEntry& candidate_older =
                history[(history_head + history_count - back - 1U) % kHistoryDepth];
            const auto offset =
                static_cast<std::int32_t>(ticks - candidate_older.ticks);
            const auto span =
                static_cast<std::int32_t>(candidate_newer.ticks - candidate_older.ticks);
            if (span > 0 && offset >= 0 && offset <= span) {
                older = candidate_older;
                newer = candidate_newer;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }

    const auto span = static_cast<std::int64_t>(newer.ticks - older.ticks);
    const auto offset = static_cast<std::int64_t>(ticks - older.ticks);
    const auto microframe_span = static_cast<std::int64_t>(newer.microframe - older.microframe);
    if (span <= 0 || microframe_span <= 0)
        return false;

    microframe_q16 = (older.microframe << 16U)
                   + static_cast<std::uint64_t>((offset * microframe_span * 65536) / span);
    return true;
}

std::uint32_t measured_ticks_per_microframe_q16() {
    const utility::InterruptLockGuard guard;
    if (history_count < 2)
        return 0;
    const HistoryEntry& oldest = history[history_head];
    const HistoryEntry& newest = history[(history_head + history_count - 1U) % kHistoryDepth];
    const auto span = static_cast<std::int64_t>(newest.ticks - oldest.ticks);
    const auto microframes = static_cast<std::int64_t>(newest.microframe - oldest.microframe);
    if (microframes <= 0)
        return 0;
    return static_cast<std::uint32_t>((span << 16U) / microframes);
}

} // namespace librmcs::firmware::sync::pulse

extern "C" {
SDK_DECLARE_EXT_ISR_M(IRQn_GPTMR0, rmcs_gptmr0_isr)
void rmcs_gptmr0_isr(void) { librmcs::firmware::sync::pulse::isr_handler(); }
}

#endif
