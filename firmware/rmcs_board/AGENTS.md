# rmcs_board 固件指南

> **文档类型**：现行规范（板级）
> **适用范围**：`firmware/rmcs_board/`，HPMicro HPM6E8Y / HPM5321（Andes RISC-V）
> **状态**：现行有效
> **相关文档**：[仓库根 AGENTS.md](../../AGENTS.md) · [BUILD_ENVIRONMENT.md](BUILD_ENVIRONMENT.md)（完整环境搭建） · [ecat/README.md](ecat/README.md)（EtherCAT 桥）

> 本目录专属指南，叠加在仓库根 `AGENTS.md` 之上。完整编译环境（依赖清单、工具链下载、烧录）见本目录 `BUILD_ENVIRONMENT.md`，此处只列 agent 关键点。

## 摘要

rmcs_board 与其他三块板最大的不同：它是 **RISC-V（Andes 核）**、用 **HPM SDK 超级构建**，
而且同一块板上有**两套互斥的固件镜像**（USB 数据固件 / EtherCAT 桥），刷一个会覆盖另一个。
工具链是仓库外的预编译二进制，装在哪由环境变量决定——本机若未安装属正常，见
[仓库根 AGENTS.md 开发机环境路径约定](../../AGENTS.md#开发机环境路径约定重要先读这条再看任何路径)。

## 芯片与工具链
- MCU：**HPM6E8Y / HPM5321**（HPMicro，**Andes RISC-V 核，不是 ARM**）；HPM6E8Y 双核。
- ISA/工具链：RISC-V，用 HPMicro GNU 工具链 `rv32imac_zicsr_zifencei_multilib_b_ext`（带 B 扩展 multilib），需 `riscv32-unknown-elf-gcc`。
- 工具链是仓库外预编译二进制，约定放 `~/3rd_party/hpm/`（`[前机路径]`，**不要**入库或做 submodule）。
- HPM SDK **v1.12.0 随仓库自带**（`bsp/hpm_sdk`，submodule），无需另装。submodule 指向
  fork `zyitom/hpm_sdk`（含 tinyusb vendor class / ZLP / full-speed 三处本地补丁），
  当前提交 `v1.12.0-3-ge4347411`，分支 `migrate-v1.12.0`。

## 三套独立镜像
| 固件 | 源码目录 / 开关 | 传输 | 说明 |
|---|---|---|---|
| USB 数据固件（单核） | `firmware/rmcs_board/`（超级构建，默认） | USB vendor bulk | **USB 延迟最低（p50 100us）**，core1 不释放 |
| EtherCAT 桥（旧布局） | `firmware/rmcs_board/ecat/` | EtherCAT（ARQ 流） | core0=SSC+USB / core1=CAN，迁移前的对照基线 |
| 核对调布局（新） | `firmware/rmcs_board/` + `-DLIBRMCS_RELEASE_CORE1=ON`，core1 源码在 `ecat/core1_ecat/` | EtherCAT + USB，可切换 | core0=USB+CAN+协议栈 / core1=EtherCAT。见 [ecat/CORE_SWAP_MIGRATION.md](ecat/CORE_SWAP_MIGRATION.md) |

> 三者是独立镜像，**同一时刻只能刷其中一个**；刷任一套会覆盖另外两套。

> **选型要点（实测 2026-08-01，别凭直觉选）**：USB 的 p50 只在 **core1 完全不运行**时是
> 100us；只要 core1 被释放（无论跑什么、也无论 USB 是否经过跨核环）就变成 ~120us，这是
> 双核争用的固定代价。所以 **EtherCAT 与"100us 的 USB"不可兼得**：
> 主力是 USB 就用单核镜像，需要 EtherCAT（或需要两者共存可切换）就用核对调布局。
> 完整数据与被推翻的假设见 [CORE_SWAP_MIGRATION.md](ecat/CORE_SWAP_MIGRATION.md) 4.6 节。

## 构建
```bash
export PATH=~/3rd_party/hpm/bin:$PATH        # [前机路径] 确保 riscv32-unknown-elf-gcc 可见
# USB 数据固件（超级构建，含 app + bootloader）
cmake --preset debug -S firmware/rmcs_board
cmake --build firmware/rmcs_board/build       # target: rmcs_board_app / rmcs_board_bootloader
# EtherCAT 桥固件（旧布局）
cmake --preset debug -S firmware/rmcs_board/ecat
cmake --build firmware/rmcs_board/ecat/build
# 核对调布局（core0=USB+CAN / core1=EtherCAT）。先建 core1 再建 core0，超级构建已排序
cmake --preset release -S firmware/rmcs_board -DBOARD=hpm6e8y -DLIBRMCS_RELEASE_CORE1=ON
cmake --build firmware/rmcs_board/build --target rmcs_board_app
```
- preset：`debug` / `debug-outside` / `release`（注意本板 `CMAKE_BUILD_TYPE` 用小写 `debug`/`release`）。
- 构建需 Python 3 + `PyYAML`、`jinja2`（HPM SDK 代码生成用）。

## 目录结构
- `app/`、`bootloader/`：USB 固件两半。`app/src/xcore/`：核对调布局下 core0 的跨核部分
  （环主机侧、次核装载、flash RPC 服务端），单核构建下整体编译为空。
  `app/generated/`：core1 镜像的 C 数组落点（构建产物，入库，同 `ecat/core0/src/sec_core_img.c` 惯例）。
- `ecat/`：EtherCAT 桥（独立超级构建 `rmcs_ecat_superbuild`）。`ecat/core1_ecat/`：核对调布局的
  core1 镜像（纯 EtherCAT）。`ecat/common/`：跨核契约（`xcore_channel.hpp` 带版本号，
  core0/core1 镜像必须同版本，不匹配时 core1 会停机而不是静默错乱）。
- `boards/`、`common/`：板级配置与共享代码。`bsp/hpm_sdk`：HPM SDK submodule（第三方，只读）。
- 无 CubeMX，不适用 CubeMX 纪律。

## 备注
- 缺工具链的机器（如 build server）只能编 host SDK；固件需在装了 RISC-V 工具链的 PC 上编/烧。详见 `BUILD_ENVIRONMENT.md`。
