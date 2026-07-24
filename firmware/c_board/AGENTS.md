# c_board 固件指南

> 本目录专属指南，叠加在仓库根 `AGENTS.md` 之上（根为共享约束，此处只写 c_board 专属）。

## 芯片与工具链
- MCU：**STM32F407IGH6**（RoboMaster C 板 / DJI C-type），Cortex-M4F。
- ISA/工具链：ARM，`cmake/gcc-arm-none-eabi.cmake`，需 `arm-none-eabi-gcc`（**不是** RISC-V）。

## 构建
```bash
cmake --preset debug -S firmware/c_board
cmake --build firmware/c_board/build --target c_board_app c_board_bootloader
```
- preset：`debug` / `debug-outside` / `release`。`debug-outside` 置 `HOST_DEBUGGER=ON`（外部调试器场景）。
- target：`c_board_app`、`c_board_bootloader`。

## 目录结构
- `app/`、`bootloader/`：两套独立镜像（C++ librmcs 层）。
- `bsp/cubemx/`：CubeMX 生成产物（含链接脚本/时钟/外设初始化）。
- `bsp/`：`cmsis-core`、`cmsis-device-f4`、`stm32f4xx-hal-driver`、`tinyusb`、`SEGGER`（RTT）——第三方，视为只读。

## CubeMX 纪律（本板适用）
- 外设/时钟/中断/DMA 配置改在 CubeMX，源为 `bsp/cubemx/c_board_slave.ioc`；AI 只指出改哪个 `.ioc` 字段，由人工 Generate，**禁止**直接改 `bsp/cubemx/Core/` 等生成代码。
- `.ioc` 与手维护 `*.ld` 仅在用户明确要求时方可由 AI 编辑。
