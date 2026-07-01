# mc02 (DM-MC02 / CtrBoard-H7)

CAN/UART <-> USB forwarding firmware for the DaMiao DM-MC02 board
(**STM32H723VGT6**, Cortex-M7 @ 550 MHz, LQFP100). Shares `core/` and the host
SDK with the other boards; this directory holds only the board firmware
(`app/` + `bootloader/`) and the CubeMX BSP (`bsp/cubemx`).

## Peripherals

| Function | Peripheral | Notes |
|---|---|---|
| CAN x3 | FDCAN1/2/3 | **CAN-FD + BRS**, 1 Mbit/s arbitration / 5 Mbit/s data |
| UART x4 | USART1, UART7, USART10, UART5 (DBUS) | RX via `ReceiveToIdle_DMA` |
| USB | OTG_HS in **Full-Speed** mode (internal FS PHY, PA11/PA12) | see below |
| IMU | BMI088 on **SPI2** | accel CS PC0, gyro CS PC3_C, INT PE10/PE12 |
| LED | WS2812 on SPI6 (TX + BDMA) | addressable status LED |

### Why USB is Full-Speed only

The H72x/H73x embedded USB HS PHY is bonded out **only on LQFP144 / UFBGA176**.
On the LQFP100 part used here, USB HS would require an *external* ULPI PHY, which
this board does not have. So USB is capped at 12 Mbit/s Full-Speed and the
forwarding-throughput lever is the CAN side (CAN-FD), not USB.

## Low-latency design

- **FDCAN kernel clock = 80 MHz** from PLL2 (`PeriphCommonClock_Config`), the
  only way to divide cleanly to an exact 5 Mbit/s data phase (24 MHz HSE cannot).
- **CAN-FD per frame**: the controller is permanently in FD+BRS mode (a superset
  of classic CAN); each frame's format follows the host's `is_fdcan` flag via the
  Tx-element FDF/BRS bits -- no INIT-mode reconfiguration on the hot path.
- **Hardware RX timestamp**: FDCAN internal 16-bit counter, 1 tick = 1 nominal
  bit time = 1 us at 1 Mbit/s; reported in `CanDataView::timestamp_us`.
- **Bus-off auto-recovery**: `HAL_FDCAN_ErrorStatusCallback` clears `CCCR.INIT`.
- **NVIC priority** (lower = higher): FDCAN **1** > USB **2** > UART/DMA **3**, so
  motor feedback (CAN RX) is never delayed by bulk USB or UART DMA.
- **ITCM hot path**: `Can::handle_uplink/handle_downlink/try_transmit` and
  `HAL_FDCAN_RxFifo0Callback` live in the `.itcm` section, copied from FLASH to
  zero-wait ITCM at boot (in `App::App()`), removing I-cache/XIP fetch jitter from
  the worst case. See `bsp/linker/STM32H723VGTx_APP.ld`.

## IMPORTANT: after every CubeMX "Generate Code"

`app/src/app.cpp` provides its own `main()` and drives the generated
`*_Config()` / `MX_*_Init()` functions directly, so a few hand edits to the
generated files are required. CubeMX regeneration **reverts** them -- re-apply:

1. **`Core/Src/main.c`**: delete the regenerated `int main(void) { ... }` block
   (app.cpp is the real entry and uses TinyUSB, not `MX_USB_DEVICE_Init`).
2. **`Core/Src/main.c`**: change `static void MPU_Config` -> `void MPU_Config`
   (both the prototype and the definition).
3. **`Core/Inc/main.h`**: the `SystemClock_Config` / `PeriphCommonClock_Config` /
   `MPU_Config` declarations are kept inside `USER CODE BEGIN EFP` and should
   survive regeneration -- verify they are still present.

Then verify in the CubeMX GUI (these come from the `.ioc`, so they normally
persist, but confirm):

- Clock tree shows **FDCAN = 80 MHz** (PLL2).
- FDCAN1/2/3 `FrameFormat = FD_BRS`, data 5 Mbit/s, nominal 1 Mbit/s.
- FDCAN **element data size stays 8 bytes** -- the `MessageRAMOffset` values
  (0x406 / 0x812) assume 8-byte elements; 64-byte elements would overlap.
- SPI2 baud <= 10 MHz (BMI088 limit); currently prescaler 32 (~5.7 MHz).

## Build

```bash
cmake --preset debug -S firmware/mc02
cmake --build firmware/mc02/build --target mc02_app mc02_bootloader
```
