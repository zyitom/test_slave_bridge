# ch32_board 固件指南

> 本目录专属指南，叠加在仓库根 `AGENTS.md` 之上。项目动机、app 布局、bring-up 缺口见本目录 `README.md` 与 `PROGRESS.md`，此处只列 agent 关键点。
>
> **改代码或上板之前先读 `PITFALLS.md`** —— 首次 bring-up 踩过的坑（调试口被 USB2 抢占、
> ISR `mret`、读保护解除要两会话 + POR、`e339e339` 假数据等）都在那里，附判据和修法。

## 芯片与工具链
- MCU：**WCH CH32H417**，RISC-V **双核**（V3F boot/offload + V5F 转发快路径）；卖点是片上 **USB 3.0 SuperSpeed（5 Gbps）** 设备控制器。
- ISA/工具链：RISC-V bare-metal，`cmake/toolchain-wch-riscv.cmake`（`riscv32-unknown-elf`，RV32IMAFC/ilp32f，**不启用** WCH 私有 `xw` 扩展）。需 `riscv32-unknown-elf-gcc`（**不是** ARM）。
- 复用 rmcs_board 的 RISC-V 工具链，构建前设：
  ```bash
  export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm
  ```

## 构建
```bash
export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm
cmake --preset debug -S firmware/ch32_board
cmake --build firmware/ch32_board/build
# -> build/ch32_board_app.elf   (V5F @0x10000)
# -> build/ch32_board_boot.elf  (V3F @0x0)
# -> build/ch32_board_merged.hex  <- 烧这个
```
- preset：`debug` / `release`。target：`ch32_board_app` / `ch32_board_boot` /
  `ch32_board_merged`（默认全建）。**`boot` 是 V3F 启动核镜像，不是 bootloader；
  本板暂无 bootloader/DFU。**

## 烧录（WCH-Link）
```bash
OCD=~/3rd_party/wch-openocd
$OCD/bin/openocd -f $OCD/bin/wch-riscv.cfg \
    -c "init" -c "wch_riscv unfreeze" -c "halt" \
    -c "program firmware/ch32_board/build/ch32_board_merged.hex verify" \
    -c "reset run" -c "exit"
```
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
- MounRiver Studio 自带的 openocd 与 `~/3rd_party/wch-openocd` **二进制和 cfg 完全相同**，
  差别只在调用序列——它能烧而命令行烧不进时，先怀疑序列，不要怀疑工具。

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
- **bulk 数据通路还没端到端验证**：枚举只走 EP0。第一个要在板上确认的是 EP1-OUT
  的收长语义（`usb_ss_ep1_out_complete()` 读 `UEP_RX_CHAIN_LEN` + 单缓冲基址，
  多包 burst 未验证）。
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
