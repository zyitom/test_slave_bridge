#include <cstddef>
#include <cstdint>

#include <board.h>
#include <hpm_clock_drv.h>
#include <hpm_iomux.h>
#include <hpm_mcan_drv.h>
#include <hpm_mcan_soc.h>
#include <hpm_soc.h>

namespace librmcs::firmware::board {
namespace {

constexpr uint32_t kHostProbeId = 0x123U;
constexpr uint32_t kResponseBaseId = 0x680U;
constexpr uint32_t kCanBaudrate = 1'000'000U;
constexpr uint32_t kCanClockDivider = 10U;
constexpr uint32_t kReceiveWindowMs = 120U;
constexpr uint32_t kResponseFrameCount = 4U;
constexpr size_t kPrimaryCandidateCount = 48U;
constexpr uint8_t kMagic = 0xC5U;
constexpr uint8_t kStageReceived = 'R';

struct Candidate {
    uintptr_t base;
    clock_name_t clock_name;
    uint8_t mcan_index;
    char tx_bank;
    uint8_t tx_pin;
    char rx_bank;
    uint8_t rx_pin;
    uint32_t tx_pad;
    uint32_t rx_pad;
    uint32_t tx_func;
    uint32_t rx_func;
    uint32_t tx_gpio_func;
    uint32_t rx_gpio_func;
};

#define IOC_PAD_NAME(bank, pin)        IOC_PAD_P##bank##pin
#define MCAN_TX_FUNC(bank, pin, index) IOC_P##bank##pin##_FUNC_CTL_MCAN##index##_TXD
#define MCAN_RX_FUNC(bank, pin, index) IOC_P##bank##pin##_FUNC_CTL_MCAN##index##_RXD
#define GPIO_FUNC(bank, pin)           IOC_P##bank##pin##_FUNC_CTL_GPIO_##bank##_##pin

#define CAN_CANDIDATE(bank, bank_char, index, tx_pin, rx_pin, tx_num, rx_num)                  \
    {                                                                                          \
        HPM_MCAN##index##_BASE, clock_can##index, index, bank_char, tx_num, bank_char, rx_num, \
            IOC_PAD_NAME(bank, tx_pin), IOC_PAD_NAME(bank, rx_pin),                            \
            MCAN_TX_FUNC(bank, tx_pin, index), MCAN_RX_FUNC(bank, rx_pin, index),              \
            GPIO_FUNC(bank, tx_pin), GPIO_FUNC(bank, rx_pin),                                  \
    }

#define BANK_CAN_CANDIDATES(bank, bank_char)               \
    CAN_CANDIDATE(bank, bank_char, 0, 00, 01, 0, 1),       \
        CAN_CANDIDATE(bank, bank_char, 1, 05, 04, 5, 4),   \
        CAN_CANDIDATE(bank, bank_char, 2, 08, 09, 8, 9),   \
        CAN_CANDIDATE(bank, bank_char, 3, 15, 14, 15, 14), \
        CAN_CANDIDATE(bank, bank_char, 4, 16, 17, 16, 17), \
        CAN_CANDIDATE(bank, bank_char, 5, 21, 20, 21, 20), \
        CAN_CANDIDATE(bank, bank_char, 6, 24, 25, 24, 25), \
        CAN_CANDIDATE(bank, bank_char, 7, 31, 30, 31, 30)

#define BANK_CAN0123_CANDIDATES(bank, bank_char)         \
    CAN_CANDIDATE(bank, bank_char, 0, 00, 01, 0, 1),     \
        CAN_CANDIDATE(bank, bank_char, 1, 05, 04, 5, 4), \
        CAN_CANDIDATE(bank, bank_char, 2, 08, 09, 8, 9), \
        CAN_CANDIDATE(bank, bank_char, 3, 15, 14, 15, 14)

#define BANK_CAN012345_CANDIDATES(bank, bank_char)         \
    CAN_CANDIDATE(bank, bank_char, 0, 00, 01, 0, 1),       \
        CAN_CANDIDATE(bank, bank_char, 1, 05, 04, 5, 4),   \
        CAN_CANDIDATE(bank, bank_char, 2, 08, 09, 8, 9),   \
        CAN_CANDIDATE(bank, bank_char, 3, 15, 14, 15, 14), \
        CAN_CANDIDATE(bank, bank_char, 4, 16, 17, 16, 17), \
        CAN_CANDIDATE(bank, bank_char, 5, 21, 20, 21, 20)

#define BANK_CAN45_CANDIDATES(bank, bank_char) \
    CAN_CANDIDATE(bank, bank_char, 4, 00, 01, 0, 1), CAN_CANDIDATE(bank, bank_char, 5, 05, 04, 5, 4)

#define BANK_CAN01_CANDIDATES(bank, bank_char) \
    CAN_CANDIDATE(bank, bank_char, 0, 00, 01, 0, 1), CAN_CANDIDATE(bank, bank_char, 1, 05, 04, 5, 4)

constexpr Candidate kCandidates[] = {
    BANK_CAN_CANDIDATES(A, 'A'),     BANK_CAN_CANDIDATES(B, 'B'),
    BANK_CAN_CANDIDATES(C, 'C'),     BANK_CAN_CANDIDATES(D, 'D'),
    BANK_CAN_CANDIDATES(E, 'E'),     BANK_CAN_CANDIDATES(F, 'F'),
    BANK_CAN0123_CANDIDATES(V, 'V'), BANK_CAN012345_CANDIDATES(W, 'W'),
    BANK_CAN45_CANDIDATES(X, 'X'),   BANK_CAN01_CANDIDATES(Y, 'Y'),
    BANK_CAN45_CANDIDATES(Z, 'Z'),
};

#undef BANK_CAN01_CANDIDATES
#undef BANK_CAN45_CANDIDATES
#undef BANK_CAN012345_CANDIDATES
#undef BANK_CAN0123_CANDIDATES
#undef BANK_CAN_CANDIDATES
#undef CAN_CANDIDATE
#undef GPIO_FUNC
#undef MCAN_RX_FUNC
#undef MCAN_TX_FUNC
#undef IOC_PAD_NAME

constexpr size_t kCandidateCount = sizeof(kCandidates) / sizeof(kCandidates[0]);
static_assert(kCandidateCount == 64U);
static_assert(kPrimaryCandidateCount <= kCandidateCount);
constexpr uint32_t kRamSliceSize = MCAN_MSG_BUF_SIZE_IN_WORDS * sizeof(uint32_t);
static_assert(8U * kRamSliceSize <= MCAN_MSG_BUF_BASE_VALID_END - MCAN_MSG_BUF_BASE_VALID_START);

MCAN_Type* can_base(const Candidate& candidate) {
    return reinterpret_cast<MCAN_Type*>(candidate.base);
}

void configure_candidate_pads(const Candidate& candidate) {
    constexpr uint32_t rx_pad_ctl =
        IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1) | IOC_PAD_PAD_CTL_HYS_SET(1);

    HPM_IOC->PAD[candidate.tx_pad].FUNC_CTL = candidate.tx_func;
    HPM_IOC->PAD[candidate.rx_pad].FUNC_CTL = candidate.rx_func;
    HPM_IOC->PAD[candidate.rx_pad].PAD_CTL = rx_pad_ctl;
}

void restore_candidate_pads(const Candidate& candidate) {
    HPM_IOC->PAD[candidate.tx_pad].FUNC_CTL = candidate.tx_gpio_func;
    HPM_IOC->PAD[candidate.rx_pad].FUNC_CTL = candidate.rx_gpio_func;
    HPM_IOC->PAD[candidate.tx_pad].PAD_CTL = 0;
    HPM_IOC->PAD[candidate.rx_pad].PAD_CTL = 0;
}

hpm_stat_t init_candidate_can(const Candidate& candidate) {
    MCAN_Type* base = can_base(candidate);

    clock_add_to_group(candidate.clock_name, 1);
    clock_set_source_divider(candidate.clock_name, clk_src_pll1_clk0, kCanClockDivider);

    mcan_deinit(base);

    const mcan_msg_buf_attr_t attr = {
        .ram_base = MCAN_MSG_BUF_BASE_VALID_START + candidate.mcan_index * kRamSliceSize,
        .ram_size = kRamSliceSize,
    };
    hpm_stat_t status = mcan_set_msg_buf_attr(base, &attr);
    if (status != status_success) {
        return status;
    }

    mcan_config_t config;
    mcan_get_default_config(base, &config);
    config.baudrate = kCanBaudrate;
    config.mode = mcan_mode_normal;
    config.enable_canfd = false;
    config.disable_auto_retransmission = true;
    config.ram_config.txbuf_dedicated_txbuf_elem_count = 0;
    config.ram_config.txbuf_fifo_or_queue_elem_count = MCAN_TXBUF_SIZE_CAN_DEFAULT;
    config.ram_config.txfifo_or_txqueue_mode = MCAN_TXBUF_OPERATION_MODE_FIFO;

    return mcan_init(base, &config, clock_get_frequency(candidate.clock_name));
}

void fill_payload(
    mcan_tx_frame_t& frame, const Candidate& candidate, size_t candidate_index, uint8_t stage) {
    frame.dlc = 8;
    frame.data_8[0] = kMagic;
    frame.data_8[1] = static_cast<uint8_t>(candidate_index);
    frame.data_8[2] = candidate.mcan_index;
    frame.data_8[3] = static_cast<uint8_t>(candidate.tx_bank);
    frame.data_8[4] = candidate.tx_pin;
    frame.data_8[5] = static_cast<uint8_t>(candidate.rx_bank);
    frame.data_8[6] = candidate.rx_pin;
    frame.data_8[7] = stage;
}

void send_candidate_response(const Candidate& candidate, size_t candidate_index) {
    mcan_tx_frame_t frame{};
    frame.use_ext_id = false;
    frame.std_id = static_cast<uint16_t>(kResponseBaseId + candidate_index);
    frame.rtr = false;
    frame.canfd_frame = false;
    frame.bitrate_switch = false;
    fill_payload(frame, candidate, candidate_index, kStageReceived);
    (void)mcan_transmit_via_txfifo_nonblocking(can_base(candidate), &frame, nullptr);
}

void drain_rx_fifo(MCAN_Type* base) {
    mcan_rx_message_t rx{};
    while (mcan_read_rxfifo(base, 0, &rx) == status_success) {}
}

bool wait_for_host_probe(const Candidate& candidate) {
    MCAN_Type* base = can_base(candidate);

    for (uint32_t elapsed_ms = 0; elapsed_ms < kReceiveWindowMs; elapsed_ms += 5U) {
        mcan_rx_message_t rx{};
        while (mcan_read_rxfifo(base, 0, &rx) == status_success) {
            if (!rx.use_ext_id && rx.std_id == kHostProbeId) {
                return true;
            }
        }
        board_delay_ms(5);
    }

    return false;
}

void scan_candidate(const Candidate& candidate, size_t candidate_index) {
    configure_candidate_pads(candidate);

    if (init_candidate_can(candidate) != status_success) {
        restore_candidate_pads(candidate);
        return;
    }

    MCAN_Type* base = can_base(candidate);
    drain_rx_fifo(base);

    if (wait_for_host_probe(candidate)) {
        for (uint32_t frame_index = 0; frame_index < kResponseFrameCount; ++frame_index) {
            send_candidate_response(candidate, candidate_index);
            board_delay_ms(20);
        }
    }

    mcan_deinit(base);
    restore_candidate_pads(candidate);
}

} // namespace

int can_pin_scanner_main() {
    while (true) {
        for (size_t candidate_index = 0; candidate_index < kCandidateCount; ++candidate_index) {
            if (candidate_index == kPrimaryCandidateCount) {
                board_delay_ms(1000);
            }
            scan_candidate(kCandidates[candidate_index], candidate_index);
            board_delay_ms(20);
        }
    }

    return 0;
}

} // namespace librmcs::firmware::board
