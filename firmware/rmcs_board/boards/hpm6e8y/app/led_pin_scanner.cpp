#include <array>
#include <cstddef>
#include <cstdint>

#include <board.h>
#include <hpm_clock_drv.h>
#include <hpm_gpio_drv.h>
#include <hpm_gpiom_drv.h>
#include <hpm_gpiom_soc_drv.h>
#include <hpm_iomux.h>
#include <hpm_mcan_drv.h>
#include <hpm_mcan_soc.h>
#include <hpm_soc.h>

namespace librmcs::firmware::board {
namespace {

constexpr uint32_t kTelemetryBaseId = 0x700U;
constexpr uint32_t kCanBaudrate = 1'000'000U;
constexpr uint32_t kCanClockDivider = 10U;
constexpr uint32_t kHighHoldMs = 700U;
// Symmetric low-hold window: some board LEDs are active-high yet sit HIGH at
// reset (nothing parks them low), so they glow before the scan touches them and
// a drive-high pass cannot reveal them -- they are already on. Driving the pad
// LOW long enough for those LEDs to visibly switch OFF is what identifies them.
constexpr uint32_t kLowHoldMs = 500U;
constexpr uint32_t kBetweenCandidatesMs = 180U;
constexpr uint8_t kMagic = 'L';
constexpr uint8_t kStageBegin = 'B';
constexpr uint8_t kStageHigh = 'H';
constexpr uint8_t kStageLow = 'L';

struct Candidate {
    char bank;
    uint8_t pin;
    uint32_t pad;
    uint32_t gpio_func;
    uint32_t gpio_do_port;
    uint32_t gpio_di_port;
    uint32_t gpiom_assign;
    uint8_t domain;
};

// Banks X/Y/Z are the always-on domain. Their pads reach the SoC GPIO only after
// a routing write through the matching domain IOC: bank X sits in the main IOC
// like A-F (no extra routing), bank Y needs PIOC, bank Z needs BIOC. In every
// case the SoC routing is ALT function 3.
enum Domain : uint8_t { kDomainMain = 0, kDomainPmic = 1, kDomainBatt = 2 };
constexpr uint32_t kSocRoutingFunc = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(3);

// This SoC exposes GPIO banks A-F, 32 pads each. The layout is fully regular:
// the IOC pad index is bank * 32 + pin, every digital pad selects GPIO through
// ALT function 0, and the GPIO DO/DI port index and the GPIOM assign index both
// equal the bank index. That regularity lets us generate the whole candidate
// table instead of hand-listing pads and mistyping one.
constexpr uint32_t kBankCount = 6;
constexpr uint32_t kPinsPerBank = 32;
constexpr uint32_t kGpioAltFunc = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(0);

// Pads with a proven fixed function on this BGA192 hpm6e8y board. EVERYTHING
// else is scanned as a LED candidate, including the EtherCAT/Ethernet pad groups
// (their pinout is still unconfirmed on this board) and pads that this package
// may not even bond out -- driving an unbonded pad is harmless, it simply never
// lights a LED. Only genuinely reserved pads are excluded here, so no real LED
// can be hidden behind an over-eager exclusion.
//
//   - UART0 console PA00/PA01 (routed to the on-board FT2232 debug bridge)
//   - JTAG PA04-PA08
//   - CAN0 PC00/PC01, CAN1 PB04/PB05, CAN2 PD08/PD09, CAN3 PD14/PD15
//   - USB0 ID/OC/PWR PF19/PF22/PF23
//   - XPI0 boot NOR flash PB25-PB31 (CS0/SCLK/DQS/D0-D3). core0 runs XIP from
//     this flash, so re-muxing any of these to GPIO hard-faults core0 mid-scan.
//     PB24 is only the unused second chip-select (CA_CS1), so it stays scannable.
constexpr bool is_reserved_pad(uint32_t bank, uint32_t pin) {
    switch (bank) {
    case 0: // A
        return pin == 0 || pin == 1 || (pin >= 4 && pin <= 8);
    case 1: // B
        return pin == 4 || pin == 5 || (pin >= 25 && pin <= 31);
    case 2: // C
        return pin == 0 || pin == 1;
    case 3: // D
        return pin == 8 || pin == 9 || pin == 14 || pin == 15;
    case 5: // F
        return pin == 19 || pin == 22 || pin == 23;
    default: // E and any other bank: fully scannable
        return false;
    }
}

// Always-on domain pads (banks X/Y/Z). The board's RGB status LED lives in this
// domain, grouped with MCAN4 (PZ00/PZ01) and UART1 (PY06/PY07) -- that is why
// the A-F sweep can never turn it off, and why the board still shows lit LEDs at
// power-up. Bank X is a plain IOC bank. Excluded: the fieldbus pins already in
// use (MCAN4 PZ00-PZ02, UART1 PY06/PY07) and pins unsafe to toggle blind
// (PY00/PY01 power-domain UART, PY05 = watchdog reset PWDG_RSTN). The remaining
// battery-domain pads (PZ03-PZ07) are the most likely RGB channels.
struct SpecialPad {
    char bank;
    uint8_t pin;
    uint32_t pad;
    uint32_t port; // GPIO DO/DI port index and GPIOM assign index for this bank
    uint8_t domain;
};

constexpr SpecialPad kSpecialPads[] = {
    {'X', 0, IOC_PAD_PX00, GPIO_DO_GPIOX, kDomainMain},
    {'X', 1, IOC_PAD_PX01, GPIO_DO_GPIOX, kDomainMain},
    {'X', 2, IOC_PAD_PX02, GPIO_DO_GPIOX, kDomainMain},
    {'X', 3, IOC_PAD_PX03, GPIO_DO_GPIOX, kDomainMain},
    {'X', 4, IOC_PAD_PX04, GPIO_DO_GPIOX, kDomainMain},
    {'X', 5, IOC_PAD_PX05, GPIO_DO_GPIOX, kDomainMain},
    {'X', 6, IOC_PAD_PX06, GPIO_DO_GPIOX, kDomainMain},
    {'X', 7, IOC_PAD_PX07, GPIO_DO_GPIOX, kDomainMain},
    {'Y', 2, IOC_PAD_PY02, GPIO_DO_GPIOY, kDomainPmic},
    {'Y', 3, IOC_PAD_PY03, GPIO_DO_GPIOY, kDomainPmic},
    {'Y', 4, IOC_PAD_PY04, GPIO_DO_GPIOY, kDomainPmic},
    {'Z', 3, IOC_PAD_PZ03, GPIO_DO_GPIOZ, kDomainBatt},
    {'Z', 4, IOC_PAD_PZ04, GPIO_DO_GPIOZ, kDomainBatt},
    {'Z', 5, IOC_PAD_PZ05, GPIO_DO_GPIOZ, kDomainBatt},
    {'Z', 6, IOC_PAD_PZ06, GPIO_DO_GPIOZ, kDomainBatt},
    {'Z', 7, IOC_PAD_PZ07, GPIO_DO_GPIOZ, kDomainBatt},
};

constexpr size_t kSpecialCount = sizeof(kSpecialPads) / sizeof(kSpecialPads[0]);

constexpr size_t count_candidates() {
    size_t count = kSpecialCount;
    for (uint32_t bank = 0; bank < kBankCount; ++bank) {
        for (uint32_t pin = 0; pin < kPinsPerBank; ++pin) {
            if (!is_reserved_pad(bank, pin)) {
                ++count;
            }
        }
    }
    return count;
}

constexpr size_t kCandidateCount = count_candidates();

constexpr std::array<Candidate, kCandidateCount> build_candidates() {
    std::array<Candidate, kCandidateCount> candidates{};
    size_t index = 0;
    for (uint32_t bank = 0; bank < kBankCount; ++bank) {
        for (uint32_t pin = 0; pin < kPinsPerBank; ++pin) {
            if (is_reserved_pad(bank, pin)) {
                continue;
            }
            candidates[index++] = Candidate{
                .bank = static_cast<char>('A' + bank),
                .pin = static_cast<uint8_t>(pin),
                .pad = bank * kPinsPerBank + pin,
                .gpio_func = kGpioAltFunc,
                .gpio_do_port = bank,
                .gpio_di_port = bank,
                .gpiom_assign = bank,
                .domain = kDomainMain,
            };
        }
    }
    for (const auto& special : kSpecialPads) {
        candidates[index++] = Candidate{
            .bank = special.bank,
            .pin = special.pin,
            .pad = special.pad,
            .gpio_func = kGpioAltFunc,
            .gpio_do_port = special.port,
            .gpio_di_port = special.port,
            .gpiom_assign = special.port,
            .domain = special.domain,
        };
    }
    return candidates;
}

constexpr std::array<Candidate, kCandidateCount> kCandidates = build_candidates();

// candidate_index must fit in one telemetry byte, and the CAN id 0x700 + index
// must stay inside the 11-bit standard id space.
static_assert(kCandidateCount <= 0xFFU);
static_assert(kTelemetryBaseId + kCandidateCount - 1 <= 0x7FFU);
constexpr uint32_t kTelemetryRamSize = MCAN_MSG_BUF_SIZE_IN_WORDS * sizeof(uint32_t);
static_assert(kTelemetryRamSize <= MCAN_MSG_BUF_BASE_VALID_END - MCAN_MSG_BUF_BASE_VALID_START);

void configure_telemetry_pads() {
    constexpr uint32_t rx_pad_ctl =
        IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1) | IOC_PAD_PAD_CTL_HYS_SET(1);

    HPM_IOC->PAD[IOC_PAD_PC00].FUNC_CTL = IOC_PC00_FUNC_CTL_MCAN0_TXD;
    HPM_IOC->PAD[IOC_PAD_PC01].FUNC_CTL = IOC_PC01_FUNC_CTL_MCAN0_RXD;
    HPM_IOC->PAD[IOC_PAD_PC01].PAD_CTL = rx_pad_ctl;
}

hpm_stat_t init_telemetry_can() {
    configure_telemetry_pads();
    clock_add_to_group(clock_can0, 1);
    clock_set_source_divider(clock_can0, clk_src_pll1_clk0, kCanClockDivider);

    mcan_deinit(HPM_MCAN0);

    const mcan_msg_buf_attr_t attr = {
        .ram_base = MCAN_MSG_BUF_BASE_VALID_START,
        .ram_size = kTelemetryRamSize,
    };
    hpm_stat_t status = mcan_set_msg_buf_attr(HPM_MCAN0, &attr);
    if (status != status_success) {
        return status;
    }

    mcan_config_t config;
    mcan_get_default_config(HPM_MCAN0, &config);
    config.baudrate = kCanBaudrate;
    config.mode = mcan_mode_normal;
    config.enable_canfd = false;
    config.disable_auto_retransmission = true;
    config.ram_config.txbuf_dedicated_txbuf_elem_count = 0;
    config.ram_config.txbuf_fifo_or_queue_elem_count = MCAN_TXBUF_SIZE_CAN_DEFAULT;
    config.ram_config.txfifo_or_txqueue_mode = MCAN_TXBUF_OPERATION_MODE_FIFO;

    return mcan_init(HPM_MCAN0, &config, clock_get_frequency(clock_can0));
}

void send_telemetry(const Candidate& candidate, size_t candidate_index, uint8_t stage) {
    mcan_tx_frame_t frame{};
    frame.use_ext_id = false;
    frame.std_id = static_cast<uint16_t>(kTelemetryBaseId + candidate_index);
    frame.rtr = false;
    frame.canfd_frame = false;
    frame.bitrate_switch = false;
    frame.dlc = 8;
    frame.data_8[0] = kMagic;
    frame.data_8[1] = static_cast<uint8_t>(candidate_index);
    frame.data_8[2] = static_cast<uint8_t>(candidate.bank);
    frame.data_8[3] = candidate.pin;
    frame.data_8[4] = stage;
    frame.data_8[5] = static_cast<uint8_t>(kCandidateCount);
    frame.data_8[6] = 0;
    frame.data_8[7] = 0;
    (void)mcan_transmit_via_txfifo_nonblocking(HPM_MCAN0, &frame, nullptr);
}

void configure_candidate_gpio(const Candidate& candidate) {
    HPM_IOC->PAD[candidate.pad].FUNC_CTL = candidate.gpio_func;
    HPM_IOC->PAD[candidate.pad].PAD_CTL = 0;
    // Always-on-domain pads must also be routed to the SoC GPIO through their
    // domain IOC (PIOC for bank Y, BIOC for bank Z); bank X and A-F need nothing.
    if (candidate.domain == kDomainPmic) {
        HPM_PIOC->PAD[candidate.pad].FUNC_CTL = kSocRoutingFunc;
    } else if (candidate.domain == kDomainBatt) {
        HPM_BIOC->PAD[candidate.pad].FUNC_CTL = kSocRoutingFunc;
    }
    gpiom_set_pin_controller(HPM_GPIOM, candidate.gpiom_assign, candidate.pin, gpiom_soc_gpio0);
    gpio_set_pin_output_with_initial(HPM_GPIO0, candidate.gpio_do_port, candidate.pin, 0);
}

void release_candidate_gpio(const Candidate& candidate) {
    gpio_write_pin(HPM_GPIO0, candidate.gpio_do_port, candidate.pin, 0);
    gpio_set_pin_input(HPM_GPIO0, candidate.gpio_di_port, candidate.pin);
}

void scan_candidate(const Candidate& candidate, size_t candidate_index) {
    configure_candidate_gpio(candidate);
    send_telemetry(candidate, candidate_index, kStageBegin);
    board_delay_ms(40);

    gpio_write_pin(HPM_GPIO0, candidate.gpio_do_port, candidate.pin, 1);
    for (uint32_t elapsed_ms = 0; elapsed_ms < kHighHoldMs; elapsed_ms += 100U) {
        send_telemetry(candidate, candidate_index, kStageHigh);
        board_delay_ms(100);
    }

    gpio_write_pin(HPM_GPIO0, candidate.gpio_do_port, candidate.pin, 0);
    for (uint32_t elapsed_ms = 0; elapsed_ms < kLowHoldMs; elapsed_ms += 100U) {
        send_telemetry(candidate, candidate_index, kStageLow);
        board_delay_ms(100);
    }

    board_delay_ms(kBetweenCandidatesMs);
    release_candidate_gpio(candidate);
}

} // namespace

int led_pin_scanner_main() {
    (void)init_telemetry_can();

    while (true) {
        for (size_t candidate_index = 0; candidate_index < kCandidateCount; ++candidate_index) {
            scan_candidate(kCandidates[candidate_index], candidate_index);
        }
        board_delay_ms(1000);
    }

    return 0;
}

} // namespace librmcs::firmware::board
