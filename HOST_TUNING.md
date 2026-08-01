# 主机侧延迟调优（USB / EtherCAT）

> **文档类型**：现行规范（主机运行环境）
> **适用范围**：跑 librmcs host SDK 的上位机，USB bulk 与 EtherCAT/IgH 两种传输
> **状态**：现行有效（2026-08-01 在本机逐项实测）
> **相关文档**：[host-tuning.sh](host-tuning.sh)（可执行版本） · [firmware/rmcs_board/ecat/DESIGN.md](firmware/rmcs_board/ecat/DESIGN.md) 3.5/3.6/第 5 节（完整分布数据与论证）

## 摘要

这份文档回答一个问题：**为了让 USB / EtherCAT 延迟达到板子本身的能力，上位机需要设置什么，
以及哪些设置其实没用。** 每一条都标了证据等级——实测有效、实测无效、没测过——因为这类调优
清单最容易变成"抄了一堆但没人验证过"的咒语。

**一句话结论：主机侧真正有用的只有一条——关掉深 C-state。** 其余常见建议在本机要么无效、
要么是空操作、要么有害。

**除内核 cmdline 外全部不持久化，每次重启都要重跑。**

```bash
sudo ./host-tuning.sh          # 应用所有一次性设置并报告
sudo ./host-tuning.sh --check  # 只报告，不改
sudo ./host-tuning.sh --pmqos  # 另开终端，测量期间持住（关深 C-state）
```

## 本文导航

- 第 1 节：唯一真正有效的一条（深 C-state）
- 第 2 节：USB 侧逐项清单与证据等级
- 第 3 节：EtherCAT 侧逐项清单与证据等级
- 第 4 节：**实测无效或有害的做法**（别再加回来）
- 第 4.1 节：**USB 代码本身（host + device）还有没有余量**
- 第 4.2 节：**板端 C++ 全流程还有没有余量**（USB 没有；EtherCAT 有，量子小 5 倍）
- 第 4.3 节：**多块 USB 板并发会互相拖累**（必读，决定多板方案选型）
- 第 5 节：为什么两种传输对主机调优的敏感度不同

---

## 1. 唯一真正有效的一条：关掉深 C-state

USB 是**中断驱动**的：libusb 事件线程阻塞在 `libusb_handle_events()` 上，核会掉进深
C-state，中断到来时要付几十微秒的退出延迟。EtherCAT 是**忙轮询**的，核根本不睡，天生躲开
这一项。

同一测试、同一板子，只切换这一个变量 [实测]：

| USB（核对调镜像） | 默认 | PM QoS = 0 |
|---|---|---|
| p50 | 124.8 | 124.8（不变，见第 5 节） |
| p99 | 150.8 | **146.0** |
| p99.9 | 188.2 | **158.9** |
| max | 216.8 | **160.7** |

**尾部差 56 微秒。** 做控制要按见过的最差留余量，这一条直接决定你能不能收紧控制周期。

做法是让一个进程持住 `/dev/cpu_dma_latency` 并写 0：

```bash
sudo ./host-tuning.sh --pmqos   # 前台运行，Ctrl-C 释放
```

**没有做成开机自启，是故意的**：它会一直吃功耗，而只有测量/运行控制环时才需要。如果你的
机器就是专用控制机，可以自己做成 systemd unit。

> **测 USB 延迟之前必须先开这个，否则测到的是电源管理，不是传输。**

## 2. USB 侧逐项清单

| 项 | 本机状态 | 证据 | 持久化 |
|---|---|---|---|
| **深 C-state 关闭** | 默认没开，需手动 | **实测有效，收益最大**（第 1 节） | 否 |
| 设备 `power/control = on` | 本来就是 `on` | 内核对本设备的默认值即 `on`，未观察到 autosuspend | 是（默认） |
| root hub `power/control = on` | 原为 `auto`，已改 | **未实测出差别**（`runtime_status` 一直是 `active`，本来就没挂起）。改它是消除一类可能性，不是修一个已知问题 | 否 |
| `usbcore.autosuspend` | `2`（秒） | 上面两条已经把这条路堵住了 | — |
| USB LPM（L1） | 本设备 sysfs 无此节点 | 硬件/驱动未暴露，无从设置 | — |
| libusb IN 队列深度 | 16 个 transfer 常驻在飞 | **代码里已实现**（`kReceiveTransferCount`），不是系统设置 | 代码 |
| 事件线程绑核 + SCHED_FIFO | 每次测量 `chrt -f 80` + `thread_setup` | 必要 | 否（按次） |
| xHCI 中断线程优先级 | FIFO 50（RT 内核默认） | **实测无效**（提到 90 三轮无差别）。它在 CPU3，测试线程在 CPU7，两者不争 | 否 |
| irqbalance | 未运行 | 若运行会在运行时挪中断制造抖动，**保持关闭** | 是（服务 inactive） |

## 3. EtherCAT 侧逐项清单

| 项 | 本机状态 | 证据 | 持久化 |
|---|---|---|---|
| `isolcpus=7 nohz_full=7 rcu_nocbs=7` | 已设 | 未单独对照 | **是**（GRUB cmdline） |
| PREEMPT_RT 内核 | `6.8.1-1015-realtime` | 未单独对照 | 是 |
| `sched_rt_runtime_us = -1` | 已设 | 沿用早期结论（曾把一个 ~50 ms 的 max 定位到 RT 限流） | **否**——`/etc/sysctl.d` 里没有任何配置，**重启变回 950000** |
| 网卡 EEE off | 本来就 off | — | 是（驱动默认） |
| 网卡 `rx-usecs 0` | 设了会被冲掉 | **实测是空操作**，见下 | 否 |

### 3.1 关于网卡的两个坑

**坑一：网卡调优必须在 EtherCAT master attach 之前做。** master 一起来，`ec_igc` 就接管
了网卡，它不再是 netdev，`ethtool` 直接报 `no device matches name`。`host-tuning.sh` 里是
stop master -> ethtool -> start master 的顺序。

**坑二：`ethtool -C <if> rx-usecs 0 tx-usecs 0` 在本机执行不了，而且执行了也没用。**

```
netlink error: igc: Queue Pair mode enabled, both Rx and Tx coalescing controlled by rx-usecs
netlink error: Invalid argument
```

igc 是 queue-pair 模式，只能写 `rx-usecs`。更关键的是：**设成 0 之后启动 master、再停掉
master 重新读，值又回到了 3** ——`ec_igc` 接管网卡时会自己重新初始化硬件，ethtool 写进去
的值根本没进 EtherCAT 数据面。本机 `DEVICE_MODULES="igc"` 用的是原生轮询驱动，中断合并
本来就不相关。

**不要因为"设了 rx-usecs=0"就认为网卡侧已经调过了。** 脚本里保留这一步只是为了将来可能
换成 `ec_generic`（走内核网络栈，那时中断合并才真正相关）。

## 4. 实测无效或有害的做法

**这一节的价值不亚于第 1 节**——它们都是看起来合理、抄起来顺手的做法。

| 做法 | 预期 | 实测 | 结论 |
|---|---|---|---|
| CPU governor `powersave` -> `performance` | 降低尾部 | p50/p99/max 全在噪声内 | **无效**。SCHED_FIFO 线程本来就把频率顶住了。脚本仍然设它，只是为了少一个变量 |
| xHCI 中断线程提到 FIFO 90（**只接一块板时**） | 中断优先被处理 | 三轮无差别 | **无效**——但**只在单板时成立**，多板时结论相反，见 4.3 节 |
| **把 xHCI 中断和 libusb 事件线程绑同一个核** | 省掉跨核唤醒 | **p50 124.8 -> 146.2（劣化 21 us）**，p90 126 -> 157 | **有害，绝对不要做**。ISR 与事件线程在一个核上串行，损失远大于跨核唤醒的收益 |

**xHCI 中断要和 libusb 事件线程分开放**，这是本机默认行为，不要"优化"它。

## 4.1 USB 代码本身还有没有余量（host + device 逐项）[核查 2026-08-01]

**结论：两侧都已经贴在"微帧"这个物理上界上，没有能改 p50 的代码改动。**
判据是实测分布本身：`min 92.0 / p50 124.8` —— 板子最快 92us 就能把回复准备好，
p50 之所以是 124.8，是**回复错过了当前微帧要等下一个**，不是代码慢。

**设备侧（固件，`app/src/usb/vendor.hpp`）**

| 项 | 状态 |
|---|---|
| 零拷贝直发 | ✅ `CFG_TUD_VENDOR_TX/RX_BUFSIZE = 0`，不过 FIFO，`tud_vendor_n_write` 直接 claim 端点起传输 |
| 变长 + 短包语义 | ✅ 有多少发多少；短包结束传输；满 512 时补 ZLP |
| 端点大小 | ✅ HS bulk 512 字节 |
| 一次只能有一个 IN 在飞 | 是栈/硬件属性。**对请求-响应型负载无影响**——同一时刻本来就只有一个回复要发 |
| 从 ISR 直接踢泵（不等主循环） | **已实现且已实测**：p50 **零收益**，只改善尾部。见 [firmware/rmcs_board/ecat/DESIGN.md](firmware/rmcs_board/ecat/DESIGN.md) 3.4 的对照表。原因就是上界是微帧不是"武装延迟" |
| 小瑕疵 | `tud_vendor_n_write()` 的返回值只在 `assert_debug` 里检查（release 是空操作）。当前配置下不可达（前面已查 `write_available`），但确实是个未检查的返回值 |

**主机侧（`host/src/transport/usb/usb.cpp`）**

| 项 | 状态 |
|---|---|
| IN 队列深度 | ✅ **16 个 transfer 常驻在飞**（`kReceiveTransferCount`），管道不会空 |
| OUT 池深度 | ✅ **64 个**（`kTransmitTransferCount`），1 kHz 控制率下不可能耗尽 |
| 重新提交时机 | ✅ 在完成回调里，不回主循环 |
| ZLP | ✅ `LIBUSB_TRANSFER_ADD_ZERO_PACKET` |
| 事件线程绑核 + RT | ✅ `thread_setup` 钩子 |
| **唯一的结构性问题** | `usb_receive_complete_callback()` 里是 `receive_callback_()`（完整反序列化 **+ 你的用户回调**）**先执行**，然后才 `libusb_submit_transfer()`。**如果 PID 跑在上位机，它就是在 libusb 事件线程上执行的**，和 USB 事件处理串行。16 个 transfer 在飞所以不会饿死管道，但**你的 PID 执行时间会原样叠进这条线程的响应延迟与抖动**。 |

**最后一条是唯一值得动的**，而且它不是 USB 的问题，是应用架构问题：把重计算从
回调里挪出去（回调只入队，PID 在自己的线程算），或者接受它并把 PID 做得足够短。
注意**不能简单地"先 resubmit 再处理"**——buffer 还在被读，重新提交会覆盖它，
要改就得配双缓冲或多一次拷贝。

## 4.2 板端 C++ 全流程还有没有优化空间 [实测 2026-08-01]

**结论分两半，不要混用：**
- **USB 路径：没有有意义的空间**，板端占整个 RTT 约 2-3%（本节以下内容）。
- **EtherCAT 路径：结构上有（量子小 5 倍），但已试过的一项没兑现。** EtherCAT 的 RTT 严格量化到
  25.75us 的周期，USB 量化到 125us 的微帧——同样是赌边界，**格子小 4.85 倍**。
  论证见 [firmware/rmcs_board/ecat/DESIGN.md](firmware/rmcs_board/ecat/DESIGN.md) 4.3 节；
  但按该推理做的第一项（把 MCAN 驱动搬进 ILM）**实测零收益**，见同文件 4.4 节。
  **下面这节的"没空间"只针对 USB。**

### 主循环周期：0.72 us（空闲）/ 0.85 us（满载）

给固件主循环加了迭代计数器（`LIBRMCS_CAN_DIAG` 遥测里的 `loop=` 字段）实测：

| 负载 | 每 100ms 迭代数 | 周期 |
|---|---|---|
| 空闲 | 138838 | **0.72 us** |
| 双流 8000 f/s（合计 16000） | 117993 | **0.85 us** |

主循环里排着 `tud_task()`、4 个 CAN 看门狗、USB 泵、跨核泵、UART 泵、诊断泵——
**全部跑完一圈不到 1 微秒**。

这一个数就把下面这类优化全部否掉了：

| 想法 | 最多能省 |
|---|---|
| 把 `tud_task()` 搬进 USB ISR（省掉"等下一趟主循环"） | **< 0.85 us** |
| 重排主循环、把泵提前 | **< 0.85 us** |
| 把 SDK 的 MCAN 驱动从 XIP flash 挪进 ILM | ISR 总共才 1-2 us，只能省其中一部分 |

对照 RTT p50 124.8 us、min 92.0 us —— **板端整条 C++ 路径不到 RTT 的 3%**，
而 p50 是被 125 us 微帧钉死的。**优化板端 C++ 改不动任何一个分位。**

### 一条确实存在但不值得动的：MCAN 驱动在 XIP flash

map 文件核实（`build-swap` release）：

```
handle_downlink                      0x00001358  ILM     <- 我们的胶水
irq_handler                          0x00001a8a  ILM
read_uplink                          0x00001454  ILM
serialize_uplink                     0x0000157e  ILM
mcan_read_rxfifo                     0x8002bbb0  FLASH(XIP)   <- SDK 驱动
mcan_transmit_via_txfifo_nonblocking 0x8002bcf6  FLASH(XIP)
```

`can.cpp` 的注释早就写明了这个取舍（"Leaf calls into the MCAN driver stay in FLASH"）。
`ecat/core0` 那套定制链接脚本把 EtherCAT 数据面拉进了 ILM，**app 镜像没有对 MCAN 做同样的事**。
理论上可以做，但按上面的账，收益在 1 us 以内，**不值得为它引入一个定制链接脚本**。

### 参考：一个 3 us 的改动能做什么

CAN-FD 从 5 提到 6 Mbit（省约 3 us 线上时间）时，p50 五轮为
124.4 / 105.3 / 100.6 / 100.6 / 100.3——**要么约 100 要么约 124**。
说明在微帧边界附近，3 us 确实可能翻掉一整档 25 us，**但完全不可控**。
所以"抠板端几微秒"不是没有效果，而是**效果是随机的**，不能作为工程手段。
可靠地拿到 100 us 的唯一办法仍然是单核镜像。

## 4.3 多块 USB 板并发：会互相拖累，且必须调中断线程优先级 [实测 2026-08-01]

两块板（HPM5321 DualCan + HPM6E8Y）同时跑同一个延迟测试，各自绑不同的核。
两块都在 **Bus 003，同一个 xHCI 控制器的同一个 root hub**，因此共用一个中断线程
（`irq/135-xhci_hcd`，RT 内核默认 FIFO 50）。

| | 5321 单独 | 5321 并发 | 6E8Y 单独 | 6E8Y 并发 |
|---|---|---|---|---|
| p50 | **99.8** | **124.6** | 124.9 | 124.7 |
| max | 130.3 | **975.5** | 160.6 | **1098.0** |

**两件事同时发生：**

1. **5321 掉了整整一个微帧**（99.8 -> 124.6）。两个设备共享同一份微帧调度，
   回复不再能稳定赶上早一班。**单板时那 25us 的优势，在有第二块板的瞬间消失。**
2. **两块板的 max 都炸到约 1 毫秒**（975 / 1098us），是单板的 6-7 倍。
   对 1 kHz 控制环这等于**整拍丢失**。

把共享的中断线程提到 FIFO 90 之后：

| | 并发 + IRQ FIFO 50 | 并发 + IRQ FIFO 90 |
|---|---|---|
| 5321 max | 975.5 | **180.3** |
| 6E8Y max | 1098.0 | **177.2** |
| p50（两者） | 124.6 / 124.7 | 124.7 / 124.8 |

**尾部救回来了（约 5.5 倍），p50 救不回来。**

**这修正了第 4 节表里的那条结论**：单板时提高 xHCI 中断线程优先级确实无效
（已实测），**但多板时它是必需的**——两个设备的完成事件要排在同一个 FIFO 50 的线程上，
那个线程一旦被别的 RT 工作挤住，两块板一起掉进毫秒级尾部。

> **多板部署的硬性要求**：
> 1. `chrt -f -p 90 $(pgrep -f "irq/N-xhci_hcd")`，否则 max 约 1 ms；
> 2. 不要指望多板还能拿到单板的 p50——**同一控制器上的板越多，越是都退化到微帧边界**；
> 3. 想避开共享调度只能把板分到**不同的 xHCI 控制器**上（本机有两个：`00:14.0` 和
>    `00:0d.0`），但控制器数量有限，扩不到很多块。

## 5. 为什么两种传输对主机调优的敏感度不同

理解这一点比记住清单更有用。

**USB 的 p50 是被微帧量化钉死的，主机调优动不了。** HS 微帧是 125 us；实测 min 92.0、
p50 124.8、p90 126.3——绝大多数落在一个微帧上，偶尔赶上早一班就是 92。所以：

- **主机调优只能改善 USB 的尾部（p99 之后），改不了 p50。** 上面所有实验都印证了这一点：
  C-state 把 max 从 216.8 压到 160.7，但 p50 纹丝不动 124.8。
- **要改 p50 只能减少板端周转时间**，让回复赶上早一个微帧。这是全有全无的一档（25 us），
  唯一已知办法是不释放 core1（单核镜像 p50 99.8）。见
  [firmware/rmcs_board/AGENTS.md](firmware/rmcs_board/AGENTS.md) 的选型表。

**EtherCAT 对主机电源管理不敏感，因为它忙轮询。** 但它对**主机栈本身**敏感：实测周期
25.75 us 里只有约 8 us 是线上时间，其余约 18 us 是 IgH 协议栈 + 网卡往返。所以 EtherCAT
侧想再快，动的是主机栈和隔离，不是网卡参数。

**两者的失效模式不同，选型时看 p50 会选错**：USB 是"要么快要么很慢"（p99−p50 = 33 us），
EtherCAT 是"稳定地慢一点"（p99−p50 = 7 us）。完整三档分布见
[firmware/rmcs_board/ecat/DESIGN.md](firmware/rmcs_board/ecat/DESIGN.md) 3.5 节。
