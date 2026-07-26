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
- **Hardware RX timestamp: disabled**（代码已注释保留）。FDCAN 内部计数器只有 16
  位，1 tick = 1 nominal bit time = 1 us @ 1 Mbit/s，约 65.5 ms 就回绕，无法满足
  `CanDataView::timestamp_us` 约定的 32 位微秒语义（主机端按 32 位回绕做差分会
  周期性算出负值）。上行不再携带该字段，每帧省 4 字节。若要恢复，需同时放开
  `can.hpp` 的 `HAL_FDCAN_ConfigTimestampCounter/EnableTimestampCounter` 与
  `can.cpp` 的赋值，并先把值补宽（例如在 ISR 内用自由运行的 TIM5 微秒计数器补齐
  高位），否则主机侧仍不可用。
- **Bus-off auto-recovery**: `HAL_FDCAN_ErrorStatusCallback` clears `CCCR.INIT`.
- **NVIC priority** (lower = higher): FDCAN **1** > USB **2** > UART/DMA **3**, so
  motor feedback (CAN RX) is never delayed by bulk USB or UART DMA. USB 的那一档
  由 `Vendor::Vendor()` 显式设置：TinyUSB 的 `dcd_int_enable` 只调
  `NVIC_EnableIRQ` 不设优先级，而会设优先级的 CubeMX `HAL_PCD_MspInit` 属于 ST 的
  设备栈、本固件不链接它——不显式钉住的话 OTG_HS 会停在复位值 0，反压在 FDCAN 之上。
- **ITCM hot path**: 整条 CAN 转发路径都在 `.itcm`，启动时（`App::App()`）从 FLASH
  拷进零等待 ITCM，把 I-cache/XIP 取指抖动从最坏情况里去掉。除 `can.cpp` 的四个函数
  外，链接脚本还按 mangled name 收进了每帧都会调到的叶子函数：`Serializer`、
  `Bitfield`、`InterruptSafeBuffer`、`RingBuffer`、`get_serializer` 和几个 `Lazy`
  取值器——否则它们留在 FLASH，每次调用都要走一条长跳 veneer（ITCM 在 0x0，FLASH 在
  0x08040000，远超 BL 的跳转范围）。目前 ISR 路径只剩 `memcpy` 和 assert 失败路径
  仍在 FLASH。占用约 5.6 KB / 64 KB。见 `bsp/linker/STM32H723VGTx_APP.ld`——那里的
  注释说明了为什么 `.itcm` 必须排在 `.text` 前面，以及哪些东西**不能**收进去
  （启动期就会执行的代码，例如 `Lazy<App>::init`）。

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
