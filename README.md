# LibRMCS

librmcs 是 [无下位机控制系统 RMCS（RoboMaster Control System）](https://github.com/Alliance-Algorithm/RMCS) 的核心通讯部分。

## LibRMCS v3

LibRMCS v3 正在开发！目前处于测试阶段，无良好的文档/教程。

您可以访问 [v2](https://github.com/Alliance-Algorithm/librmcs/tree/v2) 分支获取 LibRMCS v2 的最新 stable 版本。

## Host SDK 编译

### 依赖

- `gcc` / `g++` >= 14（需要 C++23 `<print>` 支持，GCC 13 不包含该头文件）
- `cmake` >= 3.28 与 `ninja`
- `libusb-1.0-dev`

Ubuntu 22.04 默认 GCC 版本较低，需手动安装：

```bash
sudo apt install g++-14 gcc-14 libusb-1.0-0-dev
```

### 编译

```bash
cmake --preset linux-debug -S host
cmake --build host/build
```

如需同时构建示例程序（`rx_monitor`、`uart_stress` 等），加上 `-DBUILD_EXAMPLES=ON`：

```bash
cmake --preset linux-debug -S host -DBUILD_EXAMPLES=ON
cmake --build host/build --target rx_monitor
```

如果系统默认 GCC 版本低于 14，需手动指定编译器：

```bash
cmake --preset linux-debug -S host -DBUILD_EXAMPLES=ON \
    -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_C_COMPILER=gcc-14
```

## 固件编译与烧录

### 依赖

- `arm-none-eabi-gcc`（C11 + C++23 工具链）
- `cmake`（>= 3.28）与 `ninja`
- `dfu-util`（>= 0.11，用于 USB DFU 烧录）

### 编译

以 mc02（STM32H723VG）为例，在仓库根目录执行：

```bash
cmake --preset debug -S firmware/mc02
cmake --build firmware/mc02/build --target mc02_app mc02_bootloader
```

c_board（STM32F407VG）同理，把路径换成 `firmware/c_board`、目标换成 `c_board_app c_board_bootloader` 即可。

产物：

- App：`firmware/<board>/build/app/<board>_app.elf` / `.bin` / `.dfu`
- Bootloader：`firmware/<board>/build/bootloader/<board>_bootloader.elf`

`debug` 可替换为 `release`。

### 烧录 Bootloader（首次或更新引导）

Bootloader 位于 Flash 起始地址 `0x08000000`，需要用调试器（ST-Link / J-Link）烧录一次，例如：

```bash
# 使用 OpenOCD（示例，按你的调试器调整 interface/target）
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
    -c "program firmware/mc02/build/bootloader/mc02_bootloader.elf verify reset exit"
```

App 之后即可通过下面的 DFU 流程烧录，无需调试器。

### 烧录 App（USB DFU）

App 镜像 `*.dfu` 已带好镜像哈希与 DFU 后缀，`dfu-util` 会按 VID/PID 自动匹配设备。

1. **让设备进入 DFU 模式**，任选其一：
   - 复位时按住 **KEY** 键（mc02 为 PA15，低电平有效）；
   - Flash 中没有有效 App 时，Bootloader 会自动停在 DFU 模式；
   - 由上位机软件发起 DFU 重启请求（App 运行时触发）。

2. **确认设备已枚举**（应能看到 `[a11c:d402]`，接口为 alt 0 `Internal Flash`）：

   ```bash
   dfu-util -l
   ```

3. **烧录**（`-a 0` 选择 Internal Flash 接口）：

   ```bash
   dfu-util -a 0 -D firmware/mc02/build/app/mc02_app.dfu
   ```

   若同时接了多个 DFU 设备，加 `-d 0xa11c:0xd402` 指定目标。

烧录完成后 Bootloader 会校验镜像并自动复位跳转到 App。此时 `dfu-util` 可能打印设备掉线/状态读取失败之类的提示，属于正常现象（设备自行复位了）。

### 各板 USB 标识

| 板型       | 芯片         | App PID  | Bootloader 产品名          |
| ---------- | ------------ | -------- | -------------------------- |
| c_board    | STM32F407VG  | `0xD401` | `RMCS DFU Bootloader`      |
| mc02       | STM32H723VG  | `0xD402` | `RMCS DFU Bootloader`      |
| ch32_board | WCH CH32H417 | `0xD403` | `RMCS Bootloader v<版本号>` |

VID 均为 `0xA11C`。

### ch32_board 的差异

RISC-V 双核，工具链是 `riscv32-unknown-elf-gcc`（不是 `arm-none-eabi-gcc`），
编译前需 `export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm`：

```bash
cmake --preset debug -S firmware/ch32_board && cmake --build firmware/ch32_board/build
```

- **首次烧录**（含 bootloader）用 WCH-Link 烧 `build/ch32_board_merged.hex`，
  之后 App 可走 DFU：`./flash-ch32.sh`。
- Bootloader 就是 V3F 启动核镜像本身（`ch32_board_boot`），"启动 App" 是唤醒 V5F
  而非跳转。
- App 的 `.dfu` 只带 DFU 后缀、**不带** `ImageHash` 后缀：这块板的 bootloader 会
  自己哈希烧进去的内容并写进独立的 metadata 记录。

细节见 `firmware/ch32_board/README.md`。
