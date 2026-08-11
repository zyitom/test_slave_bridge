# mc02 固件指南

> **文档类型**：现行规范（板级）
> **适用范围**：`firmware/mc02/`，DM-MC02 / CtrBoard-H7（STM32H723VGT6）
> **状态**：现行有效
> **相关文档**：[仓库根 AGENTS.md](../../AGENTS.md) · [本目录 README.md](README.md)（外设与低延迟设计） · [UART_RING_LOG.md](UART_RING_LOG.md)（UART 实录与实测） · [仓库根 README.md](../../README.md)（烧录流程）

> 本目录专属指南，叠加在仓库根 `AGENTS.md` 之上。深入的外设/时钟/低延迟设计见本目录 `README.md`，此处只列 agent 关键点。

## 摘要

mc02 是 Cortex-M7 @ 550 MHz 的高性能板，特点是 **CAN-FD 常驻 FD+BRS** 与 **热路径代码
放 `.itcm`**；USB 受封装限制只能跑 Full-Speed，所以吞吐瓶颈在 USB 而非 CAN。改这块板
之前必须知道两件事：外设配置回 CubeMX 改，以及**每次 CubeMX 重新 Generate 之后要手工
复原一批改动**（清单在 [README.md](README.md)）。

## 芯片与工具链
- MCU：**STM32H723VGT6**（DM-MC02 / CtrBoard-H7），Cortex-M7 @ 550 MHz，LQFP100。
- ISA/工具链：ARM，`cmake/gcc-arm-none-eabi.cmake`，需 `arm-none-eabi-gcc`。

## 构建
```bash
cmake --preset debug -S firmware/mc02
cmake --build firmware/mc02/build --target mc02_app mc02_bootloader
```
- preset：`debug` / `release`。target：`mc02_app`、`mc02_bootloader`。

### build 目录已存在时，`option()` 的默认值不生效 [实测 2026-08-12]

`cmake --preset debug` 在**已有** `firmware/mc02/build/` 上运行时**沿用旧 cache**，
`app/CMakeLists.txt` 里 `option(... ON)` 的默认值不会被应用。曾经因此整整一轮测试都跑在
一个自以为是 ON、实际是 OFF 的配置上，连带做出错误结论。

改开关必须显式传：

```bash
cmake -DLIBRMCS_APP_IMU_ENABLE=ON firmware/mc02/build && cmake --build firmware/mc02/build --target mc02_app
```

核对当前生效值：

```bash
grep LIBRMCS_APP firmware/mc02/build/CMakeCache.txt
arm-none-eabi-nm firmware/mc02/build/app/mc02_app.elf | grep -c bmi088   # IMU: OFF=3, ON=57
```

## 编译开关

全部定义在 `app/CMakeLists.txt`，默认值即下表。**后三个都占用 `DataId::kUart0`，互斥。**

| 开关 | 默认 | 作用 |
|---|---|---|
| `LIBRMCS_APP_IMU_ENABLE` | ON | BMI088 初始化与采样 |
| `LIBRMCS_APP_USB_DWC2_DMA` | OFF | DWC2 控制器内部 DMA（省掉 USB 中断里的 FIFO 拷贝） |
| `LIBRMCS_APP_RS485_ENABLE` | OFF | 把 USART2 的 RS-485 口暴露为 UART0；需 `.ioc` 的 USART2 DMA 已 Generate |
| `LIBRMCS_APP_LOOP_PROFILE` | OFF | 主循环分段耗时（DWT）在 kUart0 上以 ASCII 输出 |
| `LIBRMCS_APP_CAN_DIAG` | OFF | CAN 遥测记录在 kUart0 上输出 |

## 目录结构
- `app/`、`bootloader/`：两套独立镜像。`app/src/app.cpp` 提供自己的 `main()`，直接驱动生成的 `*_Config()` / `MX_*_Init()`。
- `bsp/cubemx/`：CubeMX 生成产物。`bsp/linker/`：手维护链接脚本（如 `STM32H723VGTx_APP.ld`，含 `.itcm` 热路径段）。
- `bsp/`：`cmsis-device-h7`、`stm32h7xx-hal-driver` 等第三方，视为只读。

## 关键特性（改代码前须知）
- USB：OTG_HS 跑 **Full-Speed**（LQFP100 无 HS PHY 引出）；吞吐杠杆在 CAN 侧（CAN-FD），不是 USB。
- CAN：FDCAN1/2/3 常驻 FD+BRS，逐帧按 host `is_fdcan` 切换，不做 INIT 重配。
- 热路径 `Can::handle_uplink/handle_downlink/try_transmit` 等放 `.itcm`，启动时从 FLASH 拷入。
- UART：四个口各一条**永不停的整环 circular DMA**，写指针由主循环读 `NDTR` 推导，不由中断维护。端口对象（含 DMA 环）放 `.d2_sram`，启动时从 FLASH 拷入，MPU region 1 在 `app.cpp` 里设为非缓存。**不要给 UART 的 DMA 开 FIFO/burst**——`NDTR` 只统计到 DMA FIFO，写指针会算错。
- UART 错误策略：`CR3.OVRDIS=1`、**`CR3.DDRE=0`**、`CR3.EIE=0`。`DDRE` 是"出错时禁用 DMA"，**置 1 会让一个坏字符永久杀死端口**——细节见 [UART_RING_LOG.md](UART_RING_LOG.md) 第 1 章。
- 实测吞吐天花板约 **800 KB/s 聚合**（USB Full-Speed 决定），781 KB/s 时零丢失；主循环在满过载下仍有约 10 倍余量。

## 不要用 HAL_RCCEx_GetPeriphCLKFreq() 取 UART 内核时钟 [实测 2026-08-05]

**结论先行：本版 HAL 的 `HAL_RCCEx_GetPeriphCLKFreq()` 对两个 UART 组都返回 0。**
它的 if/else 链只覆盖 SAI / SPI / ADC / SDMMC / SPI6 / FDCAN，
`RCC_PERIPHCLK_USART16910` 和 `RCC_PERIPHCLK_USART234578` **一个分支都没有**，
直接掉到末尾 `else { frequency = 0; }`（`bsp/stm32h7xx-hal-driver/Src/stm32h7xx_hal_rcc_ex.c`）。
那两个宏本身是存在的，所以编译期没有任何提示。

后果：`uart.hpp` 的 `handle_config()` 拿到 0 之后撞上自己的
`kernel_clock_hz == 0` 提前返回，**`BRR` 一次都没写过**，主机发来的运行时波特率
请求被静默忽略，端口永远停在 CubeMX 的 115200。

**正确做法**（已改成这样）：用 HAL 自己的 `UART_GETCLOCKSOURCE(handle, src)` 宏——
它按外设实例分派，正是 `UART_SetConfig()` 在 init 时算 `BRR` 用的同一个宏——
再按 `UART_CLOCKSOURCE_*` 取 `HAL_RCC_GetPCLK1Freq()` / `PCLK2` / HSI（**要按
`RCC_FLAG_HSIDIV` 右移**）/ CSI / LSE / PLL2Q / PLL3Q。不要自己手写"哪个口属于
哪个时钟组"的判断，那是在重复这个宏已经做对的事。

### 为什么 c_board 两行就够、这块板不行

c_board 同样的功能只用两行（`HAL_RCC_GetPCLKxFreq()`，见
`firmware/c_board/app/src/uart/uart.hpp` 的 `peripheral_clock_hz()`），**那在 F407
上是对的**：STM32F407 的 UART 直接挂在自己的 APB 总线上，总线频率就是内核时钟，
连 `Init.ClockPrescaler` 字段都不存在。STM32H7 在中间插了**每组可选时钟源 + 预分频器**，
所以必须先解析时钟源，频率才有意义。**别看着 c_board 简单就照抄过来。**

**为什么很难发现**：`UART7 <-> UART10` 自环测试在 115200 到 2000000 **全部 PASS**。
自环把两端同时设成目标值，配置没生效时两端**一起**留在 115200，于是波特率一致、
通信正常、测试通过。**自环只能验证两端一致，不能验证两端等于你要的值。**
要验证切换真的生效，必须**一端切、另一端不切**并确认通信变坏，或者跨板对打。
另见 [rmcs_board/AGENTS.md](../rmcs_board/AGENTS.md) 里同一次排查的 5321 侧 DLAB 坑。

## CubeMX 纪律（本板适用）
- 配置改在 CubeMX（`.ioc`），人工 Generate；**禁止**直接改 `bsp/cubemx/Core/` 生成代码。
- **每次 CubeMX “Generate Code” 后需手工复原**（会被覆盖），清单见 `README.md` 末节，例如删除 `Core/Src/main.c` 里重新生成的 `int main(void)`、把 `static void MPU_Config` 改回 `void MPU_Config`。
