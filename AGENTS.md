# 仓库指南

> 本文件是全仓库共享的 agent 指南。为兼容不同工具（Claude Code 读 `CLAUDE.md`，
> Codex 等读 `AGENTS.md`），`CLAUDE.md` 是指向本文件的符号链接——只维护
> `AGENTS.md` 一份即可，两边同步。各芯片子目录下另有专属 `AGENTS.md`（同样带
> `CLAUDE.md` 软链），会在处理该目录代码时叠加加载，不影响本文件的共享约束。

## 项目结构与模块组织
- `core/`：共享协议、数据模型和工具代码（`core/include/librmcs`、`core/src`）。
- `host/`：桌面端 SDK/库源码和公共头文件（`host/include/librmcs`、`host/src`），构建产物为 `librmcs-sdk`。
- `firmware/`：各板卡固件，详见下方“多芯片固件总览”。每块板一个子目录，含独立的 `CMakePresets.json` 与专属 `AGENTS.md`。
  - `firmware/common/`：固件层共享 CMake（`librmcs_firmware.cmake`），非某颗芯片专属。
- `firmware/*/bsp/`：厂商/子模块依赖；除非有意更新子模块，否则视为第三方代码。
- `.scripts/`：与 CI 对齐的工具脚本（`clang-format-check`、`clang-tidy-check`、`generate_version`）。

## 多芯片固件总览
每块板都复用 `core/` 与 host SDK，只在 `firmware/<board>/` 下放板级固件。芯片专属
的工具链路径、外设、烧录方式、已知坑等，写在各自的 `firmware/<board>/AGENTS.md`。

| 板子 | MCU | ISA / 工具链 | 构建 target | 备注 |
|---|---|---|---|---|
| `c_board` | STM32F4xx | ARM `cmake/gcc-arm-none-eabi.cmake` | `c_board_app` `c_board_bootloader` | CubeMX BSP + TinyUSB |
| `mc02` | STM32H723VGT6（M7） | ARM `cmake/gcc-arm-none-eabi.cmake` | `mc02_app` `mc02_bootloader` | CAN-FD，USB Full-Speed |
| `ch32_board` | WCH CH32H417（Qingke V5F） | RISC-V `cmake/toolchain-wch-riscv.cmake` | `ch32_board_app`（暂无 bootloader） | USB 3.0 SuperSpeed，片上 bring-up 中 |
| `rmcs_board` | HPM6E8Y / HPM5321（Andes） | RISC-V 超级构建 + HPMicro GNU 工具链 | `rmcs_board_app` `rmcs_board_bootloader`；EtherCAT 桥在 `ecat/` | HPM SDK v1.11.0，双核 ECAT 桥 |

> RISC-V 芯片（`ch32_board`、`rmcs_board`）用 `riscv32-unknown-elf-gcc`，**不要**用
> `arm-none-eabi-gcc`；后者只给 STM32（`c_board`、`mc02`）用。

## CubeMX BSP 修改纪律
- `firmware/*/bsp/cubemx/` 下 CubeMX 生成的产物（`Core/`、`USB_DEVICE/`、`cmake/`、`Makefile`、`.mxproject`）禁止 AI 直接修改：下次 Generate 会被覆盖。`.claude/settings.json` 已对这些目录硬禁止 Edit/Write。
- 任何外设/时钟/中断/DMA 配置变更，AI 必须明确指出应在 CubeMX（或对应 `.ioc` 键）的哪个字段修改，由人工在 CubeMX 改后重新 Generate；严禁绕过 `.ioc` 直接改生成代码。
- 例外：`.ioc` 与手维护的链接脚本 `*.ld` 仅在用户明确要求时方可由 AI 编辑。
- 仅 `c_board`、`mc02` 使用 CubeMX；`ch32_board`、`rmcs_board` 不涉及本纪律（见各自 `AGENTS.md`）。

## 构建、测试与开发命令
Host SDK（纯 x86，任意机器可编）：
```bash
cmake --preset linux-debug -S host
cmake --build host/build
```

固件统一形态为 `cmake --preset <preset> -S firmware/<board>` + `cmake --build`。
各板的 preset、target、工具链环境变量见 `firmware/<board>/AGENTS.md`，常用示例：
```bash
cmake --preset debug -S firmware/rmcs_board  && cmake --build firmware/rmcs_board/build
cmake --preset debug -S firmware/c_board     && cmake --build firmware/c_board/build     --target c_board_app c_board_bootloader
cmake --preset debug -S firmware/mc02        && cmake --build firmware/mc02/build        --target mc02_app mc02_bootloader
cmake --preset debug -S firmware/ch32_board  && cmake --build firmware/ch32_board/build  --target ch32_board_app
```

Lint（与 CI 对齐）：
```bash
.scripts/clang-format-check --fix    # 应用格式修复
.scripts/clang-tidy-check            # 静态分析（需在 CMake build 之后运行）
```

`.scripts/clang-tidy-check --fix` 可触发 clang-tidy 自动修复，但部分修复可能不符合预期，需手动调整。
例如: int var 会被修复为 int const var. 但项目中使用的 Google 风格要求使用 const int var。

## 代码风格与命名规范
- 语言：C11 + C++23，禁用 GNU 扩展。
- 格式：4 空格缩进，100 列宽度限制，指针左对齐，启用 include 排序。
- 命名：Google 风格，但函数命名为小写下划线。
- 代码不允许包含任何非 ASCII 字符（Markdown 文档除外）。

## 测试指南
- 目前尚未启用 CTest/GTest 测试目标；当前 CI 质量门禁为：clang-format、clang-tidy 和 编译验证。
- 每次修改后，应在本地运行 lint 工具，并至少构建一个相关的构建目标。

## 提交指南
- Git unstaged changes 是必要的 code review 渠道。Agent 严禁执行 git add。 
- Commit Message 全英文，不允许包含任何非 ASCII 字符。
- 遵循仓库的 Conventional Commit 风格；破坏性变更使用 `!` 标记。
- 冒号后首字母需大写：`feat(scope): Capitalize the first letter of the title`。
- 每次提交应聚焦于单个模块或关注点。
