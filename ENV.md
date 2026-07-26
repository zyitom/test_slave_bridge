# 构建环境

本机（Ubuntu 22.04.5 LTS）已配置完成，`host` / `firmware/ch32_board` 均实测可编。
本文只记**踩过的坑和为什么是这个版本**；纯粹的安装步骤见文末一键脚本。

> 板级细节另见 `firmware/<board>/AGENTS.md`；rmcs_board 的 HPM SDK / EtherCAT 额外
> 依赖见 `firmware/rmcs_board/BUILD_ENVIRONMENT.md`。

## 现有版本（2026-07-26 实测）

| 组件 | 版本 | 来源 |
|---|---|---|
| CMake | **3.31.12** | Kitware apt 源（**已 hold**，见下） |
| Ninja | 1.10.1 | jammy 自带 |
| GCC (x86) | **14.3.0** | `ppa:ubuntu-toolchain-r/test` |
| clang-format / clang-tidy | **20.1.8** | apt.llvm.org（jammy-20） |
| RISC-V GCC | **13.2.0** | HPMicro 预编译包，见下 |
| dfu-util | 0.9 | jammy 自带 |
| libusb-1.0-dev | 1.0.25 | jammy 自带 |

## 四个必须知道的坑

### 1. CMake 必须是 3.x，**不能升 4.x**

`firmware/common/bsp/tinyusb/src/CMakeLists.txt` 和 `c_board/bsp/cubemx/...` 里写着
`cmake_minimum_required(VERSION 3.0/2.8)`，而 **CMake 4.0 起直接拒绝 < 3.5** 的声明，
c_board 会当场编不了。Kitware 源默认给的是 4.4，所以这里**钉在 3.31.12 并 `apt-mark hold`**：

```bash
apt-mark showhold          # 应看到 cmake / cmake-data
```

要升级 cmake 前先确认 tinyusb 那几个文件已经改过，否则别动。

### 2. 仓库声明的 `cmake_minimum_required(VERSION 3.28)` 是虚高的

实际用到的最高特性只有 `CMAKE_CXX_STANDARD 23` 和 `cmake_path()`，两者都是 **3.20**；
`cmake --preset` 因为用了 `toolchainFile` 需要 preset schema v3 = **3.21**。
`set(CMAKE_CXX_SCAN_FOR_MODULES OFF)` 看着像 3.28 专用，其实在旧版上就是个无害的无效变量。

实测：把 host 的 floor 改成 3.20、preset 降到 `"version": 3`，**Ubuntu 自带的 cmake 3.22.1
能完整 Configuring/Generating done**。所以 3.28 那个数字可以放心降到 `3.21...3.28`
（区间写法，不会把 CMP0126/0128/0135/0141/0155 这些策略默认值一起退回旧行为）。
目前**没有改**，只是记下来。

### 3. x86 侧必须 GCC ≥ 14（jammy 自带的 11/12 都不行）

host SDK 用到 C++23 的 `<print>`（**GCC 14+**）和 `<format>`（GCC 13+），
core 的协程 `LifoTask` 在 GCC 11 的 `std::__n4861` 上会报
`operator new ... not usable with the function signature`。jammy 最高只有 g++-12，
所以走 `ppa:ubuntu-toolchain-r/test` 装 14。

默认编译器通过 `update-alternatives` 切换，**11 和 14 都还在**，可随时切回：

```bash
sudo update-alternatives --config gcc     # g++ 是 slave，跟着一起切
```

### 4. clang-format 至少 16，但**没有任何版本能让仓库 lint 全绿**

- `.clang-format` 里的 `AlignTrailingComments: {Kind:, OverEmptyLines:}` 和
  `InsertNewlineAtEOF` 是 **clang-format 16** 才有的；15 直接报
  `Invalid argument / unexpected scalar` 连配置都读不了。
- 但更重要的是：**`main` 本身就有存量格式漂移**。实测同一份 pristine HEAD：

  | clang-format | 有问题的文件数 |
  |---|---|
  | 16 / 17 | 更多 |
  | 18 | 37 |
  | **19 / 20 / 21** | **35** |

  也就是说 `.scripts/clang-format-check` 现在在**干净的 main 上就是 fail 的**，
  跟你改没改代码无关。这里选 20（并列最少 + 版本较新）。

- clang-tidy 同理：本机 20.1.8 跑出 **296 个 warning-as-error**，其中 **261 个集中在
  `host/src/transport/igh/reference/`**——而那个目录 `host/CMakeLists.txt` 里明确写着
  "documentation, not a build target" 并从库里 `list(FILTER ... EXCLUDE)` 掉了。
  lint 脚本的收集范围没跟着排除，属于存量问题。

  **结论：改完代码看 lint，要跟 HEAD 对比增量，不要看绝对数字。**

## RISC-V 工具链（ch32_board / rmcs_board 共用）

**不是 ARM。** 用 `riscv32-unknown-elf-gcc`，`arm-none-eabi-gcc` 只给 STM32
（c_board / mc02）用。

```bash
# 130 MB，解压到仓库外的固定路径
curl -fL -o ~/Downloads/riscv-hpm.tar.gz \
  https://github.com/hpmicro/riscv-gnu-toolchain/releases/download/2023.10.18/rv32imac_zicsr_zifencei_multilib_b_ext-linux.tar.gz
mkdir -p ~/3rd_party/hpm && tar -xzf ~/Downloads/riscv-hpm.tar.gz -C ~/3rd_party/hpm
```

得到 `~/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux/bin/riscv32-unknown-elf-gcc`
（GCC **13.2.0**，支持 C++23）。**每个新终端都要设**：

```bash
export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm
```

> MounRiver（<https://www.mounriver.com/download>）那套 `MRS_Toolchain_*` 也能编 ch32，
> 但同一份代码用它的 GCC15 编出来 app 会从 86.5% 涨到 95.0% FLASH 占用（区域只有 128 KB）。
> 默认仍用上面这套。烧录用的 OpenOCD 目前只在 MounRiver 包里有，见
> `firmware/ch32_board/AGENTS.md`。

## 实测可编（2026-07-26）

```bash
# host SDK
cmake --preset linux-debug -S host && cmake --build host/build
# -> host/build/liblibrmcs-sdk.a

# ch32_board（app + boot + merged.hex）
export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm
cmake --preset debug -S firmware/ch32_board && cmake --build firmware/ch32_board/build
# -> ch32_board_app.elf / ch32_board_boot.elf / ch32_board_merged.hex / ch32_board_app.dfu
```

ch32 app 占用参考（debug）：FLASH **87.84% / 128 KB**，RAM_CODE 77.68%。

> **`RAM: 100.00%` 是正常的，不是溢出。** 那个区域被链接脚本按用满设计，
> pristine HEAD 上同样显示 100.00%（app 261376/261376、boot 458496/458496）。
> 判断有没有撑爆看 **FLASH** 那一行。

## 本机做过的系统改动（如需还原）

```bash
# apt 源
/etc/apt/sources.list.d/kitware.list                        # cmake
/etc/apt/sources.list.d/llvm18.list  llvm20.list            # clang 工具
/etc/apt/sources.list.d/ubuntu-toolchain-r-ubuntu-test-jammy.list   # gcc-14

# 默认程序（都是 update-alternatives，可逆）
gcc/g++ -> 14   (11 仍在)
clang-format/clang-tidy -> 20  (15/18 仍在)

# 版本钉子
apt-mark hold cmake cmake-data      # 防止被升到 4.x
```

## 一键复现（新机器）

```bash
sudo apt-get update
sudo apt-get install -y ninja-build libusb-1.0-0-dev dfu-util pkg-config \
    python3-pip software-properties-common ca-certificates gpg wget

# cmake 3.31（不要 4.x）
wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor \
  | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' \
  | sudo tee /etc/apt/sources.list.d/kitware.list
# clang 20
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor \
  | sudo tee /usr/share/keyrings/llvm-archive-keyring.gpg >/dev/null
echo 'deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] http://apt.llvm.org/jammy/ llvm-toolchain-jammy-20 main' \
  | sudo tee /etc/apt/sources.list.d/llvm20.list
# gcc 14
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update

CMV=3.31.12-0kitware2ubuntu22.04.1
sudo apt-get install -y cmake=$CMV cmake-data=$CMV gcc-14 g++-14 clang-format-20 clang-tidy-20
sudo apt-mark hold cmake cmake-data
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 110 --slave /usr/bin/g++ g++ /usr/bin/g++-11
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 140 --slave /usr/bin/g++ g++ /usr/bin/g++-14
sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-20 200
sudo update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-20 200
```

RISC-V 工具链按上面那一节单独装。
