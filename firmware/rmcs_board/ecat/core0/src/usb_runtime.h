#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void rmcs_usb_runtime_init(void);
void rmcs_usb_runtime_task(void);

/* Run the vendor data pump once (USB IRQ masked internally). Callers below the
 * uplink doorbell IRQ must mask that IRQ around the call; see usb_runtime.cpp. */
void rmcs_usb_runtime_pump(void);

#ifdef __cplusplus
}
#endif
