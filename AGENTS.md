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
| `ch32_board` | WCH CH32H417（Qingke V3F + V5F 双核） | RISC-V `cmake/toolchain-wch-riscv.cmake` | `ch32_board_app` `ch32_board_boot` `ch32_board_merged` | USB 3.0 SuperSpeed；`boot` 是 V3F 启动核兼 DFU bootloader |
| `rmcs_board` | HPM6E8Y / HPM5321（Andes） | RISC-V 超级构建 + HPMicro GNU 工具链 | `rmcs_board_app` `rmcs_board_bootloader`；EtherCAT 桥在 `ecat/` | HPM SDK v1.12.0，双核 ECAT 桥 |

> RISC-V 芯片（`ch32_board`、`rmcs_board`）用 `riscv32-unknown-elf-gcc`，**不要**用
> `arm-none-eabi-gcc`；后者只给 STM32（`c_board`、`mc02`）用。

## 开发机环境路径约定（重要，先读这条再看任何路径）

仓库文档里大量出现 `~/3rd_party/hpm`、`~/3rd_party/MRS_Toolchain_Linux_X64_V240`、
`~/3rd_party/wch-openocd` 这类**绝对路径**。关于它们：

- 这些是**上一台开发机上真实可用的安装位置**，不是示例、不是占位符。原样保留是为了
  让换机器的人有一份"确实跑通过"的配置可以照抄。
- **当前机器上的实际安装情况（2026-07-27 实测，本节此前写的"尚未安装"已过时）**：

  | 路径 | 本机 | 说明 |
  |---|---|---|
  | `~/3rd_party/hpm` | **已装** | HPM RISC-V 工具链 + hpm_sdk + openocd + Manufacturing Tool。已实测编出 `rmcs_board/ecat` 全部目标 |
  | `~/3rd_party/MRS_Toolchain_Linux_X64_V240` | **已装** | MounRiver，ch32_board 用；内含 OpenOCD |
  | `~/3rd_party/wch-openocd` | 不存在 | 已被 MRS 包内自带的 OpenOCD 取代，见 `firmware/ch32_board/AGENTS.md` |

- **不要凭本节旧措辞断言"本机没有工具链"——先 `ls` 确认。** 路径不存在时也不要删改
  文档里的路径，按下一条重新指向即可。
- 换到新机器时的正确做法：按对应 `firmware/<board>/AGENTS.md` 的依赖清单重新安装，
  再把 `GNURISCV_TOOLCHAIN_PATH`、`WCH_TOOLCHAIN_PATH`、`PATH` 等环境变量指向新的
  安装位置；文档里的路径只当参照。
- 工具链一律**留在仓库外**，不入库、不做 submodule。

各文档中出现机器相关路径的地方标注为 `[前机路径]`，看到这个标记就按本节理解。

## 文档规范（新增或修改任何 `.md` 前先读）

仓库文档分三层，**同一件事只在它所属的那一层写一次**，其余层引用而不复述：

| 层 | 文件 | 职责 | 硬规则 |
|---|---|---|---|
| L1 索引层 | 仓库根 `README.md` | 项目是什么、host SDK 上手、板子索引、指路 | 不承载芯片内部细节，只给链接 |
| L2 现行事实层 | 各目录 `AGENTS.md`（`CLAUDE.md` 软链） | 工具链、构建、烧录、目录结构、修改纪律 | **命令与约束的唯一权威来源** |
| L3 背景与过程层 | 板级 `README.md`、`PITFALLS.md`、`PROGRESS.md`、`DESIGN.md`、`*_REVERSE_ENGINEERING.md`、`*_ROADMAP*.md` 等 | 设计取舍、硬件事实、踩坑记录、实测数据、工作日志 | 可以很长很细；不重复 L2 的命令，改为引用 |

统一要求：

- **语言**：正文一律中文。代码、命令、寄存器名、文件路径、编译选项等标识符保持原文
  不译。仓库代码本身仍禁止非 ASCII 字符，该限制不适用于 Markdown 文档。
- **文档头**：每份 `.md` 开头用引用块给出四行元信息，供人和 AI 快速判断该不该读、
  内容还算不算数：

  ```markdown
  > **文档类型**：现行规范 / 背景说明 / 过程记录 / 硬件参考
  > **适用范围**：<哪块板、哪个模块>
  > **状态**：现行有效 / 历史记录（已被 xxx 接替，保留备查）
  > **相关文档**：[xxx](path) · [yyy](path)
  ```

- **摘要**：文档头之后写一节 `## 摘要`，两到四行说明"这份文档解决什么问题、读完能
  做什么"。超过 150 行的长文档再补一节 `## 本文导航`，列出各章要点。
- **结论前置**：踩坑类、实测类段落先给结论和判据，再给推导过程。
- **章节编号**：需要编号时用扁平的 `## 1.` / `### 1.1` 并保持顺序连续；不要用
  `1.1a`、`1.1b` 这类补丁式编号，也不要中文数字与阿拉伯数字混用。
- **不删除历史**：结论被推翻或方案被取代时，**不要删除旧文档**，而是把状态改成
  "历史记录"，写清被谁接替、哪些结论仍然成立。旧的排查过程本身就是资产。
- **来源标注**：区分"实测"、"手册"、"推断"三种来源，写在结论后的方括号里，例如
  `[实测]`、`[RM]`（芯片手册）、`[推断，未上板]`。

## 文档地图

全仓库现有文档一览（不含 `bsp/`、`third_party/` 下的第三方文档）：

**根目录**
- [README.md](README.md) — 项目介绍、host SDK 与固件的编译烧录上手
- [AGENTS.md](AGENTS.md) — 本文件，全仓库共享约束
- [ENV.md](ENV.md) — 外部工具下载入口速查

**c_board（STM32F407）**
- [firmware/c_board/AGENTS.md](firmware/c_board/AGENTS.md) — 工具链、构建、CubeMX 纪律

**mc02（STM32H723）**
- [firmware/mc02/AGENTS.md](firmware/mc02/AGENTS.md) — 工具链、构建、关键特性
- [firmware/mc02/README.md](firmware/mc02/README.md) — 外设、低延迟设计、CubeMX 重生成后的复原清单

**ch32_board（WCH CH32H417 双核）**
- [firmware/ch32_board/AGENTS.md](firmware/ch32_board/AGENTS.md) — 工具链、构建、烧录、GDB、现状
- [firmware/ch32_board/README.md](firmware/ch32_board/README.md) — 选型动机、app 布局、USB SS 数据通路、移植决策
- [firmware/ch32_board/PITFALLS.md](firmware/ch32_board/PITFALLS.md) — 踩坑记录，**上板前必读**
- [firmware/ch32_board/PROGRESS.md](firmware/ch32_board/PROGRESS.md) — 移植工作日志与待办

**rmcs_board（HPM6E8Y / HPM5321）**
- [firmware/rmcs_board/AGENTS.md](firmware/rmcs_board/AGENTS.md) — 工具链、两套镜像、构建
- [firmware/rmcs_board/BUILD_ENVIRONMENT.md](firmware/rmcs_board/BUILD_ENVIRONMENT.md) — 完整编译环境搭建
- [firmware/rmcs_board/ecat/README.md](firmware/rmcs_board/ecat/README.md) — EtherCAT 桥架构与验证
- [firmware/rmcs_board/ecat/DESIGN.md](firmware/rmcs_board/ecat/DESIGN.md) — EtherCAT 桥设计决策与延迟预算
- [firmware/rmcs_board/ecat/LINKX_HW_ACCEL_PLAN.md](firmware/rmcs_board/ecat/LINKX_HW_ACCEL_PLAN.md) — 硬件加速规划
- `firmware/rmcs_board/boards/hpm6e8y/` — 板级参考：`README.md`、`ECAT_BRIDGE_BRINGUP_NOTES.md`、
  `ETHERNET_PIN_REVERSE_ENGINEERING.md`、`GPIO_LED_REVERSE_ENGINEERING.md`、`CAN_PIN_REVERSE_ENGINEERING.md`
- `firmware/rmcs_board/boards/hpm5321/`、`hpm5321_dual_can/` — 板级 `README.md` 与 `PINOUT.md`

**host 传输层**
- [host/src/transport/igh/DESIGN.md](host/src/transport/igh/DESIGN.md) — IgH EtherCAT 传输实现规格
- [host/src/transport/igh/EVALUATION.md](host/src/transport/igh/EVALUATION.md) — 主站方案选型与实测对比
- [host/src/transport/igh/LATENCY_ROADMAP.md](host/src/transport/igh/LATENCY_ROADMAP.md) — 延迟优化路线 v1（历史，已被 v2 接替）
- [host/src/transport/igh/LATENCY_ROADMAP_V2.md](host/src/transport/igh/LATENCY_ROADMAP_V2.md) — 延迟优化全记录 v2（现行）
- [host/src/transport/soem/NIC_TUNING.md](host/src/transport/soem/NIC_TUNING.md) — 主站网卡与系统调优清单

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
