#include <array>
#include <cstddef>
#include <cstdint>

#include <board.h>
#include <hpm_clock_drv.h>
#include <hpm_common.h>
#include <hpm_enet_drv.h>
#include <hpm_esc_drv.h>
#include <hpm_gpio_drv.h>
#include <hpm_gpiom_drv.h>
#include <hpm_gpiom_soc_drv.h>
#include <hpm_iomux.h>
#include <hpm_mcan_drv.h>
#include <hpm_mcan_soc.h>
#include <hpm_soc.h>
#include <hpm_tsw_drv.h>

namespace librmcs::firmware::board {
namespace {

constexpr uint32_t kInfoId = 0x72FU;
constexpr uint32_t kRawBaseId = 0x500U;
constexpr uint32_t kBasicBaseId = 0x580U;
constexpr uint32_t kStatusBaseId = 0x730U;
constexpr uint32_t kHitBaseId = 0x600U;
constexpr uint32_t kCanBaudrate = 1'000'000U;
constexpr uint32_t kCanClockDivider = 10U;
constexpr uint32_t kMdioClockDivider = 19U;
constexpr uint32_t kResetSettleMs = 120U;
constexpr uint32_t kScanPeriodMs = 1500U;
constexpr uint8_t kProbeVersion = 8U;
constexpr uint8_t kProbeBusCount = 4U;
constexpr uint8_t kMagic = 'M';
constexpr uint8_t kRawMagic = 'R';
constexpr uint8_t kStageStart = 'S';
constexpr uint8_t kStageDone = 'D';
constexpr uint8_t kStageError = 'E';

enum class MdioAccess : uint8_t {
    kEscMii,
    kEnetSmi,
    kTswPort,
};

struct ProbeBus {
    uint8_t bus_index;
    uint8_t physical_port;
    uint8_t reset_level;
    MdioAccess access;
    uint8_t tsw_port;
    void (*configure_pins)();
};

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

void send_probe_info() {
    send_frame(kInfoId, {kMagic, 'D', 'I', 'O', kProbeVersion, kProbeBusCount, 'R', 'S'});
}

void send_status(const ProbeBus& bus, uint8_t stage, uint8_t hit_count, uint8_t error_count) {
    uint8_t access = 'E';
    if (bus.access == MdioAccess::kEscMii) {
        access = 'C';
    } else if (bus.access == MdioAccess::kTswPort) {
        access = 'T';
    }

    send_frame(
        kStatusBaseId + bus.bus_index, {kMagic, bus.bus_index, bus.physical_port, stage, hit_count,
                                        error_count, bus.reset_level, access});
}

void send_hit(const ProbeBus& bus, uint8_t phy_addr, uint16_t id1, uint16_t id2) {
    send_frame(
        kHitBaseId + bus.bus_index * 32U + phy_addr,
        {kMagic, bus.bus_index, bus.physical_port, phy_addr, static_cast<uint8_t>(id1 >> 8),
         static_cast<uint8_t>(id1), static_cast<uint8_t>(id2 >> 8), static_cast<uint8_t>(id2)});
}

void send_raw(const ProbeBus& bus, uint8_t phy_addr, bool ok, uint16_t id1, uint16_t id2) {
    send_frame(
        kRawBaseId + bus.bus_index * 32U + phy_addr,
        {kRawMagic, bus.bus_index, phy_addr, static_cast<uint8_t>(ok ? 1U : 0U),
         static_cast<uint8_t>(id1 >> 8), static_cast<uint8_t>(id1), static_cast<uint8_t>(id2 >> 8),
         static_cast<uint8_t>(id2)});
}

void send_basic(const ProbeBus& bus, uint8_t phy_addr, bool ok, uint16_t bmcr, uint16_t bmsr) {
    send_frame(
        kBasicBaseId + bus.bus_index * 32U + phy_addr,
        {kRawMagic, bus.bus_index, phy_addr, static_cast<uint8_t>(ok ? 1U : 0U),
         static_cast<uint8_t>(bmcr >> 8), static_cast<uint8_t>(bmcr),
         static_cast<uint8_t>(bmsr >> 8), static_cast<uint8_t>(bmsr)});
}

bool is_valid_phy_id(uint16_t id1, uint16_t id2) {
    return id1 != 0U && id1 != 0xFFFFU && id2 != 0U && id2 != 0xFFFFU;
}

void configure_common_clocks() {
    clock_add_to_group(clock_gpio, 1);
    clock_add_to_group(clock_eth0, 1);
    clock_add_to_group(clock_esc0, 1);
    clock_add_to_group(clock_tsn1, 1);
    clock_add_to_group(clock_tsn2, 1);
    clock_add_to_group(clock_tsn3, 1);

    esc_core_enable_clock(HPM_ESC, true);
    esc_phy_enable_clock(HPM_ESC, true);
}

void configure_internal_phy_analog_pads() {
    constexpr uint32_t strap_pullup_ctl = IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1);

    HPM_IOC->PAD[IOC_PAD_PA16].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA16].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PA17].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA17].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PA18].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA18].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PA19].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA19].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PA20].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA20].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PA21].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA21].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PA22].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA22].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PA23].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA23].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PA24].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA24].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PA26].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA26].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PA27].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA27].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PA29].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PA29].PAD_CTL = 0;

    HPM_IOC->PAD[IOC_PAD_PW16].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PW16].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PW17].FUNC_CTL = IOC_PAD_FUNC_CTL_ANALOG_MASK;
    HPM_IOC->PAD[IOC_PAD_PW17].PAD_CTL = 0;

    HPM_IOC->PAD[IOC_PAD_PA25].FUNC_CTL = IOC_PA25_FUNC_CTL_ESC0_CTR_0;
    HPM_IOC->PAD[IOC_PAD_PA25].PAD_CTL = strap_pullup_ctl;
    HPM_IOC->PAD[IOC_PAD_PA28].FUNC_CTL = IOC_PA28_FUNC_CTL_ESC0_CTR_1;
    HPM_IOC->PAD[IOC_PAD_PA28].PAD_CTL = strap_pullup_ctl;
}

void configure_internal_phy_ref_clocks() {
    HPM_IOC->PAD[IOC_PAD_PW20].FUNC_CTL = IOC_PW20_FUNC_CTL_ESC0_REFCK;
    HPM_IOC->PAD[IOC_PAD_PW21].FUNC_CTL = IOC_PW21_FUNC_CTL_ESC0_REFCK;
}

void configure_internal_esc_mii_pins() {
    HPM_IOC->PAD[IOC_PAD_PV00].FUNC_CTL = IOC_PV00_FUNC_CTL_ESC0_P0_RXDV;
    HPM_IOC->PAD[IOC_PAD_PV01].FUNC_CTL = IOC_PV01_FUNC_CTL_ESC0_P0_RXD_0;
    HPM_IOC->PAD[IOC_PAD_PV02].FUNC_CTL = IOC_PV02_FUNC_CTL_ESC0_P0_RXD_1;
    HPM_IOC->PAD[IOC_PAD_PV03].FUNC_CTL = IOC_PV03_FUNC_CTL_ESC0_P0_RXD_2;
    HPM_IOC->PAD[IOC_PAD_PV04].FUNC_CTL = IOC_PV04_FUNC_CTL_ESC0_P0_RXD_3;
    HPM_IOC->PAD[IOC_PAD_PV05].FUNC_CTL = IOC_PV05_FUNC_CTL_ESC0_P0_RXCK;
    HPM_IOC->PAD[IOC_PAD_PV06].FUNC_CTL = IOC_PV06_FUNC_CTL_ESC0_P0_TXCK;
    HPM_IOC->PAD[IOC_PAD_PV07].FUNC_CTL = IOC_PV07_FUNC_CTL_ESC0_P0_TXD_0;
    HPM_IOC->PAD[IOC_PAD_PV08].FUNC_CTL = IOC_PV08_FUNC_CTL_ESC0_P0_TXD_1;
    HPM_IOC->PAD[IOC_PAD_PV09].FUNC_CTL = IOC_PV09_FUNC_CTL_ESC0_P0_TXD_2;
    HPM_IOC->PAD[IOC_PAD_PV10].FUNC_CTL = IOC_PV10_FUNC_CTL_ESC0_P0_TXD_3;
    HPM_IOC->PAD[IOC_PAD_PV11].FUNC_CTL = IOC_PV11_FUNC_CTL_ESC0_P0_TXEN;
    HPM_IOC->PAD[IOC_PAD_PV15].FUNC_CTL = IOC_PV15_FUNC_CTL_ESC0_P0_RXER;

    HPM_IOC->PAD[IOC_PAD_PW00].FUNC_CTL = IOC_PW00_FUNC_CTL_ESC0_P1_RXDV;
    HPM_IOC->PAD[IOC_PAD_PW01].FUNC_CTL = IOC_PW01_FUNC_CTL_ESC0_P1_RXD_0;
    HPM_IOC->PAD[IOC_PAD_PW02].FUNC_CTL = IOC_PW02_FUNC_CTL_ESC0_P1_RXD_1;
    HPM_IOC->PAD[IOC_PAD_PW03].FUNC_CTL = IOC_PW03_FUNC_CTL_ESC0_P1_RXD_2;
    HPM_IOC->PAD[IOC_PAD_PW04].FUNC_CTL = IOC_PW04_FUNC_CTL_ESC0_P1_RXD_3;
    HPM_IOC->PAD[IOC_PAD_PW05].FUNC_CTL = IOC_PW05_FUNC_CTL_ESC0_P1_RXCK;
    HPM_IOC->PAD[IOC_PAD_PW06].FUNC_CTL = IOC_PW06_FUNC_CTL_ESC0_P1_TXCK;
    HPM_IOC->PAD[IOC_PAD_PW07].FUNC_CTL = IOC_PW07_FUNC_CTL_ESC0_P1_TXD_0;
    HPM_IOC->PAD[IOC_PAD_PW08].FUNC_CTL = IOC_PW08_FUNC_CTL_ESC0_P1_TXD_1;
    HPM_IOC->PAD[IOC_PAD_PW09].FUNC_CTL = IOC_PW09_FUNC_CTL_ESC0_P1_TXD_2;
    HPM_IOC->PAD[IOC_PAD_PW10].FUNC_CTL = IOC_PW10_FUNC_CTL_ESC0_P1_TXD_3;
    HPM_IOC->PAD[IOC_PAD_PW11].FUNC_CTL = IOC_PW11_FUNC_CTL_ESC0_P1_TXEN;
    HPM_IOC->PAD[IOC_PAD_PW15].FUNC_CTL = IOC_PW15_FUNC_CTL_ESC0_P1_RXER;
}

void drive_internal_phy_resets(uint8_t reset_level) {
    HPM_IOC->PAD[IOC_PAD_PV12].FUNC_CTL = IOC_PV12_FUNC_CTL_GPIO_V_12;
    HPM_IOC->PAD[IOC_PAD_PW12].FUNC_CTL = IOC_PW12_FUNC_CTL_GPIO_W_12;
    HPM_IOC->PAD[IOC_PAD_PV12].PAD_CTL = 0;
    HPM_IOC->PAD[IOC_PAD_PW12].PAD_CTL = 0;

    gpiom_set_pin_controller(HPM_GPIOM, GPIOM_ASSIGN_GPIOV, 12, gpiom_soc_gpio0);
    gpiom_set_pin_controller(HPM_GPIOM, GPIOM_ASSIGN_GPIOW, 12, gpiom_soc_gpio0);
    gpio_set_pin_output_with_initial(HPM_GPIO0, GPIO_DO_GPIOV, 12, reset_level);
    gpio_set_pin_output_with_initial(HPM_GPIO0, GPIO_DO_GPIOW, 12, reset_level);
}

void release_internal_phys() {
    configure_internal_phy_analog_pads();
    configure_internal_esc_mii_pins();
    configure_internal_phy_ref_clocks();
    esc_core_enable_clock(HPM_ESC, true);
    esc_phy_enable_clock(HPM_ESC, true);

    drive_internal_phy_resets(0);
    board_delay_ms(20);
    drive_internal_phy_resets(1);
    board_delay_ms(kResetSettleMs);
}

void configure_internal_esc_mdio_pins() {
    constexpr uint32_t mdio_pad_ctl =
        IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1) | IOC_PAD_PAD_CTL_HYS_SET(1);

    static bool initialized = false;
    if (!initialized) {
        release_internal_phys();
        initialized = true;
    } else {
        configure_internal_phy_analog_pads();
        configure_internal_esc_mii_pins();
        configure_internal_phy_ref_clocks();
    }

    HPM_IOC->PAD[IOC_PAD_PA30].FUNC_CTL = IOC_PA30_FUNC_CTL_ESC0_MDIO;
    HPM_IOC->PAD[IOC_PAD_PA31].FUNC_CTL = IOC_PA31_FUNC_CTL_ESC0_MDC;
    HPM_IOC->PAD[IOC_PAD_PA30].PAD_CTL = mdio_pad_ctl;
    esc_enable_pdi_access_mii_management(HPM_ESC);
}

void configure_internal_enet_mdio_pins() {
    constexpr uint32_t mdio_pad_ctl =
        IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1) | IOC_PAD_PAD_CTL_HYS_SET(1);

    configure_internal_phy_analog_pads();
    configure_internal_esc_mii_pins();
    configure_internal_phy_ref_clocks();
    HPM_IOC->PAD[IOC_PAD_PA30].FUNC_CTL = IOC_PA30_FUNC_CTL_ETH0_MDIO;
    HPM_IOC->PAD[IOC_PAD_PA31].FUNC_CTL = IOC_PA31_FUNC_CTL_ETH0_MDC;
    HPM_IOC->PAD[IOC_PAD_PA30].PAD_CTL = mdio_pad_ctl;
}

void configure_pf_enet_mdio_pins() {
    constexpr uint32_t mdio_pad_ctl =
        IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1) | IOC_PAD_PAD_CTL_HYS_SET(1);

    configure_internal_phy_analog_pads();
    configure_internal_esc_mii_pins();
    configure_internal_phy_ref_clocks();
    HPM_IOC->PAD[IOC_PAD_PF00].FUNC_CTL = IOC_PF00_FUNC_CTL_ETH0_MDC;
    HPM_IOC->PAD[IOC_PAD_PF01].FUNC_CTL = IOC_PF01_FUNC_CTL_ETH0_MDIO;
    HPM_IOC->PAD[IOC_PAD_PF01].PAD_CTL = mdio_pad_ctl;
}

void configure_internal_tsw_mdio_pins() {
    constexpr uint32_t mdio_pad_ctl =
        IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1) | IOC_PAD_PAD_CTL_HYS_SET(1);

    configure_internal_phy_analog_pads();
    configure_internal_esc_mii_pins();
    configure_internal_phy_ref_clocks();
    HPM_IOC->PAD[IOC_PAD_PA30].FUNC_CTL = IOC_PA30_FUNC_CTL_TSW0_P1_MDC;
    HPM_IOC->PAD[IOC_PAD_PA31].FUNC_CTL = IOC_PA31_FUNC_CTL_TSW0_P1_MDIO;
    HPM_IOC->PAD[IOC_PAD_PA31].PAD_CTL = mdio_pad_ctl;
}

bool read_phy_reg(const ProbeBus& bus, uint8_t phy_addr, uint8_t reg_addr, uint16_t& value) {
    hpm_stat_t status = status_success;
    if (bus.access == MdioAccess::kEscMii) {
        status = esc_mdio_read(HPM_ESC, phy_addr, reg_addr, &value);
    } else if (bus.access == MdioAccess::kTswPort) {
        status = tsw_ep_mdio_read(HPM_TSW, bus.tsw_port, phy_addr, reg_addr, &value);
    } else {
        status = enet_read_phy(HPM_ENET0, phy_addr, reg_addr, &value);
    }
    return status == status_success;
}

bool read_phy_pair(
    const ProbeBus& bus, uint8_t phy_addr, uint8_t reg_a, uint8_t reg_b, uint16_t& value_a,
    uint16_t& value_b) {
    if (!read_phy_reg(bus, phy_addr, reg_a, value_a)) {
        return false;
    }
    return read_phy_reg(bus, phy_addr, reg_b, value_b);
}

bool read_phy_id(const ProbeBus& bus, uint8_t phy_addr, uint16_t& id1, uint16_t& id2) {
    return read_phy_pair(bus, phy_addr, 2U, 3U, id1, id2);
}

bool read_phy_basic(const ProbeBus& bus, uint8_t phy_addr, uint16_t& bmcr, uint16_t& bmsr) {
    return read_phy_pair(bus, phy_addr, 0U, 1U, bmcr, bmsr);
}

void scan_bus(const ProbeBus& bus) {
    bus.configure_pins();
    if (bus.access == MdioAccess::kTswPort) {
        tsw_ep_set_mdio_config(HPM_TSW, bus.tsw_port, kMdioClockDivider);
    } else if (bus.access == MdioAccess::kEscMii) {
        esc_enable_pdi_access_mii_management(HPM_ESC);
    }
    send_status(bus, kStageStart, 0, 0);

    uint8_t hit_count = 0;
    uint8_t error_count = 0;
    for (uint8_t phy_addr = 0; phy_addr < 32U; ++phy_addr) {
        uint16_t bmcr = 0;
        uint16_t bmsr = 0;
        bool basic_ok = read_phy_basic(bus, phy_addr, bmcr, bmsr);
        send_basic(bus, phy_addr, basic_ok, bmcr, bmsr);
        if (!basic_ok) {
            ++error_count;
        }

        uint16_t id1 = 0;
        uint16_t id2 = 0;
        if (!read_phy_id(bus, phy_addr, id1, id2)) {
            send_raw(bus, phy_addr, false, id1, id2);
            ++error_count;
            continue;
        }
        send_raw(bus, phy_addr, true, id1, id2);
        if (is_valid_phy_id(id1, id2)) {
            ++hit_count;
            send_hit(bus, phy_addr, id1, id2);
            board_delay_ms(5);
        }
    }

    if (error_count != 0U) {
        send_status(bus, kStageError, hit_count, error_count);
    }
    send_status(bus, kStageDone, hit_count, error_count);
}

constexpr ProbeBus kProbeBuses[] = {
    {0, 0, 1,  MdioAccess::kEscMii, TSW_TSNPORT_PORT1,  configure_internal_esc_mdio_pins},
    {1, 0, 1, MdioAccess::kEnetSmi, TSW_TSNPORT_PORT1, configure_internal_enet_mdio_pins},
    {2, 0, 1, MdioAccess::kTswPort, TSW_TSNPORT_PORT1,  configure_internal_tsw_mdio_pins},
    {3, 3, 1, MdioAccess::kEnetSmi, TSW_TSNPORT_PORT3, configure_pf_enet_mdio_pins},
};

} // namespace

int mdio_pin_scanner_main() {
    (void)init_telemetry_can();
    configure_common_clocks();

    while (true) {
        send_probe_info();
        board_delay_ms(20);
        for (const auto& bus : kProbeBuses) {
            scan_bus(bus);
            board_delay_ms(100);
        }
        board_delay_ms(kScanPeriodMs);
    }

    return 0;
}

} // namespace librmcs::firmware::board
