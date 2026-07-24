# rmcs_board 固件指南

> 本目录专属指南，叠加在仓库根 `AGENTS.md` 之上。完整编译环境（依赖清单、工具链下载、烧录）见本目录 `BUILD_ENVIRONMENT.md`，此处只列 agent 关键点。

## 芯片与工具链
- MCU：**HPM6E8Y / HPM5321**（HPMicro，**Andes RISC-V 核，不是 ARM**）；HPM6E8Y 双核。
- ISA/工具链：RISC-V，用 HPMicro GNU 工具链 `rv32imac_zicsr_zifencei_multilib_b_ext`（带 B 扩展 multilib），需 `riscv32-unknown-elf-gcc`。
- 工具链是仓库外预编译二进制，约定放 `~/3rd_party/hpm/`（**不要**入库或做 submodule）。
- HPM SDK **v1.11.0 随仓库自带**（`bsp/hpm_sdk`，submodule），无需另装。

## 两套独立镜像
| 固件 | 源码目录 | 传输 | 说明 |
|---|---|---|---|
| USB 数据固件 | `firmware/rmcs_board/`（超级构建） | USB vendor bulk | USB 板 / 回环对比 |
| EtherCAT 桥固件 | `firmware/rmcs_board/ecat/` | EtherCAT（ARQ 流） | 当前 6e8y 上跑的镜像 |

> 二者是独立镜像，**同一时刻只能刷其中一个**；刷 USB 固件会覆盖 EtherCAT 桥，测完需再刷回 ecat。

## 构建
```bash
export PATH=~/3rd_party/hpm/bin:$PATH        # 确保 riscv32-unknown-elf-gcc 可见
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
