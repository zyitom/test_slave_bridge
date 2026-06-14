#include "usb_device.h"
#include "stm32h7xx_hal.h"
#include "tusb.h"

// Satisfies the dead-code reference in CubeMX-generated stm32h7xx_it.c.
// The actual USB IRQ is handled by dcd_int_handler() via USER CODE in that file.
PCD_HandleTypeDef hpcd_USB_OTG_HS;

void MX_USB_DEVICE_Init(void) {
    tusb_init();
}
