# ch32_board 固件指南

> **文档类型**：现行规范（板级）
> **适用范围**：`firmware/ch32_board/`，WCH CH32H417（Qingke V3F + V5F 双核 RISC-V）
> **状态**：现行有效
> **相关文档**：[仓库根 AGENTS.md](../../AGENTS.md) · [README.md](README.md)（设计与移植决策） · [PITFALLS.md](PITFALLS.md)（**上板前必读**） · [PROGRESS.md](PROGRESS.md)（工作日志）

> 本目录专属指南，叠加在仓库根 `AGENTS.md` 之上。项目动机、app 布局、bring-up 缺口见本目录 `README.md` 与 `PROGRESS.md`，此处只列 agent 关键点。
>
> **改代码或上板之前先读 `PITFALLS.md`** —— 首次 bring-up 踩过的坑（调试口被 USB2 抢占、
> ISR `mret`、读保护解除要两会话 + POR、`e339e339` 假数据等）都在那里，附判据和修法。

## 摘要

ch32_board 是四块板里**最容易把自己弄进死胡同**的一块，原因有三：

1. **双核异构**——V3F 先复位、拥有时钟树、兼任 DFU bootloader，V5F 才是转发快路径；
   "启动 App" 是唤醒 V5F 而不是跳转，V5F 也**不能自己复位**。
2. **调试口和 USB2 抢引脚**——PB8/PB9 既是 USB2 的 D+/D-，也是 SWCLK/SWDIO，一旦回退到
   USB 2.0 就丢调试口，只能断电恢复（所以仓库里把 USB2 回退关掉了）。
3. **烧录链路坑多**——读保护、`e339e339` 假数据、`unfreeze` 的位置、halt 顺带复位导致
   读到的 PC 不可信，全部记在 `PITFALLS.md`。

本文件给的是**现在该怎么做**（工具链、构建、烧录、调试、当前进度）；**为什么这么做**
在 `README.md`，**踩过什么坑**在 `PITFALLS.md`。

## 芯片与工具链
- MCU：**WCH CH32H417**，RISC-V **双核**（V3F boot/offload + V5F 转发快路径）；卖点是片上 **USB 3.0 SuperSpeed（5 Gbps）** 设备控制器。
- ISA/工具链：RISC-V bare-metal，`cmake/toolchain-wch-riscv.cmake`（RV32IMAFC/ilp32f，**不启用** WCH 私有 `xw` 扩展）。需 RISC-V GCC（**不是** ARM）。
- **默认（推荐）**：复用 rmcs_board 的 HPM 工具链，构建前设：
  ```bash
  export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm     # [前机路径] -> riscv32-unknown-elf-gcc 13.2
  ```
  > 本板的 `toolchain-wch-riscv.cmake` 对这个变量做了通配符搜索
  > （`$ENV{GNURISCV_TOOLCHAIN_PATH}/rv32*/bin`），所以**指到工具链的上级目录也能用**
  > （如这里的 `~/3rd_party/hpm`，实际二进制在其下的
  > `rv32imac_zicsr_zifencei_multilib_b_ext-linux/bin/`）。**这个宽松规则只对
  > ch32_board 成立**——rmcs_board 用的是 HPM SDK 自带的 `cmake/toolchain.cmake`，
  > 要求该变量直接指到含 `bin/` 的那一层，指到上级会报错。同一个环境变量、两块板
  > 解析方式不同，别把这里的写法搬到 rmcs_board 去用。
- **也支持 MounRiver `MRS_Toolchain_*`**。工具前缀由 `toolchain-wch-riscv.cmake`
  自动探测（依次试 `riscv32-wch-elf-` / `riscv-wch-elf-` / `riscv32-unknown-elf-`），
  `-DWCH_TOOLCHAIN_PREFIX=` 可强制指定。注意 MRS 的 `Toolchain/` 下并排放着三套 GCC，
  前缀各不相同，`WCH_TOOLCHAIN_PATH` 要指到**具体某一套**（含 `bin/` 的那层），不是
  `Toolchain/` 本身：

  | 目录 | 前缀 | 版本 | 可用性 |
  |---|---|---|---|
  | `RISC-V Embedded GCC` | `riscv-none-embed-` | 8.2.0 | **不可用**，编不了 C++23 |
  | `RISC-V Embedded GCC12` | `riscv-wch-elf-` | 12.2.0 | 可用 |
  | `RISC-V Embedded GCC15` | `riscv32-wch-elf-` | 15.2.0 | 可用 |

  ```bash
  # 下面的安装位置是 [前机路径]，换机器后改成自己的 MRS 解压目录即可
  cmake --preset debug -S firmware/ch32_board \
      -DWCH_TOOLCHAIN_PATH="$HOME/3rd_party/MRS_Toolchain_Linux_X64_V240/Toolchain/RISC-V Embedded GCC15"
  ```
- **为什么默认仍是 HPM 那套**：同一份代码用 MRS GCC15 编出来 app 的 text+data 从
  113364 B 涨到 124472 B（FLASH 占用 86.5% -> **95.0%**，区域只有 128 KB）。两边都开了
  `--specs=nano.specs`，差异来自 GCC 13.2/15.2 与各自 newlib 的构建。要用 MRS 编就先
  确认这个余量能接受。

## 构建
```bash
export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm      # [前机路径]
cmake --preset debug -S firmware/ch32_board
cmake --build firmware/ch32_board/build
# -> build/ch32_board_app.elf   (V5F @0x10000)
# -> build/ch32_board_boot.elf  (V3F @0x0)
# -> build/ch32_board_merged.hex  <- 烧这个
```
- preset：`debug` / `release`。target：`ch32_board_app` / `ch32_board_boot` /
  `ch32_board_merged`（默认全建）。**`boot` 既是 V3F 启动核镜像，也是 DFU
  bootloader**——本芯片 V3F 先复位、拥有时钟树、决定要不要唤醒 V5F，这本来就是
  bootloader 的活，所以两者合一，"启动 App" = `NVIC_WakeUp_V5F()` 而非跳转。
- App 还会产出 `ch32_board_app.dfu`（裸 `.bin` + DFU suffix，**不带** mc02/c_board
  那个 `ImageHash` 后缀：本 bootloader 自己哈希烧进去的内容）。烧法见 `README.md`
  的 "Bootloader / DFU" 一节，或仓库根 `./flash-ch32.sh`。

## 烧录（WCH-Link）

> **路径说明**：本节的 OpenOCD 取自 MounRiver 工具链包。前机上曾经单独放过一份
> `~/3rd_party/wch-openocd`（`README.md` 里保留的就是那个版本的写法），后来改为直接用
> MRS 包内自带的，所以以 **本节路径为准**。两个路径都是 `[前机路径]`，当前机器都未安装，
> 含义见[仓库根 AGENTS.md](../../AGENTS.md#开发机环境路径约定重要先读这条再看任何路径)；
> 换机器后只需把 `OCD=` 指向新装的 MRS 包即可，命令序列不变。

注意是 **`OpenOCD/OpenOCD/bin`** 两层同名目录，`wch-riscv.cfg` 等 cfg 就和
`openocd` 放在同一个 `bin/` 里：
```bash
OCD=~/3rd_party/MRS_Toolchain_Linux_X64_V240/OpenOCD/OpenOCD
$OCD/bin/openocd -f $OCD/bin/wch-riscv.cfg \
    -c "init" -c "wch_riscv unfreeze" -c "halt" \
    -c "program firmware/ch32_board/build/ch32_board_merged.hex verify" \
    -c "reset run" -c "exit"
```
- 这份是 OpenOCD `0.11.0+dev-snapshot (2026-02-28)`，三个 cfg（`wch-riscv` /
  `wch-dual-core` / `wch-arm`）与旧版同名同用法，下面所有注意事项照旧适用。
- **`wch_riscv unfreeze` 必须在 `init` 之后、`halt` 之前**。夹到 `halt` 后面它会静默失效，
  症状是 `program` 报 `Read-Protect Status Currently Enabled` 或
  `error writing to flash at address 0x00000000`，而 `flash erase_sector` 却"成功"、
  回读全是 `e339e339`（那是保护态下的假数据，擦净应该是 `ffffffff`）。这一条是
  MounRiver 的调用方式，可用 `-c init -c "wch_riscv unfreeze"` 在其扩展里搜到。
- **不要 `flash erase_sector wch_riscv.flash 0 last`**：会擦到 option byte。`program` 自己
  会擦它要写的扇区，不需要额外擦除；多加一条 flash 命令反而会打掉 unfreeze 的效果。
- **跑板子用 `wch-riscv.cfg`（单 target），不要用 `wch-dual-core.cfg`**：后者的 `reset run`
  会同时放出两个 hart，V5F 就从 flash `0x0` 跑起 V3F 镜像（症状：V5F `mtvec=0x20100003`）。
  只在需要同时观察两个核时用 dual-core cfg（`targets wch_riscv.cpu.0` / `.1`）。
- **halt 之后读到的 PC 不可信**：WCH 的 openocd 在 halt 时会顺带复位，几次都读到复位后
  拷贝代码的循环里（`0x00004020`）。判断固件是否正常运行看 **USB 枚举**，不要看 PC，
  也不要看 `v5f_ready`（每次启动 V3F 都会重新清零）。
- halt 会让主机判 USB 掉线，且 **`resume` 不会让它回来，必须 `reset run`**。
- MounRiver Studio 自带的 openocd 与命令行这份 **二进制和 cfg 完全相同**，
  差别只在调用序列——它能烧而命令行烧不进时，先怀疑序列，不要怀疑工具。

## GDB 调试
先让 openocd 挂着当 gdb server（把上面 `program`/`exit` 换成常驻）：
```bash
OCD=~/3rd_party/MRS_Toolchain_Linux_X64_V240/OpenOCD/OpenOCD
$OCD/bin/openocd -f $OCD/bin/wch-riscv.cfg -c "init" -c "wch_riscv unfreeze" -c "halt"
# 另开一个终端
~/3rd_party/hpm/rv32imac_zicsr_zifencei_multilib_b_ext-linux/bin/riscv32-unknown-elf-gdb \
    firmware/ch32_board/build/ch32_board_app.elf -ex "target extended-remote :3333"
```
- **不要用 `RISC-V Embedded GCC15` 里的 `riscv32-wch-elf-gdb`**：它链的是
  `libpython3.8.so.1.0`，在只装了 python 3.12 的系统上直接 `error while loading
  shared libraries` 起不来。
- 可用的 gdb 有两个，任选：HPM 那套的 `riscv32-unknown-elf-gdb` **13.2**（推荐，版本最新，
  能读 GCC 15 默认输出的 DWARF 5），或 MRS `RISC-V Embedded GCC12` 里的
  `riscv-wch-elf-gdb` **12.1**。gdb 不需要和编译器同源，能解析 RISC-V ELF/DWARF 即可。
- 调试同样受"调试口与 USB 抢线"的限制，见 `PITFALLS.md`：halt 会让主机判 USB 掉线，
  且 `resume` 拉不回来，必须 `reset run`。

## 目录结构
- `app/src/`：V5F 上的 C++ librmcs 转发层（`app.cpp` 提供 `main()`，替换被排除的 WCH demo `app/User/main.c`）；`can/ uart/ usb/ timer/ led/ utility/`。
- `boot/src/`：V3F 启动/卸载核。它 **拥有整棵时钟树**（`SystemInit` 开 HSE，V5F 那份 `system_ch32h417.c` 不开），唤醒 V5F，然后跑共享 SRAM mailbox 的 offload 循环。
- `bsp/wch/`：vendored WCH 标准外设库。`bsp/usb/`：vendored CH372Device USBSS 设备栈。`bsp/syscalls.c`：newlib stub。
- `bsp/` 与 `app/User/` 下厂商代码视为第三方（只读）；已有的 local patch 一律标 `LIBRMCS LOCAL PATCH` 并登记在 `bsp/PROVENANCE.md`，改动前先读那份清单。
- 无 CubeMX，不适用 CubeMX 纪律。

## 现状（改代码前须知）
- **已上板：USB 3.0 SuperSpeed 枚举成功**（2026-07-25，WCH-LinkE 实测）。
  `A11C:D403`、`bcdUSB 3.00`、bulk EP `0x01`/`0x81` 1024B/`bMaxBurst 15`，5 Gbps，
  多次复位稳定。
- **bulk 数据通路已端到端验证**（2026-07-26）：11/11 下行包解析并回 ack，0 丢包。
  EP1-OUT 的链式 DMA 收长语义按 RM 27.2.4 修正过（`UEP_RX_DMA` 自增，实测确认）。
- **主机侧接口已就位**：`librmcs::board::Ch32Board`（`host/include/librmcs/board/ch32_board.hpp`，
  PID `0xD403`，CAN1/CAN2 + UART1/UART2），通道表在
  `core/include/librmcs/spec/ch32_board/`，`host/examples/common/multi_board.hpp`
  里有 `Ch32BoardSession`，所以 `rx_monitor` 等 example 都能直接连这块板。
- **UART 运行时改波特率已实现**（未上板）：下行 `kUartNConfig` 走
  `link::HostSession::uart_config_deserialized_callback` → `Uart::handle_config`，
  主机侧是 `Ch32Board::PacketBuilder::uart1_config` / `uart2_config`。切换时先清
  UE 再 `USART_Init`（`CTLR1_CLEAR_Mask=0x29F3` 保住 UE 和中断使能位，所以 TXE 要
  显式关掉），两个方向在途的字节一律丢弃。BRR 是 12.4 定点，只接受
  `HCLK/65536 < baud <= HCLK/16` 的值，越界**返回 false 而不是断言**（这是主机来的
  数据，坏包不该 panic 固件）。注意可表示 ≠ 精确：量化误差在范围顶端能到 ~4%，
  标准速率不受影响（100 MHz HCLK 下 115200 误差 0.006%、3 MBaud 1.01%）。
- **启动链已上板复验**（2026-07-26，无 USB 线，只接 WCH-Link）：flash 内容 == `app.bin`、
  metadata 里的 sha256 == sha256(flash) → `app_image_is_valid()` 为真、不进 DFU、唤醒 V5F；
  V5F 走完整个 `App()`（`diag[30] == 12`）、无断言、主循环 **release 219 ns/圈（4.6 MHz）、
  debug 3.25 µs/圈**。没插线时 LTSSM 只在 DISABLE/RXDET 之间循环（`diag[11] == 0x30`）、
  `USBSS_DevEnumStatus == 0`，这是预期行为不是故障（见 `PITFALLS.md` 4.5）。
- **看 V5F 死活要按 `PITFALLS.md` 4.7 的姿势**：调试器 attach 期间 V5F 不跑，同一个
  openocd 会话里 `reset run; sleep; halt` 读到的全是残留；必须先 `exit` 断开、自由跑、
  再 attach 读。判据用 `diag[30]`，**不要用 `v5f_ready`**（attach 触发的复位会把它清零）。
- **不要碰 `-Dinterrupt\(x\)=interrupt`**（`CMakeLists.txt`）：WCH 的
  `interrupt("WCH-Interrupt-fast")` 会被主线 GCC 整条丢掉，ISR 收尾变成 `ret` 而
  不是 `mret`，第一个 USB/TIM12 中断进去就永久关中断——USB 死掉的真凶。改工具链后
  用 `objdump -d --disassemble=USBSS_LINK_IRQHandler` 复验必须是 `mret`。
- **初始化顺序有硬约束**：`USBSS_Device_Init(ENABLE)` 里就 `NVIC_EnableIRQ`，所以
  它必须排在 `usb::vendor.init()` / `can` / `uart` **之后**，否则第一个 USB 中断会
  去解引用还没构造的 `Lazy<Vendor>`。
- USB 设备身份（VID/PID + 字符串描述符）在 `app/src/usb/descriptors.cpp`，
  `librmcs_usb_init_descriptors()` 必须在 `USBSS_Device_Init()` 之前调用。
- **USB 2.0 回退已关闭**（`LIBRMCS_USBSS_HS_FALLBACK=0`，`CMakeLists.txt`）。厂商栈在 SS
  训练超时 1.5 秒后会切到 USB 2.0，而 USB2 的 D+/D- 是 **PB8/PB9，同时也是 SWCLK/SWDIO**，
  一切过去调试口就没了、只能断电恢复。关掉之后 SS 失败就一直重试 SS，引脚永不被占。
  已实测：halt 核 5 秒（主机判 USB 掉线）后调试器仍能连上，旧固件此处必断。
  设成 1 可恢复原厂行为并支持 USB 2.0 主机，代价是回退时丢调试口。
- CAN/USART 的 GPIO 引脚在 `app/src/board_app.cpp` 里仍是占位值，待按原理图确定。
- **DFU 相关的两条硬约束**：
  1. 描述符按 `LIBRMCS_DFU_DEVICE` 在 `bsp/usb/usb_desc.c` 里二选一（app=0、
     boot=1），boot 那份是 DFU-mode（单接口、无端点、`bInterfaceProtocol=0x02`、
     `wTransferSize=512`=EP0 包长）。改端点或接口号要同步改
     `boot/src/usb/dfu_transport.cpp` 的 `kDfuInterfaceNumber`。
  2. **V5F 不能自己复位**：它的复位向量是 flash `0x0`（V3F 镜像），自复位会让 app
     核跑 boot 核的代码（`mtvec=0x2010_0003` 那个症状）。所以 DFU detach 时 V5F 只
     写 `boot_mailbox` + `SharedBlock::reset_request` 然后 park，由 V3F 执行
     `NVIC_SystemReset()`。
- **boot 侧轮询 `usb::dfu` 必须带 memory barrier**：`Dfu` 没有 volatile 成员，全靠
  USBSS 中断改，-O2 下会把读提到循环外变成死转（`boot/src/main.cpp` 的
  `run_dfu_mode()` 里有一条 `__asm volatile("" ::: "memory")`，别删）。
