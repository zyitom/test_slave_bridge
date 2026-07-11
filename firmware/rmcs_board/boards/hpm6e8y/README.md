# hpm6e8y 板子适配完整记录

> HPM6E80 固件运行在 HPM6E00 芯片上，hpm6e00evk 等效硬件，
> XPI NOR Flash 使用 FCFG option 1 (`0xfcf90002`)。

> EtherCAT/CAN bring-up 的最新避坑记录见
> [`ECAT_BRIDGE_BRINGUP_NOTES.md`](ECAT_BRIDGE_BRINGUP_NOTES.md)。该文档记录了
> hpm6e8y 内置 PHY、ESC port 映射反向、clean normal 固件、以及 normal CAN0
> 实际为 MCAN4/PZ00-PZ01 的结论。

---

## 目录

1. [背景](#背景)
2. [OTP 分析](#otp-分析)
3. [FCFG：Flash 配置的命门](#fcfgflash-配置的命门)
4. [Linker Script 与 Flash 内存布局](#linker-script-与-flash-内存布局)
5. [烧录地址偏移陷阱（第一次失败根因）](#烧录地址偏移陷阱第一次失败根因)
6. [正确烧录步骤](#正确烧录步骤)
7. [HPM6E80 vs HPM6E00](#hpm6e80-vs-hpm6e00)
8. [安全启动与密钥](#安全启动与密钥)
9. [调试与救砖](#调试与救砖)
10. [完整避坑清单](#完整避坑清单)

---

## 背景

- **芯片**: HPM6E00（USB PID `34b7:0006`，Boot ROM 模式）
- **板子**: hpm6e00evk 等效硬件，使用**外部 XPI NOR Flash**（16MB）
- **固件**: 来自 `rmcs_board/ecat` 的 EtherCAT Bridge 双核应用
- **原始 board**: `hpm6e80ivm1`（FCFG `0xfcf90001`，option 0）
- **新 board**: `hpm6e8y`（FCFG `0xfcf90002`，option 1）

---

## OTP 分析

### OTP 是什么

OTP（One-Time Programmable）是芯片内部的一次性可编程存储器。物理上每个 bit
出厂时全是 `1`，编程时把指定 bit 烧成 `0`，**永远无法从 `0` 恢复为 `1`**。
所以 OTP 只能「追加」烧录，已有内容无法擦除或修改。

### 本芯片 OTP 内容

使用 `hpm_manufacturing_cmd` 读取（memory_id=`0x20000`）：

```
OTP 大小: 512 bytes (128 words × 4 bytes)
USB VID:PID: 34b7:0006 → HPM6E00
ROM Version: V1.2.0
SoC LifeCycle: 0x04 (已配置，非空白)
```

关键 OTP 字段：

| Word | 值 | 含义 |
|------|-----|------|
| 0 | `0x30400016` | Device Config: boot_mode=6 (Primary Boot + ISP fallback) |
| 1 | `0x10000001` | 扩展配置 |
| 8-11 | 16 bytes | UUID/Unique ID |
| 21 | `0x03A008CC` | Flash 配置相关 |
| 25 | `0x00000006` | 编程计数/配置标志 |
| 32-54 | 92 bytes | **SRK Hash**（Super Root Key，安全启动验签用） |
| 69-75 | ASCII | `"usb_en_ec2canfd by damiao"` — 开发者注释 |
| 77 | `0x00000001` | **OTP_PROGRAM_COMPLETE**（OTP 编程完成标志） |
| 88-91 | 16 bytes | **EXIP KEK**（外部镜像加密密钥） |

### 安全状态详解

```
Word 0 = 0x30400016 = 0b_0011_0000_0100_0000_0000_0000_0001_0110

Bit[3:0]  BOOT_MODE         = 0x6  → Primary Boot (失败回退 ISP)
Bit[4]    DBG_AUTH          = 1    → 调试认证使能（但不锁）
Bit[5]    DBG_PORT_LOCK     = 0    → JTAG/SWD 未锁定 ✓
Bit[8]    SECURE_BOOT_EN    = 0    → 安全启动未开启 ✓
Bit[9]    ENCRYPTED_BOOT_EN = 0    → 加密启动未开启 ✓
```

> **关键结论**: 芯片虽然 OTP 已编程（有 SRK Hash、EXIP KEK），但 **两个
> 安全开关都没打开**（SECURE_BOOT_EN=0, DBG_PORT_LOCK=0）。这意味着：
> - J-Link 可以正常连接调试
> - 可以不签名烧录任意固件
> - USB ISP 烧录不受限制
>
> 形象比喻：锁和钥匙都装好了，但门没锁。

---

## FCFG：Flash 配置的命门

### 什么是 FCFG

FCFG（Flash Configuration）是一个 3-4 word 的配置表，告诉 ROM 如何初始化
外部 XPI NOR Flash：

```
Word 0: OPT_HDR  — 选项头 (0xfcf9XXXX)
Word 1: OPT_OPT0 — 引脚组选择 (0x00000007 = group 7)
Word 2: OPT_OPT1 — 时序参数   (0x00001000 或 0x00000000)
```

`OPT_HDR` 的末位数字决定使用哪个「选项集」：
- `0xfcf90001` → option 0
- `0xfcf90002` → option 1

不同选项集对应不同的 Flash 引脚映射和时序配置。
**同一款 SoC、同一块板子、同一颗 Flash 芯片，如果引脚布线不同，
就需要不同的 option。**

### 本板子的 FCFG 发现过程

1. `board.h`（来自 `hpm6e80ivm1`）定义了 `BOARD_APP_XPI_NOR_CFG_OPT_HDR = 0xfcf90001`
2. 但执行 `config-memory 0x10000 0x200` 时 **失败**：
   ```
   [Critical] ROM response failed! status: 1001006
              message: configuring memory failed
   ```
3. 换成 `0xfcf90002` 后 **成功** → 本板子需要 option 1

### 验证 FCFG 的方法

**不要直接烧录！先验证 FCFG 是否匹配：**

```bash
# 1. 写 FCFG 到 ILM
sudo ./hpm_manufacturing_cmd -u -f "HPM6E00,0" \
  -r "write-memory 0x0 0x200 [[0xfcf90002,0x00000007,0x00001000,0x0]]"

# 2. 尝试配置 XPI NOR（这一步会暴露 FCFG 是否正确）
sudo ./hpm_manufacturing_cmd -u -f "HPM6E00,0" \
  -r "write-memory 0x0 0x200 [[0xfcf90002,0x00000007,0x00001000,0x0]]" \
  -r "config-memory 0x10000 0x200"

# 如果 config-memory 返回 OK → FCFG 正确
# 如果返回 configuring memory failed → FCFG 不匹配，需要换 option
```

---

## Linker Script 与 Flash 内存布局

### 两个 Linker Script

这个项目有两个 linker script，分别用于 bootloader 和 application：

**Bootloader: `flash_xip.ld`**
```
__nor_cfg_option_load_addr__ = 0x80000000 + 0x400   = 0x80000400
__boot_header_load_addr__    = 0x80000000 + 0x1000  = 0x80001000
__app_load_addr__            = 0x80000000 + 0x3000  = 0x80003000
```

Flash 物理布局：
```
0x80000000 ┌──────────────────────────┐
           │ (空 0x400 bytes)          │  ← ROM 不读这里
0x80000400 ├──────────────────────────┤
           │ .nor_cfg_option           │  ← FCFG 表，ROM 从这里读
           │ (kOption[4])              │
0x80001000 ├──────────────────────────┤
           │ .boot_header              │  ← 启动头 + 签名信息
0x80003000 ├──────────────────────────┤
           │ .start / _start           │  ← 第一条指令
           │ .vectors (LMA, 复制到ILM) │
           │ .text                     │
           │ .data (LMA)               │
           │ .fast (LMA)               │
0x80015xxx ├──────────────────────────┤  ← bootloader 结束 (~86KB)
           │ (padding 到 128KB)        │
0x80020000 ├──────────────────────────┤
           │ UF2 Bootloader 保留区     │
           │ (128KB, 存放 bootloader)  │
           └──────────────────────────┘
```

**Application: `flash_uf2.ld`**
```
FLASH ORIGIN = 0x80000000 + UF2_BOOTLOADER_RESERVED_LENGTH
             = 0x80000000 + 0x20000
             = 0x80020000
```

Application Flash 布局（在 0x80020000 之后）：
```
0x80020000 ┌──────────────────────────┐
           │ UF2 Signature (4 bytes)   │  ← "HPM\n" (0x0A4D5048)
0x80020004 ├──────────────────────────┤
           │ _start / .start           │  ← 入口点
           │ .vectors (LMA, 复制到ILM) │
           │ .text                     │
           │ .data (LMA)               │
0x80043xxx ├──────────────────────────┤  ← app 结束 (~166KB)
           │ (free space)              │
0x80200000 ├──────────────────────────┤
           │ ECAT EEPROM emulation     │  ← EtherCAT 用 Flash 模拟 EEPROM
           │ (2MB)                     │
           └──────────────────────────┘
```

### objcopy -O binary 的行为

`riscv32-unknown-elf-objcopy -O binary` 从**最低的 Flash VMA** 开始输出：

- Bootloader: 最低 Flash LOAD 段在 `0x80000400` → **binary 偏移 0 = Flash 地址 0x80000400**
- Application: 最低 Flash LOAD 段在 `0x80020000` → **binary 偏移 0 = Flash 地址 0x80020000**

---

## 烧录地址偏移陷阱（第一次失败根因）

### 错误操作

```bash
# ✗ 错误！bootloader binary 被写到 0x80000000
write-memory 0x10000 0x80000000 bootloader.bin
```

### 为什么失败

Bootloader binary 从偏移 0 开始就是 FCFG 表（`.nor_cfg_option` 的内容）。
这个 FCFG 表在 flash 里的**正确位置是 `0x80000400`**。

写到 `0x80000000` 导致：
```
实际:  0x80000000: [FCFG 表]  ← ROM 不读这里
       0x80000400: [boot_header 的后半段]  ← ROM 来这里找 FCFG，读到垃圾
       0x80000C90: [.start 代码]  ← ROM 在错误位置找启动头
```

ROM 启动流程：
1. 用 OTP 或默认引脚配置尝试读取 Flash
2. 去 `0x80000400` 找 FCFG 表 → **读到的不是 FCFG（偏移错了）**
3. 无法正确配置 Flash → 回退到 **ISP 模式**
4. USB 设备显示 `34b7:0006`（Boot ROM Open）

### 正确操作

```bash
# ✓ 正确！bootloader binary 写到 0x80000400
write-memory 0x10000 0x80000400 bootloader.bin

# ✓ Application 写到 0x80020000
write-memory 0x10000 0x80020000 demo.bin
```

### 如何确认自己有没有踩这个坑

芯片重新上电后，用 `lsusb` 检查：
- `34b7:0006` → Boot ROM 模式，**启动失败**，回退 ISP
- `34b7:0A904`（或你在 CMakeLists.txt 设的 PID）→ **启动成功**，bootloader 在运行

---

## 正确烧录步骤

### 环境

```bash
cd /home/zyi/3rd_party/hpm/HPMicro_Manufacturing_Tool_v0.6.0
export GNURISCV_TOOLCHAIN_PATH="/home/zyi/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux"
export PATH="${GNURISCV_TOOLCHAIN_PATH}/bin:$PATH"
```

### 编译

```bash
cd /home/zyi/Desktop/librmcs/firmware/rmcs_board/ecat

# 配置（只需一次）
cmake -G Ninja -S . -B build_hpm6e8y \
  -DBOARD=hpm6e8y \
  -DBOARD_SEARCH_PATH="${PWD}/../boards" \
  -DRV_ARCH=rv32imac \
  -DRV_ABI=ilp32 \
  -DCMAKE_BUILD_TYPE=release

# 编译
ninja -C build_hpm6e8y rmcs_ecat_bootloader rmcs_ecat_core0
```

产物：
```
build_hpm6e8y/rmcs_ecat_bootloader/output/rmcs_board_bootloader_hpm6e8y.bin  (86KB)
build_hpm6e8y/rmcs_ecat_core0/output/demo.bin                                (166KB)
build_hpm6e8y/rmcs_ecat_core0/output/rmcs_ecat_bridge_hpm6e8y.dfu           (DFU 包)
```

### 验证 FCFG（烧录前必做）

```bash
# 读 bootloader 的前 4 字节确认 FCFG
python3 -c "
with open('build_hpm6e8y/rmcs_ecat_bootloader/output/rmcs_board_bootloader_hpm6e8y.bin', 'rb') as f:
    opt = int.from_bytes(f.read(4), 'little')
print(f'FCFG OPT_HDR: 0x{opt:08X}')
assert opt == 0xfcf90002, 'FCFG 错误！'
print('✓ FCFG 正确')
"
```

### 烧录

```bash
# sudo 密码是一个空格
echo ' ' | sudo -S ./hpm_manufacturing_cmd -u -f "HPM6E00,0" \
  -r "write-memory 0x0 0x200 [[0xfcf90002,0x00000007,0x00001000,0x0]]" \
  -r "config-memory 0x10000 0x200" \
  -r "write-memory 0x10000 0x80000400 /path/to/bootloader.bin" \
  -r "write-memory 0x10000 0x80020000 /path/to/demo.bin"
```

命令解释：
| 命令 | 作用 |
|------|------|
| `-u -f "HPM6E00,0"` | USB 连接 + 加载未签名 blfw 到 RAM |
| `write-memory 0x0 0x200 [[...]]` | 写 FCFG 到 ILM `0x200` |
| `config-memory 0x10000 0x200` | 用 ILM `0x200` 的 FCFG 初始化 XPI NOR |
| `write-memory 0x10000 0x80000400` | **写 bootloader 到正确偏移** |
| `write-memory 0x10000 0x80020000` | 写 application |

### 验证烧录

```bash
echo ' ' | sudo -S ./hpm_manufacturing_cmd -u -f "HPM6E00,0" \
  -r "write-memory 0x0 0x200 [[0xfcf90002,0x00000007,0x00001000,0x0]]" \
  -r "config-memory 0x10000 0x200" \
  -r "read-memory 0x10000 0x80000400 256 /tmp/verify.bin"

# 对比
diff <(xxd /tmp/verify.bin) \
     <(xxd -l 256 /path/to/bootloader.bin) && echo "✓ 验证通过"
```

### 测试启动

重新上电后检查 USB 设备：
```bash
lsusb | grep 34b7
# 期望: 34b7:0A904 (bootloader DFU 模式)
# 失败: 34b7:0006 (Boot ROM，说明启动失败回退 ISP)
```

---

## HPM6E80 vs HPM6E00

### 本质差异

| | HPM6E80 | HPM6E00 |
|---|---|---|
| **RISC-V 核心** | 相同 | 相同 |
| **外设地址** | 相同 | 相同 |
| **引脚电气** | 相同（同封装） | 相同 |
| **内置 Flash** | 4MB (XPI0 QSPI) | 4MB (XPI0 QSPI) |
| **SDK 路径** | `soc/HPM6E00/HPM6E80/` | `soc/HPM6E00/HPM6E80/` |

> SDK 对 HPM6E00 和 HPM6E80 使用**同一套头文件和 linker script**，
> 目录名是 `HPM6E00/HPM6E80`。两者核心+外设完全一致。

### ⚠️ HPM6E00 的 Flash 对 J-Link 来说是 QSPI

虽然 Flash 是内置的（封装在芯片内部），但它走的是 **XPI0 → QSPI 协议**，
不是传统 MCU 那种挂在 AHB 总线上的 embedded Flash。对 J-Link 来说，
它看起来就像一片外部 QSPI NOR Flash，需要专门的编程算法。

### 为什么 HPM6E80 固件能跑在 HPM6E00 上

1. 两种芯片都通过 XPI0 接口访问内置 Flash，CPU 地址都是 `0x80000000`
2. 外设寄存器地址相同
3. 引脚 mux 相同
4. 中断向量表布局相同

**编译时 target 为 HPM6E80、实际芯片为 HPM6E00 不影响运行**，
因为两者从 XPI0 的角度完全一样。

---

## 安全启动与密钥

### 当前状态

```
SECURE_BOOT_EN    = 0  → 不验证固件签名，任意固件可启动
DBG_PORT_LOCK     = 0  → J-Link 可连接
DBG_AUTH          = 1  → 调试认证使能（未锁，实际不影响）
LifeCycle         = 0x04 → 芯片已配置

SRK Hash  已写入 OTP (Words 32-54, 92 bytes)
EXIP KEK  已写入 OTP (Words 88-91, 16 bytes)
```

### 潜在风险

如果将来有人把 `SECURE_BOOT_EN` 烧成 1：
- ROM 会验证固件签名
- 未签名固件无法启动
- 必须用对应私钥签名才能烧录

如果同时把 `DBG_PORT_LOCK` 也烧成 1：
- J-Link 完全断开
- 只能用 **USB-HID ISP**（Boot ROM 模式）烧录
- 但仍然可以烧录未签名固件（只要 SECURE_BOOT_EN 没开）

### 救砖底线

无论 OTP 怎么配置，只要芯片物理上没坏，**把 BOOT_MODE 引脚
（PA02/PA03）强制设为 ISP 模式（PA02=1, PA03=0），上电后芯片
必定进入 Boot ROM ISP 模式**。这是硬件级后门，无法被任何 OTP
设置关闭。

---

## 调试与救砖

### BOOT_MODE 引脚

| PA02 | PA03 | 启动模式 |
|------|------|---------|
| 0 | 0 | 按 OTP Word 0 配置 |
| 0 | 1 | Serial Boot (UART) |
| **1** | **0** | **ISP Boot (USB-HID)** ← 救命模式 |
| 1 | 1 | Primary Boot (XPI Flash) |

### 常见问题

| 症状 | 原因 | 解决 |
|------|------|------|
| USB 显示 `34b7:0006` | Flash 启动失败，回退 ISP | 检查 FCFG + 烧录地址 |
| `config-memory` 失败 | FCFG OPT_HDR 不匹配硬件 | 换 option（0 和 1 之间切换） |
| 烧录后芯片没反应 | bootloader 偏移错误 | 确认写到 `0x80000400` 不是 `0x80000000` |
| J-Link 连不上 | DBG_PORT_LOCK=1 | 用 BOOT_MODE 引脚进 ISP 救砖 |

### 读取 Flash 当前内容（排错用）

```bash
echo ' ' | sudo -S ./hpm_manufacturing_cmd -u -f "HPM6E00,0" \
  -r "write-memory 0x0 0x200 [[0xfcf90002,0x00000007,0x00001000,0x0]]" \
  -r "config-memory 0x10000 0x200" \
  -r "read-memory 0x10000 0x80000000 4096 /tmp/flash_dump.bin"
```

### 读取 OTP（排错用）

```bash
echo ' ' | sudo -S ./hpm_manufacturing_cmd -u \
  -r "query-rte 0" \
  -r "query-rte 4 0x20000" \
  -r "read-memory 0x20000 0x0 512 /tmp/otp_dump.bin"
```

---

## PMP 与 J-Link：为什么能烧录 ELF 但不能擦除 Flash

### 问题现象

```text
J-Link> erase
Erasing device...
J-Link: Flash download: Only internal flash banks will be erased.
To enable erasing of other flash banks like QSPI or CFI,
it needs to be enabled via "exec EnableEraseAllFlashBanks"

J-Link> exec EnableEraseAllFlashBanks   # 启用 QSPI 擦除
J-Link> erase
****** Error: Timeout while waiting for core to halt after reset
****** Error: Failed to preserve target RAM @ 0x00000000-0x0001FFFF.
Failed to prepare for programming.
ERROR: Erase returned with error code -1.
```

但烧录 ELF 却正常：

```text
J-Link> loadfile demo.elf
Downloading... 166072 bytes @ 0x80020000  ← OK
```

### 根因：两条不同的访问路径

```
┌─────────────────────────────────────────────────────────────────┐
│                        J-Link 调试探针                           │
├─────────────────────┬───────────────────────────────────────────┤
│  SBA 路径 (System   │  CPU 执行路径 (Flash Loader)               │
│  Bus Access)        │                                           │
│                     │                                           │
│  J-Link → SBA →     │  J-Link → ILM (0x00000000)               │
│  直接读写内存地址     │  下载 Flash Loader 代码                   │
│  ↓                  │  → 唤醒 CPU                               │
│  XPI0 控制器        │  → CPU 执行:                               │
│  ↓                  │    - 往 XPI0 命令寄存发 QSPI Erase 指令     │
│  Flash 读写          │    - 轮询 busy flag                       │
│                     │    - 等待擦除完成                          │
│                     │                                           │
│  不走 CPU            │  必须走 CPU                                │
│  不受 PMP 限制       │  受 PMP 限制!                              │
│  只能做简单读写       │  能做复杂时序 (Erase, Program)             │
└─────────────────────┴───────────────────────────────────────────┘
```

**烧录 ELF 走 SBA 路径**：
- SBA 直接往 `0x8002xxxx`（Flash 地址空间）写数据
- XPI0 控制器自动把 AHB 写事务转成 QSPI Page Program
- 全程不碰 ILM (`0x00000000`)，PMP 管不到

**擦除走 CPU 路径**：
- J-Link 需要把一段 Flash Loader 代码下载到 ILM
- 芯片的 `board_init_pmp()` 已经把 ILM 区域锁了
- J-Link 写 ILM 失败 → `Failed to preserve target RAM @ 0x00000000-0x0001FFFF`

### PMP 是你们自己代码配的

```c
// board.c:115 — 不是 HPM SDK 的问题，是你自己的 PMP 配置
void board_init_pmp(void) {
    pmp_entry_t pmp_entry[16] = {0};

    // 第一条: 全部放行 (NAPOT, 覆盖整个地址空间)
    pmp_entry[0].pmp_addr = 0xFFFFFFFF;
    pmp_entry[0].pmp_cfg.val =
        PMP_CFG(READ_EN, WRITE_EN, EXECUTE_EN, ADDR_MATCH_NAPOT, REG_UNLOCK);

    // 后续: 锁定 non-cacheable 区域、SHARE_RAM 等
    ...
    pmp_config(&pmp_entry[0], index);
}
```

PMP 规则本身没问题（第一个 entry 全部放行），但 PMP 一旦使能，
J-Link 的 SBA 视角和 CPU 视角产生不一致，Flash Loader 下载就失败了。

### 解决方法

#### 方法 A：Reset + Halt（最简单）

```
J-Link> r                    # reset & halt（在第一条指令前停住）
J-Link> exec EnableEraseAllFlashBanks
J-Link> erase
```

`r` 让 CPU 停在 `_start` 的第一条指令，此时 `board_init()` 还没跑，
PMP 还没配置，ILM 完全开放，Flash Loader 可以正常下载。

#### 方法 B：用垃圾 ELF 覆盖 PMP 代码（不需要改接线）

核心原理：**利用 J-Link 能 SBA 写 Flash 的能力，把 bootloader 里
调用 PMP 的代码段用 0x00 或 NOP 覆盖掉。**

```
1. 编一个 "垃圾 ELF"：
   - 链接地址覆盖 bootloader 的 PMP 相关区域 (0x80003000-0x80004xxx)
   - 内容全 0x00 或非法指令
   
2. J-Link 烧这个 ELF → SBA 直接覆盖 Flash 对应扇区
3. 复位 → bootloader 被破坏，PMP 配不了 → ILM 开放
4. 此时 J-Link 可以 erase chip
5. 或者芯片掉回 Boot ROM (PID 34b7:0006) → USB 制造工具可用
6. 用制造工具重新烧录正确的 bootloader
```

实际操作更简单：直接新建一个只有 `.start` 段、链接到 `0x80000400`
的 ELF，内容全零，覆盖掉 FCFG 表 + boot header。

```c
// dummy_main.c — 最小 ELF，只用来破坏 PMP
__attribute__((section(".start"), used))
void _start(void) { while(1); }
```

链接脚本让它覆盖 `0x80000400` 开始的关键区域。烧进去后芯片复位，
bootloader 找不到有效代码 → BOOT ROM ISP 接管 → USB 制造工具可用。

#### 方法 C：BOOT_MODE 引脚进 Boot ROM（硬件方式）

PA02=1, PA03=0 → 上电 → 芯片不进你的固件 → PMP 不存在 → J-Link 随便擦。

---

## 完整避坑清单

### 坑 1：FCFG OPT_HDR 不匹配
- **现象**: `config-memory` 返回 `configuring memory failed`
- **根因**: `board.h` 里 `0xfcf90001` 不匹配板子的 Flash 引脚布线
- **解决**: 两个 option 都试一次（`0xfcf90001` 和 `0xfcf90002`），
  找到能通过 `config-memory` 的那个

### 坑 2：Bootloader 烧录偏移错误
- **现象**: 烧录后芯片仍显示 `34b7:0006`（Boot ROM），无法从 Flash 启动
- **根因**: `flash_xip.ld` 把 `.nor_cfg_option` 放在 `0x80000400`，
  binary 偏移 0 对应 Flash `0x80000400`，但被写到了 `0x80000000`
- **解决**: 永远用 `0x80000400` 作为 bootloader 的写入地址

### 坑 3：混淆 load-image 和 write-memory
- `load-image`：加载固件到 **RAM** 执行（ISP 模式，掉电丢失）
- `write-memory 0x10000`：写入 **XPI NOR Flash**（持久化，上电自启）
- 两个命令用途不同，不可混用

### 坑 4：忘记验证 FCFG 就烧录
- 应该先用 `config-memory` 验证 FCFG 能否初始化 Flash
- 验证通过后再 `write-memory` 写入
- 否则会在 Flash 上留下一份配错 FCFG 的 bootloader

### 坑 5：GTK 库缺失导致 GUI 启动失败
- `hpm_manufacturing_gui` 依赖 GTK 库，在无桌面的 Linux 上不可用
- 用 `hpm_manufacturing_cmd` 命令行工具替代

### 坑 6：sudo 权限
- Linux 下访问 USB-HID 设备需要 root 权限
- 自动化脚本：`echo ' ' | sudo -S ./hpm_manufacturing_cmd ...`
- 注意：`-S` 从 stdin 读密码，本机器的 sudo 密码是一个空格

### 坑 7：blfw 加载后设备重连
- `-f "HPM6E00,0"` 加载 blfw 后设备会断开再重连
- 连续多次调用工具时不需要每次 `-f`（设备已在 blfw 模式下）
- 但如果设备断电重来，就需要重新 `-f`

### 坑 8：OTP 烧录不可逆
- OTP 只能从 1→0，不能从 0→1
- 烧录前务必确认值正确
- 建议先用 `read-memory 0x20000` 读取当前 OTP，确认空位再写

### 坑 9：不要把 app binary 和 bootloader binary 搞混
- Bootloader（`flash_xip.ld`）：写到 `0x80000400`
- Application（`flash_uf2.ld`）：写到 `0x80020000`
- 两个 linker script 的 FLASH 基址不同！

### 坑 10：调试认证 (DBG_AUTH) 可能影响 J-Link
- 本芯片 `DBG_AUTH=1` 但 `DBG_PORT_LOCK=0`，J-Link 仍可用
- 如果某天 `DBG_PORT_LOCK` 被烧成 1，J-Link 完全不可用
- 这时候只能用 BOOT_MODE 引脚进入 ISP 来救砖

### 坑 11：PMP 使能后 J-Link 能烧不能擦
- **现象**: `loadfile demo.elf` 成功，`erase` 失败
- **根因**: 烧录走 SBA 直写 Flash 地址（绕过 PMP）；擦除需要
  下载 Flash Loader 到 ILM → 被 PMP 拦截
- **解决**: `J-Link> r`（reset+halt）在 PMP 配置前停住 CPU，
  然后执行 erase；或者用 BOOT_MODE 引脚进 ISP

---

## 参考文件路径

```
固件:
  /home/zyi/Desktop/librmcs/firmware/rmcs_board/ecat/

Board 文件夹 (hpm6e8y):
  /home/zyi/Desktop/librmcs/firmware/rmcs_board/boards/hpm6e8y/
  ├── board.h      ← FCFG: 0xfcf90002
  ├── board.c      ← kOption = {0xfcf90002, 0x00000007, 0x00001000, 0x0}
  ├── CMakeLists.txt ← USB PID: 0xA904
  └── hpm6e8y.yaml

Linker Scripts:
  boards/bsp/hpm_sdk/soc/HPM6E00/HPM6E80/toolchains/gcc/
  ├── flash_xip.ld   ← Bootloader (FCFG @ 0x400)
  └── flash_uf2.ld   ← Application (FLASH @ 0x20000)

烧录工具:
  /home/zyi/3rd_party/hpm/HPMicro_Manufacturing_Tool_v0.6.0/
  ├── hpm_manufacturing_cmd   ← 命令行工具
  ├── hpm_manufacturing_gui   ← GUI（需要 GTK）
  └── bl_fw/HPM6E00/         ← Boot Loader Firmware（ISP 辅助固件）
```
