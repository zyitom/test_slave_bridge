#include <array>
#include <cstddef>
#include <cstdint>

#include <board.h>
#include <hpm_clock_drv.h>
#include <hpm_enet_drv.h>
#include <hpm_esc_drv.h>
#include <hpm_gpio_drv.h>
#include <hpm_gpiom_drv.h>
#include <hpm_gpiom_soc_drv.h>
#include <hpm_iomux.h>
#include <hpm_mcan_drv.h>
#include <hpm_mcan_soc.h>
#include <hpm_soc.h>

namespace librmcs::firmware::board {
namespace {

// The external Realtek RTL8211F (PHY ID 0x001cc916) reads all 0xffff on its MDIO
// (PF00/PF01, ENET0 SMI) -- the bus is electrically alive but no PHY answers, so
// the PHY is held in reset or unpowered. The RTL8211F runs from its own 25 MHz
// crystal, so the most likely single blocker is its active-low PHYRSTB tied to an
// unknown SoC GPIO that no firmware releases. This image hunts that pin: it drives
// each candidate pad low (assert) then high (release), waits for the PHY to come
// up, and reads the ENET0 MDIO. The instant the Realtek OUI appears it latches the
// pad high and reports it over CAN0. If the real blocker is a missing clock rather
// than a static reset, a driven GPIO cannot reveal it and this scan will not find
// it -- that is the known limit of this approach.

constexpr uint32_t kTelemetryBaseId = 0x700U;
constexpr uint32_t kBaselineId = 0x7EEU;
constexpr uint32_t kIdentityId = 0x7EFU;
constexpr uint32_t kFoundId = 0x7F0U;
constexpr uint32_t kCanBaudrate = 1'000'000U;
constexpr uint32_t kCanClockDivider = 10U;
constexpr uint32_t kAssertLowMs = 15U;
constexpr uint32_t kReleaseSettleMs = 160U;
constexpr uint32_t kBetweenCandidatesMs = 40U;
constexpr uint8_t kScannerVersion = 1U;
constexpr uint8_t kScanMagic = 'S';
constexpr uint8_t kFoundMagic = 'F';
constexpr uint8_t kStageBegin = 'B';
constexpr uint8_t kStageResult = 'R';

// RTL8211F identity: OUI high word 0x001C, model/rev in the low word (0xC916).
// Match on the OUI high word so a strapped/revised low word still counts.
constexpr uint16_t kRealtekOuiMsb = 0x001CU;
constexpr uint8_t kPhyIdReg1 = 2U;
constexpr uint8_t kPhyIdReg2 = 3U;
constexpr uint8_t kMaxPhyAddr = 8U;

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

enum Domain : uint8_t { kDomainMain = 0, kDomainPmic = 1, kDomainBatt = 2 };
constexpr uint32_t kSocRoutingFunc = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(3);

constexpr uint32_t kBankCount = 6;
constexpr uint32_t kPinsPerBank = 32;
constexpr uint32_t kGpioAltFunc = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(0);

// Pads with a proven fixed function on this hpm6e8y board are excluded so the
// scan cannot break itself or corrupt the CAN telemetry / XIP flash. Mirrors the
// LED scanner exclusions, plus PF00/PF01 (this image's ENET0 MDC/MDIO) which must
// stay muxed as the MDIO bus that detection reads.
//
//   - UART0 console PA00/PA01
//   - JTAG PA04-PA08
//   - Internal PHY analog/strap pins PA16-PA29
//   - CAN0 PC00/PC01, CAN1 PB04/PB05, CAN2 PD08/PD09, CAN3 PD14/PD15
//   - USB0 ID/OC/PWR PF19/PF22/PF23
//   - ENET0 MDC/MDIO PF00/PF01 (the detection bus)
//   - XPI0 boot NOR flash PB25-PB31 (core0 XIP; re-muxing hard-faults core0)
constexpr bool is_reserved_pad(uint32_t bank, uint32_t pin) {
    switch (bank) {
    case 0:  // A
        return pin == 0 || pin == 1 || (pin >= 4 && pin <= 8) || (pin >= 16 && pin <= 29);
    case 1:  // B
        return pin == 4 || pin == 5 || (pin >= 25 && pin <= 31);
    case 2:  // C
        return pin == 0 || pin == 1;
    case 3:  // D
        return pin == 8 || pin == 9 || pin == 14 || pin == 15;
    case 5:  // F
        return pin == 0 || pin == 1 || pin == 19 || pin == 22 || pin == 23;
    default: // E and any other bank: fully scannable
        return false;
    }
}

struct SpecialPad {
    char bank;
    uint8_t pin;
    uint32_t pad;
    uint32_t port;
    uint8_t domain;
};

// Always-on-domain candidates: an external-facing reset could plausibly sit on a
// spare battery/PMIC-domain pad. Excludes the fieldbus pins already in use and the
// pads unsafe to toggle blind (power UART, watchdog reset).
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

static_assert(kCandidateCount <= 0xFFU);
static_assert(kTelemetryBaseId + kCandidateCount - 1 < kBaselineId);
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

void send_frame(uint32_t std_id, const std::array<uint8_t, 8>& payload) {
    mcan_tx_frame_t frame{};
    frame.use_ext_id = false;
    frame.std_id = static_cast<uint16_t>(std_id);
    frame.rtr = false;
    frame.canfd_frame = false;
    frame.bitrate_switch = false;
    frame.dlc = 8;
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.data_8[i] = payload[i];
    }
    (void)mcan_transmit_via_txfifo_nonblocking(HPM_MCAN0, &frame, nullptr);
}

void send_identity() {
    send_frame(
        kIdentityId,
        {'R', 'S', 'T', kScannerVersion, static_cast<uint8_t>(kCandidateCount), 0, 0, 0});
}

void send_scan(const Candidate& candidate, size_t index, uint8_t stage, bool found, uint8_t addr) {
    send_frame(
        kTelemetryBaseId + index,
        {kScanMagic, static_cast<uint8_t>(index), static_cast<uint8_t>(candidate.bank),
         candidate.pin, stage, static_cast<uint8_t>(kCandidateCount),
         static_cast<uint8_t>(found ? 1U : 0U), addr});
}

void send_found(const Candidate& candidate, uint8_t addr, uint16_t id1, uint16_t id2) {
    send_frame(
        kFoundId, {kFoundMagic, static_cast<uint8_t>(candidate.bank), candidate.pin, addr,
                   static_cast<uint8_t>(id1 >> 8), static_cast<uint8_t>(id1),
                   static_cast<uint8_t>(id2 >> 8), static_cast<uint8_t>(id2)});
}

void configure_common_clocks() {
    clock_add_to_group(clock_gpio, 1);
    clock_add_to_group(clock_can0, 1);
    clock_add_to_group(clock_eth0, 1);
    clock_add_to_group(clock_esc0, 1);
    clock_add_to_group(clock_tsn1, 1);
    clock_add_to_group(clock_tsn2, 1);
    clock_add_to_group(clock_tsn3, 1);

    esc_core_enable_clock(HPM_ESC, true);
    esc_phy_enable_clock(HPM_ESC, true);
}

void configure_enet_mdio_pins() {
    constexpr uint32_t mdio_pad_ctl =
        IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1) | IOC_PAD_PAD_CTL_HYS_SET(1);

    HPM_IOC->PAD[IOC_PAD_PF00].FUNC_CTL = IOC_PF00_FUNC_CTL_ETH0_MDC;
    HPM_IOC->PAD[IOC_PAD_PF01].FUNC_CTL = IOC_PF01_FUNC_CTL_ETH0_MDIO;
    HPM_IOC->PAD[IOC_PAD_PF01].PAD_CTL = mdio_pad_ctl;
}

bool read_realtek(uint8_t& found_addr, uint16_t& id1_out, uint16_t& id2_out) {
    for (uint8_t addr = 0; addr < kMaxPhyAddr; ++addr) {
        uint16_t id1 = 0;
        uint16_t id2 = 0;
        if (enet_read_phy(HPM_ENET0, addr, kPhyIdReg1, &id1) != status_success) {
            continue;
        }
        if (enet_read_phy(HPM_ENET0, addr, kPhyIdReg2, &id2) != status_success) {
            continue;
        }
        if (id1 == kRealtekOuiMsb && id2 != 0x0000U && id2 != 0xFFFFU) {
            found_addr = addr;
            id1_out = id1;
            id2_out = id2;
            return true;
        }
    }
    return false;
}

void configure_candidate_gpio(const Candidate& candidate, uint8_t initial_level) {
    HPM_IOC->PAD[candidate.pad].FUNC_CTL = candidate.gpio_func;
    HPM_IOC->PAD[candidate.pad].PAD_CTL = 0;
    if (candidate.domain == kDomainPmic) {
        HPM_PIOC->PAD[candidate.pad].FUNC_CTL = kSocRoutingFunc;
    } else if (candidate.domain == kDomainBatt) {
        HPM_BIOC->PAD[candidate.pad].FUNC_CTL = kSocRoutingFunc;
    }
    gpiom_set_pin_controller(HPM_GPIOM, candidate.gpiom_assign, candidate.pin, gpiom_soc_gpio0);
    gpio_set_pin_output_with_initial(HPM_GPIO0, candidate.gpio_do_port, candidate.pin, initial_level);
}

void release_candidate_gpio(const Candidate& candidate) {
    gpio_set_pin_input(HPM_GPIO0, candidate.gpio_di_port, candidate.pin);
}

// Drive the candidate as an active-low reset: assert (low), release (high), let
// the PHY start, then read MDIO. Returns true (pad left latched high) on a hit.
bool scan_candidate(const Candidate& candidate, size_t index) {
    configure_candidate_gpio(candidate, 0);
    send_scan(candidate, index, kStageBegin, false, 0);
    board_delay_ms(kAssertLowMs);

    gpio_write_pin(HPM_GPIO0, candidate.gpio_do_port, candidate.pin, 1);
    board_delay_ms(kReleaseSettleMs);

    uint8_t addr = 0;
    uint16_t id1 = 0;
    uint16_t id2 = 0;
    const bool found = read_realtek(addr, id1, id2);
    send_scan(candidate, index, kStageResult, found, addr);
    if (found) {
        send_found(candidate, addr, id1, id2);
        return true;
    }

    release_candidate_gpio(candidate);
    board_delay_ms(kBetweenCandidatesMs);
    return false;
}

} // namespace

int realtek_reset_scanner_main() {
    (void)init_telemetry_can();
    configure_common_clocks();
    configure_enet_mdio_pins();

    while (true) {
        send_identity();

        // Baseline: is the Realtek already answering before any pad is driven?
        uint8_t addr = 0;
        uint16_t id1 = 0;
        uint16_t id2 = 0;
        const bool baseline = read_realtek(addr, id1, id2);
        send_frame(
            kBaselineId, {'B', 'A', 'S', 'E', static_cast<uint8_t>(baseline ? 1U : 0U), addr,
                          static_cast<uint8_t>(id1), static_cast<uint8_t>(id2)});

        for (size_t index = 0; index < kCandidateCount; ++index) {
            if (scan_candidate(kCandidates[index], index)) {
                // Reset pin found: hold it released and report forever so the
                // operator can read bank/pin off CAN 0x7F0 without racing the scan.
                const Candidate& hit = kCandidates[index];
                while (true) {
                    uint8_t confirm_addr = 0;
                    uint16_t confirm_id1 = 0;
                    uint16_t confirm_id2 = 0;
                    const bool still = read_realtek(confirm_addr, confirm_id1, confirm_id2);
                    send_found(
                        hit, still ? confirm_addr : addr, still ? confirm_id1 : id1,
                        still ? confirm_id2 : id2);
                    board_delay_ms(500);
                }
            }
        }

        board_delay_ms(1000);
    }

    return 0;
}

} // namespace librmcs::firmware::board
