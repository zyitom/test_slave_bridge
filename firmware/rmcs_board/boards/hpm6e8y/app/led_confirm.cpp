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

// LED confirmation image. The GPIO LED pinout is already known (see
// GPIO_LED_REVERSE_ENGINEERING.md); this image drives each known LED so the pin
// map can be visually verified. On start every LED is driven to its OFF state
// (which also turns off the EtherCAT0 pad that otherwise floats high), then each
// LED is blinked twice in turn. Before each LED a CAN0 frame announces which pad
// is about to blink, so the physical LED can be matched to its pad.
namespace librmcs::firmware::board {
namespace {

constexpr uint32_t kAnnounceBaseId = 0x700U;
constexpr uint32_t kCanBaudrate = 1'000'000U;
constexpr uint32_t kCanClockDivider = 10U;
constexpr uint32_t kGpioAltFunc = IOC_PAD_FUNC_CTL_ALT_SELECT_SET(0);
constexpr uint32_t kBlinkOnMs = 250U;
constexpr uint32_t kBlinkOffMs = 250U;
constexpr uint32_t kBetweenLedsMs = 700U;
constexpr uint8_t kMagic = 'C';

struct Led {
    char bank;
    uint8_t pin;
    bool active_high;
};

// The 15 GPIO-controllable LEDs. The two ENET link LEDs are PHY-driven and not
// listed here. Banks A/B/C/E only -> GPIO port / GPIOM index equal the bank
// index and no always-on-domain routing is needed.
constexpr Led kLeds[] = {
    {'E', 5, false},  // Main RGB red   (active-low)
    {'E', 4, false},  // Main RGB green (active-low)
    {'E', 3, false},  // Main RGB blue  (active-low)
    {'C', 26, true},  // CAN0 green
    {'C', 27, true},  // CAN0 blue
    {'E', 0, true},   // CAN1 green
    {'E', 2, true},   // CAN1 blue
    {'A', 9, true},   // CAN2 green
    {'B', 0, true},   // CAN2 blue
    {'B', 2, true},   // CAN3 green
    {'B', 3, true},   // CAN3 blue
    {'A', 25, false}, // EtherCAT0 yellow -- active-LOW (OFF = drive high). With the
                      // wrong active-high flag the per-blink OFF step drove it low,
                      // leaving it lit when the sweep moved on.
    {'A', 28, true},  // EtherCAT1 yellow
    {'C', 20, true},  // EtherCAT middle green
    {'C', 21, true},  // EtherCAT middle red
};

constexpr size_t kLedCount = sizeof(kLeds) / sizeof(kLeds[0]);
static_assert(kLedCount == 15U);
static_assert(kAnnounceBaseId + kLedCount - 1 <= 0x7FFU);

constexpr uint32_t bank_index(char bank) { return static_cast<uint32_t>(bank - 'A'); }
constexpr uint32_t led_pad(const Led& led) { return bank_index(led.bank) * 32U + led.pin; }

// Value to write to turn the LED on/off, honouring polarity: an active-high LED
// lights when driven high, an active-low (common-anode) LED lights when low.
uint8_t drive_value(const Led& led, bool on) {
    return static_cast<uint8_t>(on == led.active_high ? 1U : 0U);
}

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

void announce(const Led& led, size_t led_index) {
    mcan_tx_frame_t frame{};
    frame.use_ext_id = false;
    frame.std_id = static_cast<uint16_t>(kAnnounceBaseId + led_index);
    frame.rtr = false;
    frame.canfd_frame = false;
    frame.bitrate_switch = false;
    frame.dlc = 8;
    frame.data_8[0] = kMagic;
    frame.data_8[1] = static_cast<uint8_t>(led_index);
    frame.data_8[2] = static_cast<uint8_t>(led.bank);
    frame.data_8[3] = led.pin;
    frame.data_8[4] = static_cast<uint8_t>(led.active_high ? 'H' : 'L');
    frame.data_8[5] = static_cast<uint8_t>(kLedCount);
    frame.data_8[6] = 0;
    frame.data_8[7] = 0;
    (void)mcan_transmit_via_txfifo_nonblocking(HPM_MCAN0, &frame, nullptr);
}

void configure_led_output(const Led& led) {
    const uint32_t port = bank_index(led.bank);
    HPM_IOC->PAD[led_pad(led)].FUNC_CTL = kGpioAltFunc;
    HPM_IOC->PAD[led_pad(led)].PAD_CTL = 0;
    gpiom_set_pin_controller(HPM_GPIOM, port, led.pin, gpiom_soc_gpio0);
    gpio_set_pin_output_with_initial(HPM_GPIO0, port, led.pin, drive_value(led, false));
}

void write_led(const Led& led, bool on) {
    gpio_write_pin(HPM_GPIO0, bank_index(led.bank), led.pin, drive_value(led, on));
}

} // namespace

int led_confirm_main() {
    (void)init_telemetry_can();

    // Park every LED OFF up front (also silences the EtherCAT0 pad that floats
    // high when the ESC does not drive it).
    for (const auto& led : kLeds) {
        configure_led_output(led);
    }
    board_delay_ms(500);

    while (true) {
        for (size_t index = 0; index < kLedCount; ++index) {
            const Led& led = kLeds[index];
            announce(led, index);
            board_delay_ms(60);
            for (uint32_t blink = 0; blink < 2; ++blink) {
                write_led(led, true);
                board_delay_ms(kBlinkOnMs);
                write_led(led, false);
                board_delay_ms(kBlinkOffMs);
            }
            board_delay_ms(kBetweenLedsMs);
        }
        board_delay_ms(1000);
    }

    return 0;
}

} // namespace librmcs::firmware::board
