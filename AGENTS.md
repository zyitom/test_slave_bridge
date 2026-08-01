# 仓库指南

> **文档类型**：现行规范（全仓库共享约束）
> **适用范围**：整个仓库，所有目录、所有芯片
> **状态**：现行有效
> **相关文档**：[README.md](README.md)（面向使用者的上手说明） · 各芯片 `firmware/<board>/AGENTS.md`

> 本文件是全仓库共享的 agent 指南。为兼容不同工具（Claude Code 读 `CLAUDE.md`，
> Codex 等读 `AGENTS.md`），`CLAUDE.md` 是指向本文件的符号链接——只维护
> `AGENTS.md` 一份即可，两边同步。各芯片子目录下另有专属 `AGENTS.md`（同样带
> `CLAUDE.md` 软链），会在处理该目录代码时叠加加载，不影响本文件的共享约束。

## 摘要

本文件规定全仓库共享的三件事：**目录职责**（代码放哪）、**构建与检查流程**（怎么编、
怎么过 CI）、**修改纪律**（哪些文件不能碰、提交怎么写）。芯片专属的工具链、外设、
烧录细节不在这里，在各自的 `firmware/<board>/AGENTS.md`。

---

## 项目结构与模块组织
- `firmware/`：各板卡固件，详见下方“多芯片固件总览”。每块板一个子目录，含独立的 `CMakePresets.json` 与专属 `AGENTS.md`。
- `firmware/*/bsp/`：厂商/子模块依赖；除非有意更新子模块，否则视为第三方代码。

## 多芯片固件总览
每块板都复用 `core/` 与 host SDK，只在 `firmware/<board>/` 下放板级固件。芯片专属
的工具链路径、外设、烧录方式、已知坑等，写在各自的 `firmware/<board>/AGENTS.md`。

| 板子 | MCU | ISA / 工具链 | 构建 target | 备注 |
|---|---|---|---|---|
| `c_board` | STM32F4xx | ARM `cmake/gcc-arm-none-eabi.cmake` | `c_board_app` `c_board_bootloader` | CubeMX BSP + TinyUSB |
| `mc02` | STM32H723VGT6（M7） | ARM `cmake/gcc-arm-none-eabi.cmake` | `mc02_app` `mc02_bootloader` | CAN-FD，USB Full-Speed |
| `ch32_board` | WCH CH32H417（Qingke V3F + V5F 双核） | RISC-V `cmake/toolchain-wch-riscv.cmake` | `ch32_board_app` `ch32_board_boot` `ch32_board_merged` | USB 3.0 SuperSpeed；`boot` 是 V3F 启动核兼 DFU bootloader |
| `rmcs_board` | HPM6E8Y / HPM5321（Andes） | RISC-V 超级构建 + HPMicro GNU 工具链 | `rmcs_board_app` `rmcs_board_bootloader`；EtherCAT 桥在 `ecat/` | HPM SDK v1.12.0，双核 ECAT 桥 |

> RISC-V 芯片（`ch32_board`、`rmcs_board`）用 `riscv32-unknown-elf-gcc`，**不要**用
> `arm-none-eabi-gcc`；后者只给 STM32（`c_board`、`mc02`）用。

## 开发机环境路径约定（重要，先读这条再看任何路径）

仓库文档里大量出现 `~/3rd_party/hpm`、`~/3rd_party/MRS_Toolchain_Linux_X64_V240`、
`~/3rd_party/wch-openocd` 这类**绝对路径**。关于它们：

- 这些是**上一台开发机上真实可用的安装位置**，不是示例、不是占位符。原样保留是为了
  让换机器的人有一份"确实跑通过"的配置可以照抄。
- **当前机器上的实际安装情况（2026-08-01 逐条 `ls` 复核，取代 2026-07-27 那份，
  后者对本机已不准确）**：

  | 路径 | 本机 | 说明 |
  |---|---|---|
  | `~/3rd_party/rv32imac_zicsr_zifencei_multilib_b_ext-linux` | **已装** | **RISC-V 工具链的真实位置**（gcc 13.2.0）。已实测编出 `rmcs_board` 单核 / 核对调 / `ecat` 全部目标 |
  | `~/3rd_party/hpm` | 部分 | **只有 `HPMicro_Manufacturing_Tool_v0.6.0`**。此前记的"工具链 + hpm_sdk + openocd 都在这里"对本机不成立 |
  | OpenOCD | **未装** | 全机没有。HPM 板的日常烧录走 USB DFU（`dfu-util`），不需要它；只有给空片烧 bootloader 才需要 |
  | `~/3rd_party/MRS_Toolchain_Linux_X64_V240` | **不存在** | 本机没有 MounRiver，`ch32_board` 在本机编不了 |
  | `~/3rd_party/wch-openocd` | 不存在 | 见 `firmware/ch32_board/AGENTS.md` |

  由此，本机可用的构建/烧录组合是：**host SDK + `rmcs_board` 全部固件 + DFU 烧录**；
  `ch32_board` 缺工具链，板级调试缺 JTAG。**缺 JTAG 不等于看不到现场**——
  `rmcs_board` 已有两条走 USB 的带内诊断通道，见
  `firmware/rmcs_board/AGENTS.md`「板上没有调试器时怎么看现场」。

- **不要凭本节旧措辞断言"本机没有工具链"——先 `ls` 确认。** 路径不存在时也不要删改
  文档里的路径，按下一条重新指向即可。
- 换到新机器时的正确做法：按对应 `firmware/<board>/AGENTS.md` 的依赖清单重新安装，
  再把 `GNURISCV_TOOLCHAIN_PATH`、`WCH_TOOLCHAIN_PATH`、`PATH` 等环境变量指向新的
  安装位置；文档里的路径只当参照。
- 工具链一律**留在仓库外**，不入库、不做 submodule。

各文档中出现机器相关路径的地方标注为 `[前机路径]`，看到这个标记就按本节理解。

## 文档规范
新增或修改任何 `.md` 前先读 `.claude/skills/doc-convention/SKILL.md`（Claude Code 按需加载，
其他工具直接读该文件）：三层文档模型、四行文档头、摘要与导航、章节编号、来源标注、
历史文档处理。

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
cmake --preset debug -S firmware/ch32_board  && cmake --build firmware/ch32_board/build  # app + boot + merged.hex
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
- 格式：以 `.clang-format` 为准，由 `.scripts/clang-format-check` 强制。
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
