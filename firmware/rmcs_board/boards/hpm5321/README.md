# RMCS Slave HPM5321

> **文档类型**：硬件参考（板级说明）
> **适用范围**：`firmware/rmcs_board/boards/hpm5321/`，单 CAN 版 HPM5321 板
> **状态**：现行有效
> **相关文档**：[PINOUT.md](PINOUT.md)（引脚分配） · [../hpm5321_dual_can/README.md](../hpm5321_dual_can/README.md)（双 CAN 版，需要 CAN-FD 时用那块） · [../../AGENTS.md](../../AGENTS.md) · [../../BUILD_ENVIRONMENT.md](../../BUILD_ENVIRONMENT.md)

## 摘要

最精简的 USB <-> 现场总线转发桥：**一路经典 CAN 控制器、一路数据 UART、一颗普通 GPIO
驱动的 RGB 状态灯**。没有 IMU、没有 DBUS 接收机、没有 GPIO 应用通道、也没有每路 CAN
的独立指示灯。

**选板提醒**：这块板的 CAN 控制器配置为**仅经典模式**，主机请求 CAN-FD 的帧会被静默降
成经典 CAN 2.0 发出。真需要 CAN-FD 上线，请用 `hpm5321_dual_can`。

硬件引脚分配见 [PINOUT.md](PINOUT.md)。

## 标识信息

| 项目 | 取值 |
| ----------- | --------------------------------- |
| 板名 | `RMCS_Slave_HPM5321`（`BOARD_NAME`） |
| SoC | HPM5361（QFN48），单 RV32 核 |
| USB PID | `0xA901`（VID `0xA11C`） |
| USB 速度 | High speed（480 Mbps） |
| Flash | 1 MiB XPI NOR，app 位于共享 DFU bootloader 之后 |

## 主机侧看到什么

这块板跑的是 rmcs_board 的共享 USB 应用（`app/`）：一个承载 librmcs 字节流的 USB
vendor 类设备，会话握手（kStart nonce + keepalive 租约）由共享的 `link::HostSession`
处理。

| 主机侧端点 | 板上资源 | 说明 |
| ------------- | -------------- | ------------------------------ |
| `DataId::kCan0` | MCAN0（经典模式 1 Mbps） | DM（达妙）电机总线 |
| `DataId::kUart0` | UART2，921600-8N1 | 收发双向均由 DMA 驱动 |

### 仅经典 CAN 与逐帧的 FD 标志

共享的 CAN 驱动会尊重主机下发的逐帧 `is_fdcan` 标志，但受控制器能力封顶
（`send_fd = canfd_ && data.is_fdcan`）。这块板把 MCAN0 配成仅经典模式，因此
`is_fdcan = true` 的帧仍会以经典 CAN 2.0 发出——**安全，但是被静默降级了**。
需要 CAN-FD 真正上线时请改用 `hpm5321_dual_can`。

## 构建与烧录

```bash
cmake --preset debug -S firmware/rmcs_board -DBOARD=hpm5321
cmake --build firmware/rmcs_board/build
```

构建产物是 `firmware/rmcs_board/build/app/output/` 下的
`rmcs_board_app_hpm5321.dfu`。通过 USB DFU 烧录（**前提是芯片上已经有共享的 RMCS DFU
bootloader**；首次烧 bootloader 需要调试器）：

```bash
dfu-util -d 0xa11c:0xa901 -a 0 -D firmware/rmcs_board/build/app/output/rmcs_board_app_hpm5321.dfu
```

正在运行的 app 会暴露 DFU 运行时接口，所以 `dfu-util` 能自动让它 detach 并重新枚举进
DFU 模式。若要强制 bootloader 常驻，复位时把 **PA07（与 JTAG_TMS 复用，不一定是按键，
见 [PINOUT.md](PINOUT.md)）拉到 GND**，或者在没有有效 app 的状态下上电。

> app 与 bootloader 的 mailbox 版本不一致时，上面的自动 detach 会静默失效 —— 现象、
> 起因和恢复手段见同系列的
> [hpm5321_dual_can/README.md](../hpm5321_dual_can/README.md) [实测 2026-08-03]。

## 状态灯语义

单颗 RGB 灯（引脚见 [PINOUT.md](PINOUT.md)），由共享的 Led 驱动
（`app/src/led/led.hpp`）驱动。黄色 = 红+绿，青色 = 绿+蓝；**优先级从上到下**：

| 灯效 | 含义 |
| --------------------- | --------------------------------------------------------- |
| 黄/青交替 | 双向都拥塞（上行和下行缓冲同时满） |
| 黄色闪烁（约 4 Hz） | 上行缓冲满（板 -> 主机）；主机没在取数据 |
| 青色闪烁（约 4 Hz） | 下行缓冲满（主机 -> 板）；CAN 发送积压 |
| 绿色常亮 | 主机会话已建立，正在转发数据 |
| 绿色慢闪 1 Hz | 存活，等待主机会话 |

"主机会话已建立"指的是 **librmcs 握手已完成**（kStart nonce 得到确认，且 keepalive
租约在 1 秒内被刷新），**不等于"USB 线插上了"**。设备枚举成功但主机侧没有应用程序在跑
时，灯会停在绿色慢闪。
