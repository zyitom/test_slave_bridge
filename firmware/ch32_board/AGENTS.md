# ch32_board 固件指南

> 本目录专属指南，叠加在仓库根 `AGENTS.md` 之上。项目动机、app 布局、bring-up 缺口见本目录 `README.md` 与 `PROGRESS.md`，此处只列 agent 关键点。

## 芯片与工具链
- MCU：**WCH CH32H417**（Qingke V5F），RISC-V 单核；卖点是片上 **USB 3.0 SuperSpeed（5 Gbps）** 设备控制器。
- ISA/工具链：RISC-V bare-metal，`cmake/toolchain-wch-riscv.cmake`（`riscv32-unknown-elf`，RV32IMAFC/ilp32f，**不启用** WCH 私有 `xw` 扩展）。需 `riscv32-unknown-elf-gcc`（**不是** ARM）。
- 复用 rmcs_board 的 RISC-V 工具链，构建前设：
  ```bash
  export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm
  ```

## 构建
```bash
export GNURISCV_TOOLCHAIN_PATH=~/3rd_party/hpm
cmake --preset debug -S firmware/ch32_board
cmake --build firmware/ch32_board/build --target ch32_board_app
# -> build/ch32_board_app.elf + .bin
```
- preset：`debug` / `release`。target：`ch32_board_app`。**只有 app，无 bootloader。**

## 目录结构
- `app/src/`：C++ librmcs 转发层（`app.cpp` 提供 `main()`，替换被排除的 WCH demo `app/User/main.c`）；`can/ uart/ usb/ timer/ led/ utility/`。
- `bsp/wch/`：vendored WCH 标准外设库。`bsp/usb/`：vendored CH372Device USBSS 设备栈。`bsp/syscalls.c`：newlib stub。
- `bsp/` 与 `app/User/` 下厂商代码视为第三方（只读）。
- 无 CubeMX，不适用 CubeMX 纪律。

## 现状（改代码前须知）
- **编译干净但尚未上板 bring-up**；USB SS 数据通路仍是 placeholder。
- `usb/vendor.cpp` 的 `ss::tx_ready/tx_write/tx_write_zlp` 是编译正确的占位实现，未真正驱动 EP1 IN 链；EP1-OUT 完成中断仍需改为调 `usb_ss_deliver_downlink()`。细节见 `README.md` 的 “OPEN: USB SS data path”。
