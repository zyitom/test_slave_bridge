# mc02（DM-MC02 / CtrBoard-H7）

> **文档类型**：背景说明（外设、低延迟设计、CubeMX 复原清单）
> **适用范围**：`firmware/mc02/`，达妙 DM-MC02 板（STM32H723VGT6）
> **状态**：现行有效
> **相关文档**：[AGENTS.md](AGENTS.md)（命令与约束以那份为准） · [仓库根 README.md](../../README.md)（DFU 烧录流程） · [仓库根 AGENTS.md](../../AGENTS.md)

## 摘要

本文件讲 mc02 这块板**有什么外设、为什么这么设计**，以及一份必须照做的操作清单：
**每次 CubeMX 重新 Generate Code 之后要手工复原哪些改动**。日常构建命令见
[AGENTS.md](AGENTS.md)，烧录见[仓库根 README.md](../../README.md#烧录-appusb-dfu)。

mc02 是达妙 DM-MC02 板（**STM32H723VGT6**，Cortex-M7 @ 550 MHz，LQFP100）上的
CAN/UART <-> USB 转发固件。它与其他板共用 `core/` 和主机 SDK；本目录只放板级固件
（`app/` + `bootloader/`）与 CubeMX BSP（`bsp/cubemx`）。

## 本文导航

| 章节 | 内容 |
|---|---|
| [外设一览](#外设一览) | CAN/UART/USB/IMU/LED 分别挂在哪 |
| [为什么 USB 只能跑 Full-Speed](#为什么-usb-只能跑-full-speed) | 封装限制，非软件问题 |
| [低延迟设计](#低延迟设计) | 时钟、CAN-FD、中断优先级、ITCM 热路径 |
| [重要：每次 CubeMX Generate 之后](#重要每次-cubemx-generate-code-之后) | **必做**的复原清单 |
| [构建](#构建) | 编译命令 |

## 外设一览

| 功能 | 外设 | 说明 |
|---|---|---|
| CAN x3 | FDCAN1/2/3 | **CAN-FD + BRS**，仲裁段 1 Mbit/s、数据段 5 Mbit/s |
| UART x4 | USART1、UART7、USART10、UART5（DBUS） | 接收走 `ReceiveToIdle_DMA` |
| USB | OTG_HS 工作在 **Full-Speed** 模式（片内 FS PHY，PA11/PA12） | 原因见下节 |
| IMU | BMI088，挂 **SPI2** | 加速度计片选 PC0，陀螺片选 PC3_C，中断 PE10/PE12 |
| LED | WS2812，挂 SPI6（TX + BDMA） | 可寻址状态灯 |

## 为什么 USB 只能跑 Full-Speed

H72x/H73x 的片内 USB HS PHY **只在 LQFP144 / UFBGA176 封装上引出**。本板用的是 LQFP100，
要跑 USB HS 就必须外挂 ULPI PHY，而这块板没有。所以 USB 被限制在 12 Mbit/s 的
Full-Speed，**转发吞吐的杠杆在 CAN 侧（CAN-FD），不在 USB 侧**。

## 低延迟设计

- **FDCAN 内核时钟 = 80 MHz**，来自 PLL2（`PeriphCommonClock_Config`）。这是唯一能整除
  出精确 5 Mbit/s 数据段的取法（24 MHz 的 HSE 做不到）。
- **CAN-FD 逐帧切换**：控制器**常驻** FD+BRS 模式（它是经典 CAN 的超集）；每一帧的格式
  通过发送元素的 FDF/BRS 位、按主机下发的 `is_fdcan` 标志决定——热路径上**不做 INIT
  模式重配置**。
- **硬件接收时间戳：已禁用**（代码已注释保留）。FDCAN 内部计数器只有 16 位，1 tick =
  1 个标称位时间 = 1 us @ 1 Mbit/s，约 65.5 ms 就回绕，无法满足 `CanDataView::timestamp_us`
  约定的 32 位微秒语义（主机端按 32 位回绕做差分会周期性算出负值）。上行不再携带该字段，
  每帧省 4 字节。若要恢复，需同时放开 `can.hpp` 的
  `HAL_FDCAN_ConfigTimestampCounter/EnableTimestampCounter` 与 `can.cpp` 的赋值，并先把
  值补宽（例如在 ISR 内用自由运行的 TIM5 微秒计数器补齐高位），否则主机侧仍不可用。
- **Bus-off 自动恢复**：`HAL_FDCAN_ErrorStatusCallback` 里清 `CCCR.INIT`。
- **NVIC 优先级**（数值越小优先级越高）：FDCAN **1** > USB **2** > UART/DMA **3**，
  保证电机反馈（CAN RX）永远不会被大块 USB 传输或 UART DMA 拖延。USB 的那一档
  由 `Vendor::Vendor()` 显式设置：TinyUSB 的 `dcd_int_enable` 只调 `NVIC_EnableIRQ`
  不设优先级，而会设优先级的 CubeMX `HAL_PCD_MspInit` 属于 ST 的设备栈、本固件不链接
  它——不显式钉住的话 OTG_HS 会停在复位值 0，反压在 FDCAN 之上。
- **ITCM 热路径**：整条 CAN 转发路径都在 `.itcm`，启动时（`App::App()`）从 FLASH
  拷进零等待 ITCM，把 I-cache/XIP 取指抖动从最坏情况里去掉。除 `can.cpp` 的四个函数
  外，链接脚本还按 mangled name 收进了每帧都会调到的叶子函数：`Serializer`、
  `Bitfield`、`InterruptSafeBuffer`、`RingBuffer`、`get_serializer` 和几个 `Lazy`
  取值器——否则它们留在 FLASH，每次调用都要走一条长跳 veneer（ITCM 在 0x0，FLASH 在
  0x08040000，远超 BL 的跳转范围）。目前 ISR 路径只剩 `memcpy` 和 assert 失败路径
  仍在 FLASH。占用约 5.6 KB / 64 KB。见 `bsp/linker/STM32H723VGTx_APP.ld`——那里的
  注释说明了为什么 `.itcm` 必须排在 `.text` 前面，以及哪些东西**不能**收进去
  （启动期就会执行的代码，例如 `Lazy<App>::init`）。

## 重要：每次 CubeMX "Generate Code" 之后

`app/src/app.cpp` 提供自己的 `main()`，并直接驱动生成出来的 `*_Config()` / `MX_*_Init()`
函数，因此对生成文件做了少量手工改动。**CubeMX 重新生成会把这些改动冲掉**，需要逐条
重新施加：

1. **`Core/Src/main.c`**：删除重新生成出来的 `int main(void) { ... }` 整块
   （真正的入口是 `app.cpp`，而且它用的是 TinyUSB，不是 `MX_USB_DEVICE_Init`）。
2. **`Core/Src/main.c`**：把 `static void MPU_Config` 改回 `void MPU_Config`
   （函数声明和定义两处都要改）。
3. **`Core/Inc/main.h`**：`SystemClock_Config` / `PeriphCommonClock_Config` /
   `MPU_Config` 三个声明放在 `USER CODE BEGIN EFP` 区块内，正常情况下能在重新生成后
   保留下来——但仍需确认它们还在。

然后在 CubeMX 图形界面里核对以下几项（它们来自 `.ioc`，通常会保留，但要确认）：

- 时钟树里 **FDCAN = 80 MHz**（来自 PLL2）。
- FDCAN1/2/3 的 `FrameFormat = FD_BRS`，数据段 5 Mbit/s，标称段 1 Mbit/s。
- FDCAN **元素数据长度保持 8 字节**——`MessageRAMOffset` 那两个值（`0x406` / `0x812`）
  是按 8 字节元素算的；改成 64 字节元素会导致区域重叠。
- SPI2 波特率 <= 10 MHz（BMI088 的上限）；当前分频系数 32（约 5.7 MHz）。

> 相关约束见[仓库根 AGENTS.md 的 CubeMX BSP 修改纪律](../../AGENTS.md#cubemx-bsp-修改纪律)：
> 生成目录禁止直接编辑，配置改动一律回到 `.ioc` / CubeMX。

## 构建

```bash
cmake --preset debug -S firmware/mc02
cmake --build firmware/mc02/build --target mc02_app mc02_bootloader
```

preset 与 target 的完整说明见 [AGENTS.md](AGENTS.md#构建)。
