# mc02 固件指南

> 本目录专属指南，叠加在仓库根 `AGENTS.md` 之上。深入的外设/时钟/低延迟设计见本目录 `README.md`，此处只列 agent 关键点。

## 芯片与工具链
- MCU：**STM32H723VGT6**（DM-MC02 / CtrBoard-H7），Cortex-M7 @ 550 MHz，LQFP100。
- ISA/工具链：ARM，`cmake/gcc-arm-none-eabi.cmake`，需 `arm-none-eabi-gcc`。

## 构建
```bash
cmake --preset debug -S firmware/mc02
cmake --build firmware/mc02/build --target mc02_app mc02_bootloader
```
- preset：`debug` / `release`。target：`mc02_app`、`mc02_bootloader`。

## 目录结构
- `app/`、`bootloader/`：两套独立镜像。`app/src/app.cpp` 提供自己的 `main()`，直接驱动生成的 `*_Config()` / `MX_*_Init()`。
- `bsp/cubemx/`：CubeMX 生成产物。`bsp/linker/`：手维护链接脚本（如 `STM32H723VGTx_APP.ld`，含 `.itcm` 热路径段）。
- `bsp/`：`cmsis-device-h7`、`stm32h7xx-hal-driver` 等第三方，视为只读。

## 关键特性（改代码前须知）
- USB：OTG_HS 跑 **Full-Speed**（LQFP100 无 HS PHY 引出）；吞吐杠杆在 CAN 侧（CAN-FD），不是 USB。
- CAN：FDCAN1/2/3 常驻 FD+BRS，逐帧按 host `is_fdcan` 切换，不做 INIT 重配。
- 热路径 `Can::handle_uplink/handle_downlink/try_transmit` 等放 `.itcm`，启动时从 FLASH 拷入。

## CubeMX 纪律（本板适用）
- 配置改在 CubeMX（`.ioc`），人工 Generate；**禁止**直接改 `bsp/cubemx/Core/` 生成代码。
- **每次 CubeMX “Generate Code” 后需手工复原**（会被覆盖），清单见 `README.md` 末节，例如删除 `Core/Src/main.c` 里重新生成的 `int main(void)`、把 `static void MPU_Config` 改回 `void MPU_Config`。
