# rmcs_board 固件编译环境

本文档说明编译 `firmware/rmcs_board` 下两套固件所需的环境。两套固件**共用同一
条 RISC-V 工具链和同一份随仓库自带的 HPM SDK**,区别只在源码目录与构建命令:

| 固件 | 源码目录 | 传输 | 典型用途 |
|---|---|---|---|
| USB 数据固件 | `firmware/rmcs_board/`(超级构建) | USB vendor 类(bulk) | USB 板 / USB 回环对比测试 |
| EtherCAT 桥固件 | `firmware/rmcs_board/ecat/` | EtherCAT(ARQ 流) | 当前 6e8y 上跑的镜像 |

> 注意:两者是**独立镜像**,同一时刻只能刷其中一个;刷 USB 固件会覆盖 EtherCAT
> 桥,测完 USB 需要再刷回 ecat。

---

## 1. 依赖清单

| 组件 | 版本 / 型号 | 是否随仓库 | TL101 现状 |
|---|---|---|---|
| CMake | >= 3.13(实测 3.28.3 可用) | 否(系统装) | 已装 |
| Ninja | 任意近版(实测 1.11.1) | 否 | 已装 |
| Python | 3.x(实测 3.12.3) | 否 | 已装 |
| Python 包 | `PyYAML`、`jinja2` | 否(pip 装) | 需确认 |
| HPM SDK | v1.11.0 | **是**(`bsp/hpm_sdk`) | 随仓库 |
| RISC-V 工具链 | `rv32imac_zicsr_zifencei_multilib_b_ext`(HPMicro GNU) | **否** | **缺** |
| 烧录工具 | HPMicro Manufacturing Tool v0.6.0(DFU) | 否 | 见 §5 |

**结论:TL101 上目前缺 RISC-V 工具链**(`~/3rd_party/hpm` 不存在,PATH 里无
`riscv32-unknown-elf-gcc`),所以固件的实际编译/烧录需在**已装工具链的开发 PC**
上完成。Host 端 SDK / examples(纯 x86)可以在 TL101 上编。

---

## 2. 一次性安装

### 2.1 RISC-V 工具链(不是 ARM)

> **HPM6E8Y / HPM5321 是 RISC-V 芯片(Andes 核),不是 ARM。** 因此用
> `riscv32-unknown-elf-gcc`,**不要用 `arm-none-eabi-gcc`**。arm-gcc 只在编
> STM32 的 `firmware/c_board` 时才需要。

下载带 B 扩展 multilib 的 HPMicro GNU RISC-V 工具链(版本 2023.10.18):

```
https://github.com/hpmicro/riscv-gnu-toolchain/releases/download/2023.10.18/rv32imac_zicsr_zifencei_multilib_b_ext-linux.tar.gz
```

**这是预编译二进制工具,不是源码——不要放进仓库或 git submodule。** 解压到
仓库外的固定路径(沿用现有约定 `~/3rd_party/hpm/`):

```bash
mkdir -p ~/3rd_party/hpm && tar -xzf rv32imac_zicsr_zifencei_multilib_b_ext-linux.tar.gz -C ~/3rd_party/hpm
```

解压后应得到 `~/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux/bin/riscv32-unknown-elf-gcc` 等。

> 对比:`bsp/hpm_sdk` 能随仓库是因为它是**源码**(SDK);工具链是**二进制工具**,
> 留在仓库外,仅用 `GNURISCV_TOOLCHAIN_PATH` 引用。

### 2.2 Python 包

```bash
pip3 install PyYAML jinja2
```

### 2.3 HPM SDK

无需单独安装——已随仓库 vendored 在 `firmware/rmcs_board/bsp/hpm_sdk`。

---

## 3. 环境变量(每个新终端都要设)

```bash
cd <repo-root>
export GNURISCV_TOOLCHAIN_PATH="/home/<user>/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux"
export PATH="${GNURISCV_TOOLCHAIN_PATH}/bin:$PATH"
export HPM_SDK_BASE="$(pwd)/firmware/rmcs_board/bsp/hpm_sdk"
```

验证:

```bash
riscv32-unknown-elf-gcc --version   # 应打印工具链版本
echo "$HPM_SDK_BASE"                 # 应指向 bsp/hpm_sdk
```

---

## 4. 编译

### 4.1 USB 数据固件(超级构建,BOARD 可选 6e8y)

```bash
# 配置:BOARD 决定目标板与 USB PID(6e8y=0xA904,见 boards/hpm6e8y/CMakeLists.txt)
cmake --preset debug -S firmware/rmcs_board -DBOARD=hpm6e8y
cmake --build firmware/rmcs_board/build
```

产物 ELF/BIN/DFU 名字里带 BOARD(如 `rmcs_board_app_hpm6e8y.*`)。

> 提示:`BOARD` 是自由字符串,`BOARD_SEARCH_PATH` 默认指向 `boards/`,只要
> `boards/hpm6e8y/` 存在即可用 `-DBOARD=hpm6e8y`。超级构建 `CMakeLists.txt` 的
> `STRINGS` 下拉列表目前只列了 `hpm5321 / hpm5321_dual_can`,那只是 GUI 提示,
> 不影响命令行;把 `hpm6e8y` 加进该列表纯属可读性优化。

### 4.2 EtherCAT 桥固件(当前 6e8y 镜像)

EtherCAT 版**额外需要 Beckhoff SSC 生成代码**(许可限制不入库),首次构建前须按
`firmware/rmcs_board/ecat/README.md` 用 ETG 账号下载 SSC Tool 生成并运行
`ecat/tools/import_ssc.sh`。之后:

```bash
cd firmware/rmcs_board/ecat
cmake -G Ninja -S . -B build_hpm6e8y \
  -DBOARD=hpm6e8y \
  -DBOARD_SEARCH_PATH="${PWD}/../boards" \
  -DRV_ARCH=rv32imac -DRV_ABI=ilp32 \
  -DCMAKE_BUILD_TYPE=release
ninja -C build_hpm6e8y rmcs_ecat_bootloader rmcs_ecat_core0
```

产物:
```
build_hpm6e8y/rmcs_ecat_bootloader/output/rmcs_board_bootloader_hpm6e8y.bin
build_hpm6e8y/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu
```

---

## 5. 烧录(简述)

用 HPMicro Manufacturing Tool(DFU 方式),把板子的 USB0 接开发 PC,上电进
bootloader(`lsusb` 见 `a11c:a904` 表示 bootloader 在跑),再用 `.dfu` 包烧录。
FCFG / 详细步骤见 `boards/hpm6e8y/README.md`。

---

## 6. Host 端(x86,可在 TL101 上编)

固件之外的上位机 SDK 与 examples 不需要 RISC-V 工具链:

```bash
cmake --preset linux-debug -S host -DLIBRMCS_ENABLE_IGH=ON
cmake --build host/build
```

USB 回环对比测试用现成的 `host/build/examples/can_loopback_latency`(走
`common/multi_board` 的 USB API);EtherCAT 侧用 `ecat_canfd_stress` /
`ecat_stream_latency`(默认后端已改为 IgH)。

---

## 附:最小检查清单

- [ ] `riscv32-unknown-elf-gcc --version` 能跑
- [ ] `$HPM_SDK_BASE` 指向 `bsp/hpm_sdk`
- [ ] `pip3 show PyYAML jinja2` 均已安装
- [ ] (仅 EtherCAT 版)已运行 `import_ssc.sh` 生成 SSC 代码
- [ ] BOARD 选对(`hpm6e8y`),USB PID 对应 `0xA904`
