#pragma once

#include <cstddef>

#include <hpm_common.h>
#include <hpm_gpiom_soc_drv.h>
#include <hpm_iomux.h>
#include <hpm_mcan_regs.h>
#include <hpm_soc.h>
#include <hpm_soc_irq.h>
#include <hpm_uart_regs.h>

#include "firmware/rmcs_board/app/src/gpio/gpio_pin.hpp"

namespace librmcs::firmware::board {

// Board capability flags. This board only exposes a single CAN, a single UART
// and a plain GPIO RGB LED, so the optional application modules (IMU/SPI, the
// GPIO application, the DBUS UART and the USB high/full-speed switch) are
// compiled out of the shared application layer.
#define BOARD_HAS_BMI088           0
#define BOARD_HAS_GPIO_APP         0
#define BOARD_HAS_USB_SPEED_SWITCH 0
#define BOARD_LED_USE_WS2812       0

// No HS/FS switch on this board; the HPM5321 USB runs at high speed.
#define BOARD_USB_FIXED_SPEED TUSB_SPEED_HIGH

#define BOARD_CAN0(prefix, suffix) prefix##0##suffix

uint32_t init_can(MCAN_Type* ptr);
void can_irq_handler(size_t board_can_index);

#define BOARD_UART0(prefix, suffix) prefix##2##suffix

uint32_t init_uart(UART_Type* ptr);
void uart_irq_handler(size_t board_uart_index);

// Plain GPIO RGB LED, active-low (common-anode: drive the pad LOW to light the
// channel). The last template argument is active_high = false.
constexpr GpioPin kLedBluePin = make_gpio_pin<gpiom_soc_gpio0, 'A', 29, false>();
constexpr GpioPin kLedGreenPin = make_gpio_pin<gpiom_soc_gpio0, 'A', 30, false>();
constexpr GpioPin kLedRedPin = make_gpio_pin<gpiom_soc_gpio0, 'A', 31, false>();

void init_led_pins();

} // namespace librmcs::firmware::board
