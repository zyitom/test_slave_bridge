# 构建环境

> **文档类型**：现行规范
> **适用范围**：全仓库
> **状态**：现行有效
> **相关文档**：[README.md](README.md) · `firmware/<board>/AGENTS.md`

## 摘要

本文件是全仓库依赖清单与安装命令；具体某台机器装了什么、装到哪，不在本文件
断言，见根 [AGENTS.md 开发机环境路径约定](AGENTS.md#开发机环境路径约定重要先读这条再看任何路径)。
本文档的 apt 包名假定 **Ubuntu 24.04（Noble）**；若在 22.04 等更早版本上配置，
先核对对应包名/版本是否仍在仓库里（尤其是 GCC 14，见下）。

## 版本要求

| 组件 | 版本 | 安装方式 |
|---|---|---|
| CMake | ≥ 3.21 | Ubuntu 24.04 自带 3.28，无需额外安装 |
| Ninja | ≥ 1.10 | `sudo apt install ninja-build` |
| GCC (x86) | **≥ 14** | Ubuntu 24.04 自带 gcc-14，见下方 |
| ARM GCC | `gcc-arm-none-eabi`（apt，13.2.rel1）或官方 15.3.rel1 二进制 | c_board / mc02，见下方 |
| RISC-V GCC (HPM) | 13.2.0 | rmcs_board，见下方 |
| RISC-V GCC (WCH) | 15.2.0 | ch32_board，MounRiver，见下方 |
| clang-format / clang-tidy | ≥ 16 | `sudo apt install clang-format-20 clang-tidy-20` |
| libusb-1.0-dev | ≥ 1.0 | `sudo apt install libusb-1.0-0-dev` |
| dfu-util | ≥ 0.9 | `sudo apt install dfu-util` |
| Python 包 | `PyYAML`、`jinja2` | `sudo apt install python3-yaml python3-jinja2`（rmcs_board 代码生成用） |

## GCC 14（host SDK）

host SDK 用到 C++23 `<print>`（GCC 14+）。**Ubuntu 24.04 自带 gcc-14**，直接装
即可，不需要额外 PPA：

```bash
sudo apt-get install -y gcc-14 g++-14
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 140 --slave /usr/bin/g++ g++ /usr/bin/g++-14
```

> 仅 **Ubuntu 22.04 及更早**（自带 GCC 只到 12）才需要额外加
> `ppa:ubuntu-toolchain-r/test` 再装 gcc-14；24.04 上加这个 PPA 是多余的一步。

## ARM GCC（c_board / mc02）

优先用 apt 装（Ubuntu 24.04 自带 13.2.rel1，已实测编过 c_board / mc02）：

```bash
sudo apt-get install -y gcc-arm-none-eabi
```

如需官方更新版本（如追某个具体 bugfix），可改用 Arm 官方二进制：

```bash
curl -fL -o ~/Downloads/arm-gnu-toolchain.tar.xz \
  https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel1/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi.tar.xz
mkdir -p ~/3rd_party && tar -xf ~/Downloads/arm-gnu-toolchain.tar.xz -C ~/3rd_party
export PATH="$HOME/3rd_party/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi/bin:$PATH"
```

## RISC-V GCC

两块板用两套工具链，**非常不建议混用**。

### rmcs_board（HPM6E8Y / HPM5321）

```bash
curl -fL -o ~/Downloads/riscv-hpm.tar.gz \
  https://github.com/hpmicro/riscv-gnu-toolchain/releases/download/2023.10.18/rv32imac_zicsr_zifencei_multilib_b_ext-linux.tar.gz
mkdir -p ~/3rd_party/hpm && tar -xzf ~/Downloads/riscv-hpm.tar.gz -C ~/3rd_party/hpm
export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux
```

> **`GNURISCV_TOOLCHAIN_PATH` 必须直接指向含 `bin/` 的那一层**，即解压出来的
> `rv32imac_zicsr_zifencei_multilib_b_ext-linux/` 目录本身，**不是**它的上级
> `~/3rd_party/hpm`。HPM SDK 的 `cmake/toolchain.cmake` 不会往下多找一层，指错
> 会在 configure 阶段报 `It was unable to find the toolchain`
> （`CROSS_COMPILE: .../hpm/bin/riscv32-unknown-elf-` 这种缺一层的路径就是这个症状）[实测]。
> ch32_board 的 `toolchain-wch-riscv.cmake` 对同一个环境变量做了通配符搜索
> （见下方 ch32_board 小节），能容忍指到上级目录，但 rmcs_board **不能**，
> 两者对这个变量的解析方式不同，别以为通用。

详见 `firmware/rmcs_board/BUILD_ENVIRONMENT.md`。

### ch32_board（WCH CH32H417）

**默认直接复用上面装好的 HPM 工具链**，不需要单独装（两者共享
`riscv32-unknown-elf-` 前缀，见 `firmware/ch32_board/AGENTS.md`）——同一个
`GNURISCV_TOOLCHAIN_PATH` 即可，ch32_board 的 `toolchain-wch-riscv.cmake` 会自动
探测。只有需要 MounRiver 自带 GCC15（如追求更小 FLASH 占用）时才去
<https://www.mounriver.com/download> 单独下载，见 `firmware/ch32_board/AGENTS.md`。

## clang-format / clang-tidy

```bash
sudo apt install clang-format-20 clang-tidy-20
sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-20 200
sudo update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-20 200
```

> `main` 分支本身就有存量格式漂移，lint 在干净 checkout 上也 fail。
> 判断标准用**增量对比 HEAD**，不看绝对数字。

## 实测可编 [2026-08-04]

```bash
# host SDK
cmake --preset linux-debug -S host && cmake --build host/build

export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux
export PATH="${GNURISCV_TOOLCHAIN_PATH}/bin:$PATH"

# ch32_board（用的就是上面这条 HPM 工具链，不是单独一套）
cmake --preset debug -S firmware/ch32_board && cmake --build firmware/ch32_board/build

# rmcs_board（USB 数据固件）
cmake --preset debug -S firmware/rmcs_board && cmake --build firmware/rmcs_board/build

# c_board / mc02（用系统 apt 装的 gcc-arm-none-eabi，不需要上面的 RISC-V 变量）
cmake --preset debug -S firmware/c_board && cmake --build firmware/c_board/build --target c_board_app c_board_bootloader
cmake --preset debug -S firmware/mc02    && cmake --build firmware/mc02/build    --target mc02_app mc02_bootloader
```

> `RAM: 100.00%` 是正常的，不是溢出。判断是否撑爆看 FLASH。
>
> `mc02` 首次 clone 后如果报 "Cannot find source file...stm32h7xx_hal_fdcan.c"，
> 是 `firmware/mc02/bsp/stm32h7xx-hal-driver`、`cmsis-device-h7` 两个子模块只注册
> 未拉取（`git submodule status` 输出以 `-` 开头就是这个状态），补一次
> `git submodule update --init` 即可，与工具链无关[实测]。
