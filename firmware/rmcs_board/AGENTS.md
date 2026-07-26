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
- HPM SDK **v1.11.0 随仓库自带**（`bsp/hpm_sdk`，submodule），无需另装。

## 两套独立镜像
| 固件 | 源码目录 | 传输 | 说明 |
|---|---|---|---|
| USB 数据固件 | `firmware/rmcs_board/`（超级构建） | USB vendor bulk | USB 板 / 回环对比 |
| EtherCAT 桥固件 | `firmware/rmcs_board/ecat/` | EtherCAT（ARQ 流） | 当前 6e8y 上跑的镜像 |

> 二者是独立镜像，**同一时刻只能刷其中一个**；刷 USB 固件会覆盖 EtherCAT 桥，测完需再刷回 ecat。

## 构建
```bash
export PATH=~/3rd_party/hpm/bin:$PATH        # [前机路径] 确保 riscv32-unknown-elf-gcc 可见
# USB 数据固件（超级构建，含 app + bootloader）
cmake --preset debug -S firmware/rmcs_board
cmake --build firmware/rmcs_board/build       # target: rmcs_board_app / rmcs_board_bootloader
# EtherCAT 桥固件
cmake --preset debug -S firmware/rmcs_board/ecat
cmake --build firmware/rmcs_board/ecat/build
```
- preset：`debug` / `debug-outside` / `release`（注意本板 `CMAKE_BUILD_TYPE` 用小写 `debug`/`release`）。
- 构建需 Python 3 + `PyYAML`、`jinja2`（HPM SDK 代码生成用）。

## 目录结构
- `app/`、`bootloader/`：USB 固件两半。`ecat/`：EtherCAT 桥（独立超级构建 `rmcs_ecat_superbuild`）。
- `boards/`、`common/`：板级配置与共享代码。`bsp/hpm_sdk`：HPM SDK submodule（第三方，只读）。
- 无 CubeMX，不适用 CubeMX 纪律。

## 备注
- 缺工具链的机器（如 build server）只能编 host SDK；固件需在装了 RISC-V 工具链的 PC 上编/烧。详见 `BUILD_ENVIRONMENT.md`。
