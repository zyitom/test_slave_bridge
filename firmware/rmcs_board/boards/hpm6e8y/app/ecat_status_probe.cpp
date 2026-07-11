#include <array>
#include <cstddef>
#include <cstdint>

#include <board.h>
#include <hpm_clock_drv.h>
#include <hpm_mcan_drv.h>
#include <hpm_mcan_soc.h>
#include <hpm_soc.h>

// Answers the first question left after "No EtherCAT slave found": is the ESC
// datalink not receiving frames because the on-die PHY <-> ESC MII pin mapping is
// wrong (a hardware/pin fault), or because of an ESC configuration/software issue?
//
// core0 runs the full ESC bring-up (this image does NOT disable it); core1 just
// reads the standard ESC register file, memory-mapped at HPM_ESC_BASE, and reports
// the decisive registers over CAN0. The key one is DL Status (0x0110): bit4/bit5
// are "physical link on port 0/1". If the ESC sees link there, its MII RX path is
// fed correctly and the problem is above the pins; if not, the PV/PW MII pins (or
// the on-die PHY forwarding) are the fault.

namespace librmcs::firmware::board {
namespace {

constexpr uint32_t kIdentityId = 0x77FU;
constexpr uint32_t kDlAlId = 0x780U;
constexpr uint32_t kErrId = 0x781U;
constexpr uint32_t kGprId = 0x782U;
constexpr uint32_t kPhyBasicBaseId = 0x783U;
constexpr uint32_t kPhyIdBaseId = 0x785U;
constexpr uint32_t kEscConfigId = 0x787U;
constexpr uint32_t kEscPhyConfigId = 0x788U;
constexpr uint32_t kEscGprStatusId = 0x789U;
constexpr uint32_t kPhyRmsrBaseId = 0x78AU;
constexpr uint32_t kCanBaudrate = 1'000'000U;
constexpr uint32_t kCanClockDivider = 10U;
constexpr uint32_t kStartupDelayMs = 1200U;
constexpr uint32_t kReportPeriodMs = 500U;
constexpr uint8_t kProbeVersion = 4U;

// Standard EtherCAT ESC register offsets (ET1100-compatible register file).
constexpr uint32_t kEscDlControl = 0x0100U;
constexpr uint32_t kEscDlStatus = 0x0110U;
constexpr uint32_t kEscAlControl = 0x0120U;
constexpr uint32_t kEscAlStatus = 0x0130U;
constexpr uint32_t kEscRxErrorPort0 = 0x0300U;
constexpr uint32_t kEscRxErrorPort1 = 0x0302U;
constexpr uint32_t kEscPuErrorCounter = 0x030CU;
constexpr uint32_t kEscPdiErrorCounter = 0x030DU;

constexpr uint32_t kTelemetryRamSize = MCAN_MSG_BUF_SIZE_IN_WORDS * sizeof(uint32_t);
constexpr uint32_t kTelemetryRamBase = MCAN_MSG_BUF_BASE_VALID_START + 7U * kTelemetryRamSize;
static_assert(
    8U * kTelemetryRamSize <= MCAN_MSG_BUF_BASE_VALID_END - MCAN_MSG_BUF_BASE_VALID_START);

uint16_t esc_read16(uint32_t offset) {
    return reinterpret_cast<volatile uint16_t*>(HPM_ESC_BASE)[offset >> 1U];
}

uint8_t esc_read8(uint32_t offset) {
    return reinterpret_cast<volatile uint8_t*>(HPM_ESC_BASE)[offset];
}

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
        .ram_base = kTelemetryRamBase,
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

uint8_t phy_flags(bool read_ok, bool link_up) {
    return static_cast<uint8_t>((read_ok ? 0x01U : 0x00U) | (link_up ? 0x02U : 0x00U));
}

void send_phy_frames(
    uint8_t port, bool read_ok, bool link_up, uint16_t bmcr, uint16_t bmsr, uint16_t id1,
    uint16_t id2, uint16_t rmsr_p7) {
    const uint8_t flags = phy_flags(read_ok, link_up);
    send_frame(
        kPhyBasicBaseId + port,
        {'P', 'B', port, flags, static_cast<uint8_t>(bmcr >> 8), static_cast<uint8_t>(bmcr),
         static_cast<uint8_t>(bmsr >> 8), static_cast<uint8_t>(bmsr)});
    send_frame(
        kPhyIdBaseId + port,
        {'P', 'I', port, flags, static_cast<uint8_t>(id1 >> 8), static_cast<uint8_t>(id1),
         static_cast<uint8_t>(id2 >> 8), static_cast<uint8_t>(id2)});
    send_frame(
        kPhyRmsrBaseId + port, {'P', '7', port, flags, static_cast<uint8_t>(rmsr_p7 >> 8),
                                static_cast<uint8_t>(rmsr_p7), 0, 0});
}

void send_esc_config_frames() {
    const uint16_t feature = HPM_ESC->FEATURE;
    send_frame(
        kEscConfigId,
        {'C', '0', HPM_ESC->TYPE, HPM_ESC->REVISION, HPM_ESC->PORT_DESC, HPM_ESC->PDI_CTRL,
         static_cast<uint8_t>(feature), static_cast<uint8_t>(feature >> 8)});

    const uint32_t phy_cfg0 = HPM_ESC->PHY_CFG0;
    const uint32_t phy_cfg1 = HPM_ESC->PHY_CFG1;
    send_frame(
        kEscPhyConfigId,
        {'C', '1', static_cast<uint8_t>(phy_cfg0), static_cast<uint8_t>(phy_cfg0 >> 8),
         static_cast<uint8_t>(phy_cfg0 >> 16), static_cast<uint8_t>(phy_cfg0 >> 24),
         static_cast<uint8_t>(phy_cfg1), static_cast<uint8_t>(phy_cfg1 >> 8)});

    const uint32_t gpr_status = HPM_ESC->GPR_STATUS;
    send_frame(
        kEscGprStatusId,
        {'G', 'S', static_cast<uint8_t>(gpr_status), static_cast<uint8_t>(gpr_status >> 8),
         static_cast<uint8_t>(gpr_status >> 16), static_cast<uint8_t>(gpr_status >> 24),
         static_cast<uint8_t>(HPM_ESC->IO_CFG[0]), static_cast<uint8_t>(HPM_ESC->IO_CFG[1])});
}

void send_status_report() {
    send_frame(kIdentityId, {'E', 'S', 'T', 'A', kProbeVersion, 0, 0, 0});

    board_ecat_phy_status_t phy_status{};
    if (board_ecat_get_internal_phy_status(&phy_status)) {
#if defined(BOARD_ECAT_SWAP_PHY_LINK_TO_ESC_PORT) && BOARD_ECAT_SWAP_PHY_LINK_TO_ESC_PORT
        board_ecat_set_internal_phy_link(phy_status.port1_link_up, phy_status.port0_link_up);
#else
        board_ecat_set_internal_phy_link(phy_status.port0_link_up, phy_status.port1_link_up);
#endif
    }

    send_esc_config_frames();

    const uint32_t gpr_cfg2 = HPM_ESC->GPR_CFG2;
    const uint16_t mii_mng_cs = static_cast<uint16_t>(HPM_ESC->MII_MNG_CS);
    send_frame(
        kGprId, {'G', 'P', static_cast<uint8_t>(gpr_cfg2), static_cast<uint8_t>(gpr_cfg2 >> 8),
                 static_cast<uint8_t>(gpr_cfg2 >> 16), static_cast<uint8_t>(gpr_cfg2 >> 24),
                 static_cast<uint8_t>(mii_mng_cs), static_cast<uint8_t>(mii_mng_cs >> 8)});
    send_phy_frames(
        0, phy_status.port0_read_ok, phy_status.port0_link_up, phy_status.port0_bmcr,
        phy_status.port0_bmsr, phy_status.port0_id1, phy_status.port0_id2,
        phy_status.port0_rmsr_p7);
    send_phy_frames(
        1, phy_status.port1_read_ok, phy_status.port1_link_up, phy_status.port1_bmcr,
        phy_status.port1_bmsr, phy_status.port1_id1, phy_status.port1_id2,
        phy_status.port1_rmsr_p7);

    const uint16_t dl_control = esc_read16(kEscDlControl);
    const uint16_t dl_status = esc_read16(kEscDlStatus);
    const uint16_t al_control = esc_read16(kEscAlControl);
    const uint16_t al_status = esc_read16(kEscAlStatus);

    send_frame(
        kDlAlId, {'E', 'S', static_cast<uint8_t>(dl_status), static_cast<uint8_t>(dl_status >> 8),
                  static_cast<uint8_t>(al_status), static_cast<uint8_t>(al_status >> 8),
                  static_cast<uint8_t>(al_control), static_cast<uint8_t>(dl_control)});

    const uint16_t rx_err_p0 = esc_read16(kEscRxErrorPort0);
    const uint16_t rx_err_p1 = esc_read16(kEscRxErrorPort1);
    const uint8_t pu_err = esc_read8(kEscPuErrorCounter);
    const uint8_t pdi_err = esc_read8(kEscPdiErrorCounter);

    send_frame(
        kErrId,
        {'E', 'R', static_cast<uint8_t>(rx_err_p0), static_cast<uint8_t>(rx_err_p0 >> 8),
         static_cast<uint8_t>(rx_err_p1), static_cast<uint8_t>(rx_err_p1 >> 8), pu_err, pdi_err});
}

} // namespace

void ecat_status_telemetry_init() { (void)init_telemetry_can(); }

void ecat_status_telemetry_task(uint32_t now_ms) {
    static bool started = false;
    static uint32_t last_report_ms = 0;

    if (!started) {
        if (now_ms < kStartupDelayMs) {
            return;
        }
        started = true;
        last_report_ms = now_ms;
        send_status_report();
        return;
    }

    if (static_cast<uint32_t>(now_ms - last_report_ms) < kReportPeriodMs) {
        return;
    }

    last_report_ms = now_ms;
    send_status_report();
}

int ecat_status_probe_main() {
    ecat_status_telemetry_init();

    // Let core0 finish board_init_ethercat + ecat_hardware_init before reading.
    board_delay_ms(kStartupDelayMs);

    while (true) {
        send_status_report();
        board_delay_ms(kReportPeriodMs);
    }

    return 0;
}

} // namespace librmcs::firmware::board
