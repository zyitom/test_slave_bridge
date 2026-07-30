# 构建环境

> **文档类型**：现行规范
> **适用范围**：全仓库
> **状态**：现行有效
> **相关文档**：[README.md](README.md) · `firmware/<board>/AGENTS.md`

## 摘要

本机（Ubuntu 22.04.5 LTS）已配置完成，`host` / `firmware/ch32_board` 实测可编。

## 版本要求

| 组件 | 版本 | 安装方式 |
|---|---|---|
| CMake | ≥ 3.21 | Ubuntu 22.04 自带 3.22.1，无需额外安装 |
| Ninja | ≥ 1.10 | `sudo apt install ninja-build` |
| GCC (x86) | **≥ 14** | 见下方 |
| ARM GCC | **15.3.rel1** | `arm-none-eabi-gcc`，c_board / mc02，见下方 |
| RISC-V GCC (HPM) | 13.2.0 | rmcs_board，见下方 |
| RISC-V GCC (WCH) | 15.2.0 | ch32_board，MounRiver，见下方 |
| clang-format / clang-tidy | ≥ 16 | `sudo apt install clang-format-20 clang-tidy-20` |
| libusb-1.0-dev | ≥ 1.0 | `sudo apt install libusb-1.0-0-dev` |
| dfu-util | ≥ 0.9 | `sudo apt install dfu-util` |

## GCC 14（host SDK）

host SDK 用到 C++23 `<print>`（GCC 14+），Ubuntu 22.04 自带只到 12：

```bash
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install -y gcc-14 g++-14
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 140 --slave /usr/bin/g++ g++ /usr/bin/g++-14
```

## ARM GCC（c_board / mc02）

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
export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm
```

详见 `firmware/rmcs_board/BUILD_ENVIRONMENT.md`。

### ch32_board（WCH CH32H417）

从 MounRiver 下载 WCH 工具链：<https://www.mounriver.com/download>

详见 `firmware/ch32_board/AGENTS.md`。

## clang-format / clang-tidy

```bash
sudo apt install clang-format-20 clang-tidy-20
sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-20 200
sudo update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-20 200
```

> `main` 分支本身就有存量格式漂移，lint 在干净 checkout 上也 fail。
> 判断标准用**增量对比 HEAD**，不看绝对数字。

## 实测可编

```bash
# host SDK
cmake --preset linux-debug -S host && cmake --build host/build

# ch32_board
export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm
cmake --preset debug -S firmware/ch32_board && cmake --build firmware/ch32_board/build
```

> `RAM: 100.00%` 是正常的，不是溢出。判断是否撑爆看 FLASH。
