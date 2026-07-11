#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

namespace librmcs::firmware::board {
namespace {

constexpr uint32_t kInfoId = 0x740U;
constexpr uint32_t kStatusId = 0x741U;
constexpr uint32_t kCounterId = 0x750U;
constexpr uint32_t kPhyIdBase = 0x760U;
constexpr uint32_t kPhyStatusId = 0x770U;
constexpr uint32_t kCanBaudrate = 1'000'000U;
constexpr uint32_t kCanClockDivider = 10U;
constexpr uint32_t kVariantTestMs = 3000U;
constexpr uint32_t kResetSettleMs = 120U;
constexpr uint32_t kPollStepMs = 10U;
constexpr uint32_t kTxPeriodMs = 200U;
constexpr uint32_t kReportPeriodMs = 1000U;
constexpr uint8_t kProbeVersion = 3U;
constexpr uint8_t kCandidateCount = 2U;
constexpr uint8_t kVariantCount = 12U;
constexpr uint8_t kEthMagic = 'E';
constexpr uint8_t kCounterMagic = 'C';
constexpr uint8_t kPhyMagic = 'P';
constexpr uint8_t kStatusPins = 'P';
constexpr uint8_t kStatusInit = 'I';
constexpr uint8_t kStatusTest = 'T';
constexpr uint16_t kProbeEtherType = 0x88B5U;
constexpr uint32_t kTxDescCount = 4U;
constexpr uint32_t kRxDescCount = 8U;
constexpr uint16_t kTxBufferSize = 1536U;
constexpr uint16_t kRxBufferSize = 1536U;
constexpr uint16_t kFrameLength = 60U;

constexpr uint8_t kRtl8211Bmcr = 0U;
constexpr uint8_t kRtl8211Bmsr = 1U;
constexpr uint8_t kRtl8211PhyId1 = 2U;
constexpr uint8_t kRtl8211PhyId2 = 3U;
constexpr uint8_t kRtl8211Physr = 17U;
constexpr uint16_t kRtl8211OuiMsb = 0x001CU;
constexpr uint16_t kRtl8211OuiLsb = 0x32U;
constexpr uint16_t kRtl8211OuiLsbMask = 0xFC00U;
constexpr uint8_t kRtl8211OuiLsbShift = 10U;
constexpr uint16_t kBmsrAutoNegotiationCompleteMask = 0x0020U;
constexpr uint16_t kBmsrLinkStatusMask = 0x0004U;
constexpr uint16_t kRtl8211PhysrSpeedMask = 0xC000U;
constexpr uint8_t kRtl8211PhysrSpeedShift = 14U;
constexpr uint16_t kRtl8211PhysrDuplexMask = 0x2000U;
constexpr uint16_t kRtl8211PhysrResolvedMask = 0x0800U;
constexpr uint16_t kRtl8211PhysrLinkMask = 0x0400U;

enum class CandidateStatus : uint8_t {
    kOk = 0,
    kNoRealtek = 1,
    kNoLink = 2,
    kUnresolved = 3,
    kEnetInitFailed = 4,
};

struct Candidate {
    uint8_t index;
    void (*configure_pins)();
};

struct TestVariant {
    uint8_t index;
    uint8_t tx_delay;
    uint8_t rx_delay;
    uint8_t speed_code;
};

struct PhySelection {
    bool valid;
    bool link;
    bool resolved;
    uint8_t phy_addr;
    uint8_t speed_code;
    uint8_t duplex;
    uint16_t bmcr;
    uint16_t bmsr;
    uint16_t id1;
    uint16_t id2;
    uint16_t physr;
};

struct Counters {
    uint16_t tx_ok;
    uint16_t tx_fail;
    uint16_t rx_total;
    uint16_t rx_match;
};

constexpr uint32_t kTelemetryRamSize = MCAN_MSG_BUF_SIZE_IN_WORDS * sizeof(uint32_t);
static_assert(kTelemetryRamSize <= MCAN_MSG_BUF_BASE_VALID_END - MCAN_MSG_BUF_BASE_VALID_START);

ATTR_PLACE_AT_NONCACHEABLE_WITH_ALIGNMENT(ENET_SOC_DESC_ADDR_ALIGNMENT)
enet_rx_desc_t g_rx_desc[kRxDescCount];

ATTR_PLACE_AT_NONCACHEABLE_WITH_ALIGNMENT(ENET_SOC_DESC_ADDR_ALIGNMENT)
enet_tx_desc_t g_tx_desc[kTxDescCount];

ATTR_PLACE_AT_NONCACHEABLE_WITH_ALIGNMENT(HPM_L1C_CACHELINE_SIZE)
uint8_t g_rx_buffer[kRxDescCount][kRxBufferSize];

ATTR_PLACE_AT_NONCACHEABLE_WITH_ALIGNMENT(HPM_L1C_CACHELINE_SIZE)
uint8_t g_tx_buffer[kTxDescCount][kTxBufferSize];

enet_desc_t g_desc;

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

void send_info() {
    send_frame(
        kInfoId, {kEthMagic, 'N', 'E', 'T', kProbeVersion, kCandidateCount, kVariantCount, 'R'});
}

void send_status(
    const Candidate& candidate, const TestVariant& variant, uint8_t stage, CandidateStatus status,
    const PhySelection& phy) {
    send_frame(
        kStatusId, {kEthMagic, candidate.index, variant.index, stage, static_cast<uint8_t>(status),
                    phy.phy_addr, variant.speed_code,
                    static_cast<uint8_t>((phy.duplex << 1U) | static_cast<uint8_t>(phy.link))});
}

void send_counters(
    const Candidate& candidate, const TestVariant& variant, const Counters& counters, bool link) {
    send_frame(
        kCounterId,
        {kCounterMagic, candidate.index, variant.index, static_cast<uint8_t>(counters.tx_ok),
         static_cast<uint8_t>(counters.rx_match), static_cast<uint8_t>(counters.tx_fail),
         static_cast<uint8_t>(counters.rx_total), static_cast<uint8_t>(link)});
}

void send_phy_hit(uint8_t phy_addr, uint16_t id1, uint16_t id2) {
    send_frame(
        kPhyIdBase + phy_addr,
        {kPhyMagic, phy_addr, 1, 0, static_cast<uint8_t>(id1 >> 8), static_cast<uint8_t>(id1),
         static_cast<uint8_t>(id2 >> 8), static_cast<uint8_t>(id2)});
}

void send_phy_status(const PhySelection& phy) {
    send_frame(
        kPhyStatusId, {kPhyMagic, phy.phy_addr, static_cast<uint8_t>(phy.bmsr >> 8),
                       static_cast<uint8_t>(phy.bmsr), static_cast<uint8_t>(phy.physr >> 8),
                       static_cast<uint8_t>(phy.physr), phy.speed_code,
                       static_cast<uint8_t>((phy.duplex << 1U) | static_cast<uint8_t>(phy.link))});
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

// PE01 is the external Realtek RTL8211F active-low PHYRSTB, identified by the
// reset-pin scan (realtek_reset_scanner): with it left asserted the PHY reads all
// 0xffff on MDIO and never links (dark RJ45 LEDs); driving it high makes the PHY
// answer 0x001cc916 at once. The board provides no other release, so pulse it
// (assert low, then release high) before any ENET0 MDIO/RGMII use. PE01 is not in
// the PF/PB RGMII candidate banks, so this is safe for both candidates.
void release_realtek_reset() {
    HPM_IOC->PAD[IOC_PAD_PE01].FUNC_CTL = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(0);
    HPM_IOC->PAD[IOC_PAD_PE01].PAD_CTL = 0;
    gpiom_set_pin_controller(HPM_GPIOM, GPIOM_ASSIGN_GPIOE, 1, gpiom_soc_gpio0);
    gpio_set_pin_output_with_initial(HPM_GPIO0, GPIO_DO_GPIOE, 1, 0);
    board_delay_ms(15);
    gpio_write_pin(HPM_GPIO0, GPIO_DO_GPIOE, 1, 1);
    board_delay_ms(kResetSettleMs);
}

void configure_enet_mdio_pins() {
    constexpr uint32_t mdio_pad_ctl =
        IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1) | IOC_PAD_PAD_CTL_HYS_SET(1);

    HPM_IOC->PAD[IOC_PAD_PF00].FUNC_CTL = IOC_PF00_FUNC_CTL_ETH0_MDC;
    HPM_IOC->PAD[IOC_PAD_PF01].FUNC_CTL = IOC_PF01_FUNC_CTL_ETH0_MDIO;
    HPM_IOC->PAD[IOC_PAD_PF01].PAD_CTL = mdio_pad_ctl;
}

void configure_pf_rgmii_pins() {
    configure_enet_mdio_pins();

    HPM_IOC->PAD[IOC_PAD_PF02].FUNC_CTL = IOC_PF02_FUNC_CTL_ETH0_TXCK;
    HPM_IOC->PAD[IOC_PAD_PF03].FUNC_CTL = IOC_PF03_FUNC_CTL_ETH0_TXD_0;
    HPM_IOC->PAD[IOC_PAD_PF04].FUNC_CTL = IOC_PF04_FUNC_CTL_ETH0_TXD_1;
    HPM_IOC->PAD[IOC_PAD_PF05].FUNC_CTL = IOC_PF05_FUNC_CTL_ETH0_TXD_2;
    HPM_IOC->PAD[IOC_PAD_PF06].FUNC_CTL = IOC_PF06_FUNC_CTL_ETH0_TXD_3;
    HPM_IOC->PAD[IOC_PAD_PF07].FUNC_CTL = IOC_PF07_FUNC_CTL_ETH0_TXEN;
    HPM_IOC->PAD[IOC_PAD_PF08].FUNC_CTL = IOC_PF08_FUNC_CTL_ETH0_RXDV;
    HPM_IOC->PAD[IOC_PAD_PF09].FUNC_CTL = IOC_PF09_FUNC_CTL_ETH0_RXD_0;
    HPM_IOC->PAD[IOC_PAD_PF10].FUNC_CTL = IOC_PF10_FUNC_CTL_ETH0_RXD_1;
    HPM_IOC->PAD[IOC_PAD_PF11].FUNC_CTL = IOC_PF11_FUNC_CTL_ETH0_RXD_2;
    HPM_IOC->PAD[IOC_PAD_PF12].FUNC_CTL = IOC_PF12_FUNC_CTL_ETH0_RXD_3;
    HPM_IOC->PAD[IOC_PAD_PF13].FUNC_CTL = IOC_PF13_FUNC_CTL_ETH0_RXCK;
    HPM_IOC->PAD[IOC_PAD_PF14].FUNC_CTL = IOC_PF14_FUNC_CTL_ETH0_RXER;
    HPM_IOC->PAD[IOC_PAD_PF15].FUNC_CTL = IOC_PF15_FUNC_CTL_ETH0_TXER;
}

void configure_pb_rgmii_pins() {
    configure_enet_mdio_pins();

    HPM_IOC->PAD[IOC_PAD_PB00].FUNC_CTL = IOC_PB00_FUNC_CTL_ETH0_RXDV;
    HPM_IOC->PAD[IOC_PAD_PB01].FUNC_CTL = IOC_PB01_FUNC_CTL_ETH0_RXD_0;
    HPM_IOC->PAD[IOC_PAD_PB02].FUNC_CTL = IOC_PB02_FUNC_CTL_ETH0_RXD_1;
    HPM_IOC->PAD[IOC_PAD_PB03].FUNC_CTL = IOC_PB03_FUNC_CTL_ETH0_RXD_2;
    HPM_IOC->PAD[IOC_PAD_PB04].FUNC_CTL = IOC_PB04_FUNC_CTL_ETH0_RXD_3;
    HPM_IOC->PAD[IOC_PAD_PB05].FUNC_CTL = IOC_PB05_FUNC_CTL_ETH0_RXCK;
    HPM_IOC->PAD[IOC_PAD_PB06].FUNC_CTL = IOC_PB06_FUNC_CTL_ETH0_TXCK;
    HPM_IOC->PAD[IOC_PAD_PB07].FUNC_CTL = IOC_PB07_FUNC_CTL_ETH0_TXD_0;
    HPM_IOC->PAD[IOC_PAD_PB08].FUNC_CTL = IOC_PB08_FUNC_CTL_ETH0_TXD_1;
    HPM_IOC->PAD[IOC_PAD_PB09].FUNC_CTL = IOC_PB09_FUNC_CTL_ETH0_TXD_2;
    HPM_IOC->PAD[IOC_PAD_PB10].FUNC_CTL = IOC_PB10_FUNC_CTL_ETH0_TXD_3;
    HPM_IOC->PAD[IOC_PAD_PB11].FUNC_CTL = IOC_PB11_FUNC_CTL_ETH0_TXEN;
}

bool read_phy(uint8_t phy_addr, uint8_t reg_addr, uint16_t& value) {
    return enet_read_phy(HPM_ENET0, phy_addr, reg_addr, &value) == status_success;
}

bool is_realtek_id(uint16_t id1, uint16_t id2) {
    return id1 == kRtl8211OuiMsb
        && ((id2 & kRtl8211OuiLsbMask) >> kRtl8211OuiLsbShift) == kRtl8211OuiLsb;
}

uint8_t physr_speed_code(uint16_t physr) {
    const uint8_t speed =
        static_cast<uint8_t>((physr & kRtl8211PhysrSpeedMask) >> kRtl8211PhysrSpeedShift);
    if (speed == 2U) {
        return 3U;
    }
    if (speed == 1U) {
        return 2U;
    }
    return 1U;
}

enet_line_speed_t enet_speed_from_code(uint8_t speed_code) {
    if (speed_code == 3U) {
        return enet_line_speed_1000mbps;
    }
    if (speed_code == 2U) {
        return enet_line_speed_100mbps;
    }
    return enet_line_speed_10mbps;
}

PhySelection scan_realtek_phys() {
    PhySelection first_valid{};
    PhySelection first_linked{};

    configure_enet_mdio_pins();

    for (uint8_t phy_addr = 0; phy_addr < 32U; ++phy_addr) {
        uint16_t id1 = 0;
        uint16_t id2 = 0;
        if (!read_phy(phy_addr, kRtl8211PhyId1, id1) || !read_phy(phy_addr, kRtl8211PhyId2, id2)
            || !is_realtek_id(id1, id2)) {
            continue;
        }

        PhySelection phy{};
        phy.valid = true;
        phy.phy_addr = phy_addr;
        phy.id1 = id1;
        phy.id2 = id2;
        (void)read_phy(phy_addr, kRtl8211Bmcr, phy.bmcr);
        (void)read_phy(phy_addr, kRtl8211Bmsr, phy.bmsr);
        (void)read_phy(phy_addr, kRtl8211Physr, phy.physr);
        const bool bmsr_link = (phy.bmsr & kBmsrLinkStatusMask) != 0U;
        const bool bmsr_resolved = (phy.bmsr & kBmsrAutoNegotiationCompleteMask) != 0U;
        const bool physr_link = (phy.physr & kRtl8211PhysrLinkMask) != 0U;
        const bool physr_resolved = (phy.physr & kRtl8211PhysrResolvedMask) != 0U;
        phy.link = bmsr_link || physr_link;
        phy.resolved = bmsr_resolved || physr_resolved;
        phy.duplex = static_cast<uint8_t>((phy.physr & kRtl8211PhysrDuplexMask) != 0U);
        phy.speed_code = physr_resolved ? physr_speed_code(phy.physr) : 0U;

        send_phy_hit(phy_addr, id1, id2);

        if (!first_valid.valid) {
            first_valid = phy;
        }
        if (phy.link && phy.resolved && !first_linked.valid) {
            first_linked = phy;
        }
    }

    if (first_linked.valid) {
        return first_linked;
    }
    return first_valid;
}

void init_enet_descriptors() {
    std::memset(g_rx_desc, 0, sizeof(g_rx_desc));
    std::memset(g_tx_desc, 0, sizeof(g_tx_desc));
    std::memset(g_rx_buffer, 0, sizeof(g_rx_buffer));
    std::memset(g_tx_buffer, 0, sizeof(g_tx_buffer));
    std::memset(&g_desc, 0, sizeof(g_desc));

    g_desc.tx_desc_list_head = reinterpret_cast<enet_tx_desc_t*>(
        core_local_mem_to_sys_address(BOARD_RUNNING_CORE, reinterpret_cast<uint32_t>(g_tx_desc)));
    g_desc.rx_desc_list_head = reinterpret_cast<enet_rx_desc_t*>(
        core_local_mem_to_sys_address(BOARD_RUNNING_CORE, reinterpret_cast<uint32_t>(g_rx_desc)));
    g_desc.tx_buff_cfg.buffer =
        core_local_mem_to_sys_address(BOARD_RUNNING_CORE, reinterpret_cast<uint32_t>(g_tx_buffer));
    g_desc.tx_buff_cfg.count = kTxDescCount;
    g_desc.tx_buff_cfg.size = kTxBufferSize;
    g_desc.rx_buff_cfg.buffer =
        core_local_mem_to_sys_address(BOARD_RUNNING_CORE, reinterpret_cast<uint32_t>(g_rx_buffer));
    g_desc.rx_buff_cfg.count = kRxDescCount;
    g_desc.rx_buff_cfg.size = kRxBufferSize;

    enet_get_default_tx_control_config(HPM_ENET0, &g_desc.tx_control_config);
    g_desc.tx_control_config.disable_crc = false;
    g_desc.tx_control_config.enable_crcr = false;
    g_desc.tx_control_config.cic = enet_cic_disable;
}

void build_test_frame(uint8_t source_id, uint16_t sequence, uint8_t* buffer) {
    constexpr uint8_t kBroadcast[] = {0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU};
    constexpr uint8_t kPayload[] = {
        'R', 'M', 'C', 'S', '-', 'H', 'P', 'M', '6', 'E', '8', 'Y', '-',
        'E', 'N', 'E', 'T', '-', 'P', 'I', 'N', '-', 'T', 'E', 'S', 'T',
    };

    std::memset(buffer, 0, kFrameLength);
    std::memcpy(&buffer[0], kBroadcast, sizeof(kBroadcast));
    buffer[6] = 0x02U;
    buffer[7] = 0x52U;
    buffer[8] = 0x4DU;
    buffer[9] = 0x43U;
    buffer[10] = 0x53U;
    buffer[11] = source_id;
    buffer[12] = static_cast<uint8_t>(kProbeEtherType >> 8);
    buffer[13] = static_cast<uint8_t>(kProbeEtherType);
    std::memcpy(&buffer[14], kPayload, sizeof(kPayload));
    buffer[54] = source_id;
    buffer[55] = static_cast<uint8_t>(sequence >> 8);
    buffer[56] = static_cast<uint8_t>(sequence);
}

bool transmit_test_frame(
    const Candidate& candidate, const TestVariant& variant, uint16_t sequence) {
    enet_tx_desc_t* tx_desc = g_desc.tx_desc_list_cur;
    if (tx_desc == nullptr || tx_desc->tdes0_bm.own != 0U) {
        return false;
    }

    auto* buffer = reinterpret_cast<uint8_t*>(tx_desc->tdes2_bm.buffer1);
    build_test_frame(
        static_cast<uint8_t>((candidate.index << 4U) | variant.index), sequence, buffer);

    return enet_prepare_tx_desc(
               HPM_ENET0, &g_desc.tx_desc_list_cur, &g_desc.tx_control_config, kFrameLength,
               g_desc.tx_buff_cfg.size)
        == ENET_SUCCESS;
}

void release_rx_frame(const enet_frame_t& frame) {
    enet_rx_desc_t* rx_desc = frame.rx_desc;
    for (uint32_t i = 0; i < frame.seg && rx_desc != nullptr; ++i) {
        rx_desc->rdes0_bm.own = 1;
        rx_desc = reinterpret_cast<enet_rx_desc_t*>(rx_desc->rdes3_bm.next_desc);
    }
}

void poll_receive(Counters& counters) {
    for (uint32_t i = 0; i < kRxDescCount; ++i) {
        if (enet_check_received_frame(&g_desc.rx_desc_list_cur, &g_desc.rx_frame_info) != 1U) {
            break;
        }

        const enet_frame_t frame =
            enet_get_received_frame(&g_desc.rx_desc_list_cur, &g_desc.rx_frame_info);
        g_desc.rx_frame_info.seg_count = 0;
        if (frame.length >= 14U && frame.buffer != 0U) {
            ++counters.rx_total;
            const auto* buffer = reinterpret_cast<const uint8_t*>(frame.buffer);
            const uint16_t ether_type =
                (static_cast<uint16_t>(buffer[12]) << 8U) | static_cast<uint16_t>(buffer[13]);
            if (ether_type == kProbeEtherType) {
                ++counters.rx_match;
            }
        }
        release_rx_frame(frame);
    }

    enet_rx_resume(HPM_ENET0);
}

CandidateStatus init_enet_for_variant(
    const Candidate& candidate, const TestVariant& variant, PhySelection& phy) {
    candidate.configure_pins();
    send_status(candidate, variant, kStatusPins, CandidateStatus::kOk, phy);
    board_delay_ms(20);

    phy = scan_realtek_phys();
    CandidateStatus mdio_status = CandidateStatus::kOk;
    if (!phy.valid) {
        mdio_status = CandidateStatus::kNoRealtek;
        phy.speed_code = variant.speed_code;
    } else {
        send_phy_status(phy);
        if (!phy.link) {
            mdio_status = CandidateStatus::kNoLink;
        } else if (!phy.resolved) {
            mdio_status = CandidateStatus::kUnresolved;
        }
    }
    send_status(candidate, variant, kStatusInit, mdio_status, phy);

    (void)enet_rgmii_enable_clock(HPM_ENET0);
    (void)enet_rgmii_set_clock_delay(HPM_ENET0, variant.tx_delay, variant.rx_delay);

    init_enet_descriptors();

    enet_mac_config_t mac_config{};
    const uint8_t source_id = static_cast<uint8_t>((candidate.index << 4U) | variant.index);
    mac_config.mac_addr_high[0] = (static_cast<uint32_t>(source_id) << 8U) | 0x53U;
    mac_config.mac_addr_low[0] = (0x43UL << 24U) | (0x4DUL << 16U) | (0x52UL << 8U) | 0x02UL;
    mac_config.valid_max_count = 1;
    mac_config.dma_pbl = enet_pbl_32;
    mac_config.sarc = enet_sarc_disable;

    enet_int_config_t int_config{};
    if (enet_controller_init(HPM_ENET0, enet_inf_rgmii, &g_desc, &mac_config, &int_config)
        != status_success) {
        return CandidateStatus::kEnetInitFailed;
    }

    enet_set_line_speed(HPM_ENET0, enet_speed_from_code(variant.speed_code));
    enet_set_duplex_mode(HPM_ENET0, phy.duplex != 0U ? enet_full_duplex : enet_half_duplex);

    return mdio_status;
}

void exercise_variant(const Candidate& candidate, const TestVariant& variant) {
    PhySelection phy{};
    CandidateStatus status = init_enet_for_variant(candidate, variant, phy);
    send_status(candidate, variant, kStatusInit, status, phy);

    Counters counters{};
    if (status == CandidateStatus::kEnetInitFailed) {
        send_counters(candidate, variant, counters, phy.link);
        board_delay_ms(kVariantTestMs);
        return;
    }

    uint16_t sequence = 0;
    for (uint32_t elapsed = 0; elapsed < kVariantTestMs; elapsed += kPollStepMs) {
        if ((elapsed % kTxPeriodMs) == 0U) {
            if (transmit_test_frame(candidate, variant, sequence++)) {
                ++counters.tx_ok;
            } else {
                ++counters.tx_fail;
            }
        }

        poll_receive(counters);

        if ((elapsed % kReportPeriodMs) == 0U) {
            phy = scan_realtek_phys();
            send_status(candidate, variant, kStatusTest, status, phy);
            send_counters(candidate, variant, counters, phy.link);
        }

        board_delay_ms(kPollStepMs);
    }

    send_counters(candidate, variant, counters, phy.link);
}

constexpr Candidate kCandidates[] = {
    {0, configure_pf_rgmii_pins},
    {1, configure_pb_rgmii_pins},
};

constexpr TestVariant kTestVariants[] = {
    { 0, 0, 0, 3},
    { 1, 0, 7, 3},
    { 2, 7, 0, 3},
    { 3, 7, 7, 3},
    { 4, 0, 0, 2},
    { 5, 0, 7, 2},
    { 6, 7, 0, 2},
    { 7, 7, 7, 2},
    { 8, 0, 0, 1},
    { 9, 0, 7, 1},
    {10, 7, 0, 1},
    {11, 7, 7, 1},
};

} // namespace

int enet_packet_tester_main() {
    (void)init_telemetry_can();
    configure_common_clocks();

    // Release the external Realtek PHY reset (PE01) once before probing; the PHY
    // stays in reset otherwise and reads all 0xffff. The per-variant pin config
    // below only remuxes the RGMII data pins on top of this.
    release_realtek_reset();

    while (true) {
        send_info();
        for (const auto& candidate : kCandidates) {
            for (const auto& variant : kTestVariants) {
                exercise_variant(candidate, variant);
                board_delay_ms(100);
            }
        }
    }

    return 0;
}

} // namespace librmcs::firmware::board
