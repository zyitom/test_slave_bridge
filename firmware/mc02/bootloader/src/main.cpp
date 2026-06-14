#include <device/usbd.h>
#include <gpio.h>
#include <main.h>
#include <tusb.h>

#include "firmware/mc02/bootloader/src/flash/layout.hpp"
#include "firmware/mc02/bootloader/src/flash/validation.hpp"
#include "firmware/mc02/bootloader/src/usb/dfu.hpp"
#include "firmware/mc02/bootloader/src/utility/assert.hpp"
#include "firmware/mc02/bootloader/src/utility/boot_mailbox.hpp"
#include "firmware/mc02/bootloader/src/utility/jump.hpp"

int main() {
    // The bootloader runs cache-less, mirroring the c_board (Cortex-M4) design.
    // With the D-cache enabled, every flash read in the DFU validate path
    // (CRC32 / SHA-256 / vector-table check) depends on manual cache
    // invalidation after each flash program; any gap leaves the validator
    // reading stale data, which makes a freshly-flashed image fail validation
    // and the device fall back into DFU. The bootloader only drives USB DFU and
    // flash programming, neither of which benefits from caching, so we leave the
    // caches and MPU off entirely to keep flash and RAM trivially coherent.
    HAL_Init();
    SystemClock_Config();

    RCC_PeriphCLKInitTypeDef usb_clk = {};
    usb_clk.PeriphClockSelection = RCC_PERIPHCLK_USB;
    usb_clk.UsbClockSelection    = RCC_USBCLKSOURCE_HSI48;
    librmcs::firmware::utility::assert_always(HAL_RCCEx_PeriphCLKConfig(&usb_clk) == HAL_OK);
    HAL_PWREx_EnableUSBVoltageDetector();
    __HAL_RCC_USB_OTG_HS_CLK_ENABLE();

    MX_GPIO_Init();

    using namespace librmcs::firmware; // NOLINT(google-build-using-namespace)

    const bool key_pressed = HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_RESET;
    const bool force_dfu   = utility::boot_mailbox.consume_enter_dfu_request() || key_pressed;
    if (!force_dfu) {
        if (flash::validate_app_image())
            utility::jump_to_app(flash::kAppStartAddress);
    }

    utility::assert_always(tusb_rhport_init(0, nullptr));

    while (true) {
        tud_task();
        usb::Dfu::instance().poll();
    }
}
