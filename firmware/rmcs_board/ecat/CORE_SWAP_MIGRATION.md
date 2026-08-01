# 核对调迁移方案：EtherCAT 上移 core1，USB+CAN 下沉 core0

> **文档类型**：背景说明（方案论证）+ 过程记录（实施计划）
> **适用范围**：`firmware/rmcs_board/`（app 与 ecat 两套镜像），HPM6E8Y 双核
> **状态**：现行有效（步骤 0-4 已实施并上板验证；步骤 5 部分完成，hybrid 变体未做；
> 第 6 节的前置阻塞项已于 2026-08-01 定位并修复）
> **相关文档**：[DESIGN.md](DESIGN.md) 第 3 节与 3.1、3.4 · [README.md](README.md) · [../boards/hpm6e8y/ECAT_BRIDGE_BRINGUP_NOTES.md](../boards/hpm6e8y/ECAT_BRIDGE_BRINGUP_NOTES.md)

## 摘要

本文件给出**把 EtherCAT 协议栈搬到 core1、把 USB+CAN+协议栈合并到 core0** 这次迁移的
完整方案：为什么这么切、可行性依据、按风险排序的实施步骤、以及每一步的上板判据。

它是 [DESIGN.md](DESIGN.md) 3.1 里被否决的方案 (d) 的**严格改进版**——同样把 CAN 与 USB
放到一起（拿到实测的 25us），但**搬的是 EtherCAT 而不是 USB**，于是 (d) 最贵的那项代价
（DFU 必须一起搬、刷机路径失效等于变砖）**直接消失**。

**当前状态（2026-08-01）：步骤 0-4 已实施并上板验证，功能全部达成。**
但有两件事必须先读：

1. **迁移的性能动机不成立**。实测表明那 25us 来自"core1 在运行"（双核争用），
   **不是**"USB 过跨核环"，所以任何双核布局都拿不到它。若 USB 是主力传输，
   最快的仍是单核 `app/` 镜像。详见步骤 4 的结果小节与 DESIGN.md 3.4 的修正块。
   迁移剩下的价值是**工程收益**：一个镜像同时支持两种传输，且 DFU 不用搬。
2. ~~**前置阻塞项仍未解决**，见第 6 节（单核布局的 CAN 闩死，需要 JTAG）。~~
   **已解除 [2026-08-01]**：根因是 MCAN ISR 的中断标志确认顺序，不是 PLIC；
   修复后 19000 f/s 双流（合计 38000 f/s，超过原失效点 36%）连续 90 秒干净。
   全过程没有用调试器，见 6.1。

## 本文导航

- 第 1 节：目标布局与它相对 (d) 的差异
- 第 2 节：可行性依据（硬件 + 体积，全部已核实）
- 第 3 节：三个必须重新设计的子系统（含 **3.1.1 仲裁的实际行为**：USB 与 EtherCAT
  谁抢得走数据面、并发时会看到什么）
- 第 4 节：按风险排序的实施步骤与上板判据（每步都带实测结果）
- **第 4.6 节：被实测推翻的六条假设 + 一条方法论错误——想省时间就先读这节**
- 第 5 节：风险清单
- 第 6 节：前置阻塞项（CAN 闩死，**已于 2026-08-01 定位并修复**，根因与验证见 6.1）

---

## 1. 目标布局

```
                     core0 (boot 核, flash XIP)         core1 (纯 RAM 镜像, ILM)
  USB host --(vendor bulk, 核内直连)--> [librmcs 协议栈] <--跨核环--> [SSC + ESC]  <-- EtherCAT master
                                        [CAN x4 / UART1]                (仿真 SII EEPROM)
                                        [DFU-RT]                        flash 写 --RPC--> core0
```

与 [DESIGN.md](DESIGN.md) 3.1 方案 (d) 的关键差异：

| | 方案 (d)（已否决） | 本方案 |
|---|---|---|
| 搬的是 | USB + DFU 下沉 core1 | **EtherCAT 上移 core1** |
| DFU / 刷机路径 | 必须整体迁移并重验，失败即变砖 | **一行不动**（USB 留在 boot 核） |
| 已验证的部分 | 无 | **core0 半边就是 `app/` 单核镜像，2026-07-31 已实测 p50 100.0us** |
| core0 中断层次 | 需要重排 | 与现有 `app/` 完全一致，(d) 的缺点 2 不成立 |
| EtherCAT 热路径 | 维持手工 ILM 挑段 | **整栈天然在 ILM**，定制链接脚本可删除 |
| 新增的设计题 | 无 | flash 写跨核委托（见 3.2） |

性能预期：USB 路径拿到实测的 −25us（CAN-FD）/ −16us（经典 CAN）；**EtherCAT 路径跳数
完全不变**（下行 ESC->ARQ->环->对面核->deserializer->CAN，上行对称），因此延迟应基本持平。
若上板后 EtherCAT 回环 p50 明显劣化，说明搬迁引入了本不该有的开销，而不是设计代价——
这是一条很好的二分判据。

### 1.1 迁移前的完整基线 [实测 2026-08-01]

四组数据在同一天、同一条 CAN0->CAN1 接线、同一个 host release 构建、3000 样本、
绑核 7/6 下取得，**全部 PASS、0 WKC error**，可直接与迁移后对比：

| 传输 | CAN-FD p50 | 经典 CAN p50 | 差值（= CAN 线上时间） |
|---|---|---|---|
| USB（现有 ecat 镜像，走跨核环） | 124.8 | — | — |
| **EtherCAT（IgH，free-run busy-poll）** | **130.1** | **205.8** | **75.7** |
| USB（单核 app 镜像，核内直连） | 99.9 | 171.7 | 71.8 |

三条重要结论：

1. **EtherCAT 只比走环的 USB 慢 5.3us**（130.1 vs 124.8）。EtherCAT 侧的 p99 甚至更好
   （134.6 vs 147.0），因为它是硬件定时的轮询，没有 USB 微帧的相位量化。
2. **EtherCAT 分布极紧**：min 121.2 / p50 130.1 / p99 134.6 / p99.9 141.9，
   p99-p50 只有 4.5us。这说明 free-run busy-poll 在这个 PD 尺寸下已经很稳，
   DESIGN.md 第 4 节担心的"轮询相位是主要项 A"在单从站直连下**没有兑现成大抖动**。
3. **两种传输的"经典 - FD"之差几乎相同**（75.7 vs 71.8），再次确认那个差值就是 CAN
   线上时间，与传输侧无关。

对迁移的含义：**EtherCAT 侧本来就没有 25us 可赚**（它已经和走环的 USB 在同一水平），
所以迁移的全部收益仍然只在 USB 路径（124.8 -> 99.9）。这印证了第 1 节"EtherCAT 跳数
不变、延迟应持平"的预期方向，也给出了迁移后必须回归到的具体数字。

## 2. 可行性依据

### 2.1 硬件（HPM6E00 用户手册 Rev0.8）

| 问题 | 结论 | 依据 |
|---|---|---|
| ESC 中断能否送到 CPU1 | **能** | 系统框图 §1.1：CPU0/CPU1 子系统**各有私有 PLIC**；中断源表（表 12）全局共享，`133=ESC`、`134/135=ESC_SYNC0/1`、`136=ESC_RESET`、`23=GPTMR0`。谁 enable 送给谁 |
| CPU1 能否访问 ESC 寄存器 | **能** | ESC 在 `0xF1700000`（128 KB，内存映射），两核都是 AXI 主设备 |
| 时钟组能否把 ESC 划给 CPU1 | **能** | SYSCTL GROUP 位表（表 29）bit 27 = ESC；CPU 与 group через `AFFILIATE[x]` 绑定 |
| 时钟组的真实语义 | group 决定"**谁请求这个外设开着**"，不决定谁能访问；关闭跟随 CPU 休眠 | §14.4.1–14.4.3 |

**时钟组这一关不是阻碍**：`clock_esc0` 现在在 group0（绑 CPU0），而新布局里 CPU0 是常驻
运行的核，所以 ESC 会一直被使能，core1 访问它没有问题。反过来说，2026-07-31 那个
"CAN 挂在没在跑的核的组里 -> 全程时钟门控"的坑（见 bring-up 笔记陷阱 12），在新布局下
**结构上不可能重演**，因为 ESC 的组绑的是永远在跑的 core0。[手册 + 实测]

### 2.2 体积（实测 map，ecat-rev6 release 构建）

新 core1（EtherCAT）镜像估算：

```
EtherCAT 半边（SSC + port + e2p + glue）   45,863
+ newlib                                  23,404
+ libgcc（软双精度，由 printf-float 拉入）  9,162
+ SDK/board 基座                          11,166
- multicore_common（归 core0）             -1,837
+ 跨核 glue / flash RPC 客户端 / 对齐        ~3,000
────────────────────────────────────────────────
                                        ~90,758 B  =  ~88.6 KiB
```

交叉校验：`core0 bin 188,352 − sec_core_img 83,484 − USB 14,389 − multicore 1,837 = 88,642`。
两条路径一致。

| 项 | 占用 |
|---|---|
| 新 core1 ILM | **88–95 KB / 256 KB（35–37%）**；摘掉 printf 后约 60–65 KB |
| 新 core1 AXI_SRAM (.bss) | ~34.3 KB / 176 KB（其中 32 KB 是 `e2p_info_table`） |
| 新 core0 flash | app 98,136 + sec_core_img ~95K ≈ **197 KB**（app 区上限 1.875 MB） |

**体积不是风险项，余量 160+ KB。**[实测]

> 注意一个易错点：`ram_core1.ld` 末尾的 `ASSERT(__fw_size__ <= LENGTH(ILM))` 里的
> `__fw_size__` **包含 `.data/.fast/.tdata` 的加载映像**（AT 地址接在 ILM 的 etext 之后），
> 所以 `.data` 初值大小要算进 ILM 预算。EtherCAT 侧 `.data` 约 3.3 KB，不构成问题。

## 3. 三个必须重新设计的子系统

### 3.1 环与仲裁

**结论：ARQ 端点搬到环对面语义自洽，且仲裁反而变简单。**[代码级论证]

- **exactly-once 成立**。上行重传只从 `Slot::payload` 重放，**从不二次 pop 环**
  （`pd_stream.hpp` 的 `build_own_chunk` 只在"暂存新 chunk"分支里 pop）；下行的
  `on_peer_chunk` 在 `try_push` **之前**就用 `seq != next_seq(rx_ack_)` 挡掉重复/跳号。
  所以环的 `in_`/`out_` 对每个 seq 只前进一次，与 PDO 重传次数完全解耦。
- **两级信道可组合**：ARQ 层与 `XcoreRing` 都是 exactly-once/有序/无损，串联后仍然是。
  core0 的 deserializer 看到的字节序列与端点同核时完全一致。
- **分层更干净**：ARQ 是传输层（补偿 SyncManager 的 latest-wins），迁移后与 EtherCAT 线缆
  同核；协议栈是应用层，迁移后与 CAN/UART 数据源同核。
- **仲裁降级**：今天 `usb_active` 必须让 ESC 钩子变惰性，是为了维持环的 SPSC 不变式；
  迁移后 **USB 根本不碰环**，SPSC 由构造保证。跨核只剩"让远端变惰性"这一项，且
  **权威方与执行方同核**（core0 既是 USB 事件源，又是环的消费端）。
- **环冲刷规则**（今天就成立、但没被利用）：**每个核只冲刷它作为消费者的那个环**。
  core0 冲 `down`、core1 冲 `up`，各自单侧推进 `out_`，与对端的 `in_` 无竞态。
- **尺寸**：`up` 环 8192 **不能缩**——它的硬下界是整个批次池 `8 × 1023 = 8184`
  （`InterruptSafeBuffer::kBatchCount` × `kProtocolBufferSize`），因为 `try_push` 是
  all-or-nothing 的整批推送。`down` 环可从 4096 **缩到 1024**：迁移后它的生产者是
  core1 的 ARQ，每个 EtherCAT 周期最多 44 字节、窗口只有 2。

**门铃方向对调**：今天 core1 敲 `HPM_MBX0B` -> core0 吃 `IRQn_MBX0A`；迁移后变成
core0 敲 `MBX0A` -> core1 吃 `IRQn_MBX0B`。语义一字不变（"up 环里有新字节，持有 ESC 的
核请提前重建输入映像"），MBX0 是全双工对，**零外设成本**。敲钟前的 full `fence`
必须原样保留——缺它会"看到门铃但读到旧数据"，这是本项目超出 SDK sample 的部分。

建议 **flash RPC 单独占 MBX1**，不要和热路径门铃挤同一个单字邮箱（`mbx_send_message`
在有 pending 时会失败）。

### 3.1.1 仲裁的实际行为（不是"先连上的赢"）

**先纠正一个常见误解：不存在"谁先建立连接谁独占、另一个从此不能用"。** 规则是
**后来者夺取**，而且两边的触发条件是**不对称**的：

| 传输 | 靠什么夺取数据面 | 触发频率 |
|---|---|---|
| USB | **任何一个 vendor OUT 包**（`tud_vendor_rx_cb` -> `notify_usb_activity()`），包括会话 keepalive | 只要 host 程序在跑，就持续在发 |
| EtherCAT | **主站 SAFEOP -> OP 这一次状态跃迁**（core1 `ecat_claim.fetch_add`） | 一次；主站待在 OP 里不会再发 |

夺取时做的事（`app/src/xcore/pd_link.cpp` `pump_data_plane()`）：写 `channel.owner`
-> `handle_link_restart()`（复位会话、清批次池）-> bump `link_epoch`；EtherCAT 夺回时
还会额外冲刷 `down` 环。所以**被夺走的一方是掉会话，不是被永久拉黑**——它重新握手就
能再抢回来。

**并发时到底会发生什么 [实测 2026-08-01]**：让 IgH 主站在 OP 里跑 `ecat_canfd_stress`，
第 8 秒起并发启动 `usb_canfd_stress`，观察到的完整序列是：

```
[ 9s] 01  tx=18001  rx=18000            <- EtherCAT 正常
[10s] 01  tx=20001  rx=18501  missing=1500   <- USB 夺走数据面，转发停止
[librmcs] [info] EtherCAT (IgH) cycle rate: 39.0 kHz (0 wc errors in the last 5 s)
[librmcs] [error] Failed to refresh session: Timed out waiting for SESSION_KEEPALIVE_ACK. Terminating...
terminate called after throwing an instance of 'std::runtime_error'
```

同一时刻 USB 那侧 `result: PASS (no loss, no corruption)`。三条结论：

1. **USB 确实持续赢**：它的 keepalive 每次都算一次夺取，而主站进 OP 之后不再 bump
   `ecat_claim`，所以 EtherCAT 拿不回来。
2. **失败是响的，不是哑的**：输的一方拿不到 keepalive ack，`protocol::Handler` 的
   `refresh_session()` 抛异常并 `std::terminate()`，host 程序**直接 abort（退出码 134）**
   并打印上面那行。不会出现"以为在跑其实没数据"的静默劣化。
3. **但 EtherCAT 链路层看起来完全正常**：`39.0 kHz，0 wc errors`，主站仍在 OP。
   所以**别在 IgH 层面找问题**——总线是好的，丢的是 librmcs 会话。

实际使用规则：

- 两个 host 程序**不要同时跑**；切换传输时先退出旧的，再起新的。
- 恢复方式：重启 EtherCAT 侧的 host 程序即可。它会重新配置从站、再走一遍
  SAFEOP -> OP，于是重新 bump `ecat_claim` 把数据面拿回来（已实测：USB 压测退出后
  再跑 `ecat_canfd_stress`，60000/60000 PASS、0 WKC error）。
- 这一条对旧布局同样成立（[README.md](README.md)「USB 协处传输」一节），
  迁移没有改变它。

为什么不做成"先到先得+锁定"：数据面只有一个 `HostSession`（一个 deserializer、
一个批次池），两个传输**在字节层面本来就不能并存**；而把所有权做成粘性会带来更糟的
失败模式——插着 USB 线的调试机会让现场的 EtherCAT 主站永远抢不回来。当前设计里
"主站进 OP"是一个明确的人为意图，所以它被授予；USB 只是发了包，所以它容易被顶掉。

### 3.2 flash 写委托（本次迁移唯一的新设计题）

全片只有一条 NOR flash（XPI0）。仿真 SII EEPROM 存在里面，而 flash 擦写期间不能同时
取指——新 core0 正是 XIP 核。

**"core1 读 / core0 写"这条朴素切法是错的**：e2p 的索引表 `e2p_info_table` 是 **32 KB 的
纯 RAM 状态**，core0 写完会让 core1 的索引表变脏，下一次 `e2p_read` 用陈旧的 `data_addr`
去读会 CRC 失败。**不存在"两边各持一份 e2p"的可行方案。**

**正确切法**：e2p 状态机整个留在 core1，只把 `e2p_config_t` 的三个函数指针做成跨核桩：

| 底层操作 | 归属 | 理由 |
|---|---|---|
| `nor_flash_init`（`rom_xpi_nor_auto_config`） | **core0 独占** | 会重配 XPI0 控制器，core1 绝不能碰。core0 在释放 core1 前完成，`sector_size` 经 channel 发布 |
| `nor_flash_read` | **core1 本地** | 它**不走 ROM API**，是从 XIP 映射窗 memcpy，天然可行且快（SII 上载是热路径） |
| `nor_flash_write` / `nor_flash_erase` | **RPC 到 core0** | core0 关中断执行 ROM API |

技术细节修正：严格必要的是**关中断**（`disable_global_irq`），RAM 驻留是加固。ROM API
的代码本身在 Boot ROM 区、不在 flash，且返回前会恢复 XIP——现有 bootloader
（`bootloader/src/flash/xpi_nor.hpp`）就是 flash_xip 构建、无 `ATTR_RAMFUNC`，且已在本板
验证。致命的是中途来中断（ISR 从 flash 取指时 XPI 正忙）。

**时序有利条件**：SSC 的 EEPROM 写只发生在 `EEPROM_CommandHandler()`，而它由
`ECAT_Main()` 调用，是**线程上下文而非 PDI ISR**，所以 core1 可以直接忙等 RPC 完成，
不会阻塞 PDI 中断。

**代价（必须记录）**：一次 4 KiB 扇区擦除会让 core0 关中断几十毫秒，期间 USB 会 NAK
甚至掉会话、MCAN RX 可能溢出。缓解：首启刷 EEPROM 的窗口放在 core0 起 USB/CAN
**之前**；运行期主站的 EEPROM 写请求要么延迟到安全窗口、要么直接回
`ESC_EEPROM_EMULATION_ACK_ERROR`。**禁止把 e2p 的 GC/flush 放在数据面运行时。**

### 3.3 core1 的 printf 必须彻底断根（最高优先级）

**这是本次迁移唯一"必然发生 + 完全无法归因"的故障。**[已核实]

- `hpm_debug_console.c:17`：`g_console_uart = NULL`，core1 从不调 `board_init_console()`。
- `hpm_misc.h:11-23`：`CORE0_ILM_LOCAL_BASE = 0x00000000`、`CORE1_ILM_LOCAL_BASE = 0x00040000`
  ——两核 ILM 是同一地址空间的**不同区域**，没有 per-core 别名。
- core0 的 `.vectors` 实测就在 `0x00000000`、长 `0xf58`。
- 于是 core1 上一次 `printf` -> `_write` -> `uart_send_byte(NULL, ...)` -> 读 `0x34` / 写 `0x20`，
  落在 core0 中断向量表的地址范围内：要么在 LSR 轮询里死循环，要么写坏 core0 的向量表。

EtherCAT 半边有 **29 处 printf 调用点**（`hpm_ecat_hw.c` 12 处、`hpm_ecat_e2p_emulation.c`
9 处、`ecat_main.c` 2 处，外加 `eeprom_emulation.c` 的 62 处 `e2p_err/e2p_warn`），
**全在错误/首启路径上，必然被触发**。

处理（三条都要做）：
1. core1 构建里 `E2P_DEBUG_LEVEL = NONE`，编掉 e2p 的 62 处；
2. 让 core1 的构建**不链接 `hpm_debug_console.c`**（SDK 的 `_write` 不是 weak），或用
   `-Wl,--wrap=printf`；
3. 把 port 层的 printf 改成"写进 SHARE_RAM 诊断环、由 core0 打印"——顺带把 EtherCAT
   的启动日志救回来。

副产品：摘掉 printf 后 core1 少约 **25 KB**（newlib printf-float 15.7 KB + libgcc 软双精度
9.2 KB），这两块今天在两个镜像里各带一份而 core1 从不调用。

## 4. 实施步骤（按风险倒序）

每一步都有可上板验证的判据，前一步不过不进入下一步。

### 步骤 0：最小 EtherCAT-on-core1 探针镜像

**目的**：一次性打掉风险 1（printf）、3（时钟组）、10（ESC 跨核访问），且**刻意把最难的
flash 写委托隔离在外**。

> **前置条件（做之前先确认，否则步骤 0 会假失败）**：探针镜像的 EEPROM 走只读，靠的是
> flash 里**已经有一份有效的 SII**。当前板上状态已核实自洽 [实测 2026-08-01]：
> stock `eeprom.h` 的 revision = **6**（2026-08-01 起为 **7**，为验证写路径而提升）
> > hybrid 的 5，vendor `0x1a81` / product `0x1` 两者一致，
> 所以现有 ecat 镜像上电时会把仿真 EEPROM 刷成正确的 48 字节 stream SII。
> **必须先用现有双核 ecat 镜像启动一次、让它把 SII 固化下来，再刷探针镜像。**
> 否则探针镜像的只读 EEPROM 会读到 hybrid 的 352 字节 SM 配置（或空白 flash），
> 表现为主站枚举不到从站或 SM 长度校验失败——很容易被误判成"ESC 搬核失败"。

- core0 = 现有 `app/`，只改三处：`BOARD_FIELDBUS_ON_CORE0=1`（已有）、
  `kMchtmrClockName` 从 `clock_mchtmr1` 改回 `clock_mchtmr0`、加 `multicore_release_cpu`。
- core1 = 只含 `board_init_core1()` + ESC 初始化 + `ecat_hardware_init()` + `MainInit()` +
  `MainLoop()`；**EEPROM 走只读**（把 `ecat_flash_eeprom_init` 的写分支短路，假定 flash 里
  已有旧固件写好的有效 SII）；**printf 全部替换成诊断环**。
- 时钟组：`clock_esc0` / `clock_gptmr0` 改挂 group1。

**判据**：主站能枚举到从站并进 PREOP、`ecat_time_ms` 在涨、core0 的 USB 与 CAN 回环不受
影响。

#### 步骤 0 结果：**通过** [实测 2026-08-01]

**核心假设已验证：ESC 的 PDI 中断确实能在 CPU1 上取到。** 这在此前一直是纸面推导
（手册说每个 CPU 子系统有私有 PLIC），现在有了硬件证据：

| 观测 | 结果 |
|---|---|
| 主站枚举（IgH，网卡被接管后名为 `ecm0`） | `1 slave(s) responding` -> `INIT` -> **`PREOP`**，bus scan 11 ms |
| 从站身份 | Vendor `0x1a81` / Product `0x1` / **Revision `0x6`** / Device name `rmcs_stream` |
| core0 的 USB+CAN 回环（core1 同时在跑 EtherCAT） | p50 **104.0 us**，3000/3000 PASS |
| **A/B 对照**：同一个 app 但不释放 core1 | 主站**看不到任何从站**（link 都不 UP，ESC PHY 从未初始化） |
| 可复现性 | 重刷 + 重启主站后再次到 PREOP |

A/B 对照排除了"ESC 硬件残留状态"这一替代解释——从站响应确实来自 core1 上的固件。

core1 探针镜像实测占用（比预估的 88-95 KB 小得多，因为 printf 断根顺带消掉了整套
newlib 浮点格式化 + libgcc 软双精度）：

| 区域 | 占用 |
|---|---|
| ILM | **50,512 B / 256 KB（19.3%）** |
| DLM | 32 KB / 256 KB（12.5%） |
| AXI_SRAM | 37,560 B / 176 KB（20.8%，其中 32 KB 是 `e2p_info_table`） |

**printf 断根已在符号级验证**：core1 镜像的 `nm` 输出里 `printf` / `puts` / `_write` /
`_dtoa` / `_strtod` / `_mprec` / `vfprintf` **一个符号都没有**。core0 侧的 USB+CAN 回环
正常也是间接证据——若 core1 真的写到了低地址，core0 的向量表早就坏了。

~~**尚未验证**：`ecat_time_ms` 是否在涨~~ **已验证 [实测 2026-08-01]**。本机确实
没有 FT2232 串口，改为把 core1 的诊断环经 core0 从 USB vendor 端点送出
（`-DLIBRMCS_DIAG_OVER_USB=ON`，主机侧 `host/examples/core1_log.cpp`），直接读到心跳：

```
core1 alive: t=3075 ms, al_status=0x1
core1 alive: t=4075 ms, al_status=0x1
core1 alive: t=5075 ms, al_status=0x1
```

**严格 1000 ms 步进，GPTMR0 时基正常**（`al_status=0x1` = INIT，当时没有主站在跑）。
教训同 6.1：拿不到串口不等于拿不到现场，带内通道往往就够。

### 步骤 1：拆 `pd_glue.cpp`

此时环方向已对调，**仲裁先硬编码为"永远 EtherCAT 拥有"、USB 数据面禁用**，把正确性问题
和仲裁问题分开验证。

现有 `pd_glue.cpp` 共 108 行，逐个函数的归属（这是施工图，照着搬）：

| 现有函数 | 新归属 | 备注 |
|---|---|---|
| `rmcs_pd_init`（`xcore_channel_init`） | **core0** | 它是 channel 的构造与发布，必须留在 boot 核；从本文件拆出去 |
| `endpoint`（`PdStreamEndpoint` 实例） | **core1** | ARQ 是传输层状态，跟 ESC 同核 |
| `rmcs_pd_on_outputs` / `rmcs_pd_build_inputs` | **core1** | PDO 钩子，PDI ISR 上下文 |
| `rmcs_pd_uplink_pending` | **core1** | `up.readable()` 只在消费者侧精确，迁移后 core1 正是 `up` 的消费者，原注释里 "racy by design" 的论证逐字成立 |
| `rmcs_pd_reset`（SAFEOP->OP） | **拆开**：`endpoint.reset()` + `up` 环冲刷留 core1 本地同步执行；"夺回链路"改为 `ecat_claim.fetch_add()` 请求，由 core0 裁决 | 语义从"同步且必然"变成"异步且可被拒绝"——这是改进，见 3.1 |
| `rmcs_pd_downlink_free` / `push_downlink` / `pop_uplink` | **删除** | 存在的唯一理由是让 USB 碰环；迁移后 USB 核内直连，无调用者 |
| `rmcs_pd_set_usb_active` / `rmcs_pd_usb_active` | **删除**（C 边界） | ownership 变成 core0 上的 C++ 对象 + channel 的 `owner` 字段 |
| 文件内的 `std::atomic<bool> usb_active` | **删除** | 它只为"让同核的 ESC 钩子变惰性"而存在 |

**判据**：EtherCAT 回环通过，p50 与迁移前持平（见第 1 节的二分判据）。

#### 步骤 1 结果：**通过** [实测 2026-08-01]

EtherCAT 数据面在对调后的布局上跑通，**延迟与迁移前完全持平**——第 1 节那条二分判据
给出了"设计没有引入额外开销"的结论：

| 指标 | 迁移前基线 | 步骤 1 实测 | 差值 |
|---|---|---|---|
| CAN-FD p50 | 130.1 | **130.2** | +0.1 |
| CAN-FD p99 | 134.6 | 137.0 | +2.4 |
| 经典 CAN p50 | 205.8 | 207.1 | +1.3 |
| WKC error | 0 | **0** | — |
| 样本 | 3000/3000 | 3000/3000 | — |

（首轮运行 p50 为 134.1，第二轮起稳定在 130.2；取稳定值。）

**这验证了 3.1 节的核心论证**：ARQ 端点搬到环对面后，exactly-once 语义与延迟都没有退化。
代码级依据也已逐条核对（不是引用分析结论）：

- 下行 `on_peer_chunk`：seq 检查在 `pd_stream.hpp:122`、`try_push` 在 `:124`，**重复/跳号
  在碰环之前就被挡掉**，所以环的 `in_` 对每个 seq 只前进一次。
- 上行：`transmit_ring.pop` 全文件**只出现一处**（`:167`，存进 `slot.payload`），两条重传
  路径（`:161`、`:191`）都读 `slots_[]`，所以 `out_` 只随新 chunk 前进。

USB 侧行为符合设计：**枚举与 DFU-RT 正常**（本轮三次刷写都走 DFU），**数据面按预期禁用**
（`Timed out waiting for SESSION_ACK`）。步骤 4 才把 USB 合并回来。

踩到并修掉的两个问题：

1. **`ecat_pd_init()` 漏调**。core1 的 channel 指针为 null 而 PDO 钩子无条件解引用它。
   症状很有迷惑性：WKC 全对、38800 个周期一个错都没有（ESC 层完全健康），但主机侧报
   `Deserializer encountered an error`——**数据面在跑，只是内容是垃圾**。已在 `MainInit()`
   之前绑定，并在注释里写明"未绑定的端点是空指针解引用、不是良性 no-op"。
2. **一个 ODR 违反**。core0 侧最初用 per-source `COMPILE_DEFINITIONS` 改名来绕开
   `uplink_serializer` 等强符号冲突，但那也会改掉 `HostSession::uplink_enabled()` 这个
   inline 成员在该 TU 内的名字——不同 TU 看到的成员集合不同，是编译器无法诊断的 UB。
   已改为 `vendor.cpp` 内基于 `LIBRMCS_APP_RELEASE_CORE1` 的条件编译。符号级已验证：
   无 `*_detached` 残留、`tud_vendor_rx_cb` 为 weak（TinyUSB no-op 接管）、
   `tusb_rhport_init` / `tud_dfu_runtime_reboot_to_dfu_cb` 存活。

### 步骤 2：反转门铃

**判据**：EtherCAT 上行 p50/p99 不劣于迁移前。

#### 步骤 2 结果：**已实现，但 p50 零收益** [实测 2026-08-01]

| 指标 | 步骤 1（仅 MainLoop 兜底） | 步骤 2（门铃反转） |
|---|---|---|
| p50 | 130.2 | **129.7 / 130.0** |
| p99 | 137.0 | 138.8 / 140.9 |
| WKC error | 0 | 0 |

**收益在测量噪声内。** 这与迁移前 core0 上做同一改动时的结论一致（见 DESIGN.md 3.4：
门铃踢泵对 p50 零收益、只改善尾部），但**原因在对调后变了**：

- 迁移前的理由是"SSC 主循环混有 mailbox/CoE 慢路径、几十微秒抖动"，门铃绕开它。
- 对调后 **core1 只跑 EtherCAT**，它的 MainLoop 极紧（没有 CAN/UART/USB 混在里面），
  所以"下一次 MainLoop 传递"本来就快，门铃能抢回来的时间所剩无几。

**结论：保留，但要明白它买的是什么。** 它不改善这个单请求单响应的基准，价值在于
**负载升高时**——当 core1 的 MainLoop 因为 CoE/mailbox 事务变慢时，门铃是那条不受影响的
旁路。代价是每次 push 一条 MBX 写加一次 core1 中断，实测未见 p99 恶化。

如果将来要精简，这是可以第一个拿掉的东西——但先用带 SDO 负载的场景复测，不要用这个
空载基准下结论。

### 步骤 3：flash RPC

按 3.2 实现。**判据**：首启（bump revision）时 1024 次写全部成功且 `e2p_read` 立刻可读回。

#### 步骤 3 结果：**已实现，写路径未触发验证** [实测 2026-08-01]

`XcoreFlashRpc` 已进契约（**version 3 -> 4**，字段偏移变了；2026-08-01 又因
`eeprom_ready` 升到 **5**，见步骤 3 未验证项 2）。SHARE_RAM 10840/16384
（hybrid 12392），余量 34%。core1 ILM 20.9%。

上板结果：core1 正常启动、**SII 读回正确（revision 6）**、进 PREOP、EtherCAT 与 USB
数据面全部回归通过（130.1 / 124.8，0 WKC error）。这验证了**读路径**（core1 本地 memcpy，
不走 RPC）和**版本握手**。

设计要点（与 3.2 一致，实现时补充的）：

- **粒度 = 一次 ROM 调用**：一次 program（≤512B）或**一个扇区**的 erase。多扇区擦除拆成
  逐扇区往返，让 core0 在扇区之间把中断放回去，而不是为整片连续关中断。
- **地址窗口硬校验**：只允许 `0x80200000..0x80210000`（仿真 EEPROM 区）。core1 状态机
  跑飞也碰不到 app 镜像和 bootloader。
- **MBX1 中断而非轮询**：`E2P_CRITICAL_ENTER` 会让 core1 在忙等 RPC 时关着自己的中断，
  所以服务延迟 1:1 变成 core1 PDI ISR 的阻塞时间——ISR 延迟有界，core0 主循环没有。
- **运行期写策略分两级**：擦除只在 boot window 内允许（门放在 RPC 层，覆盖所有隐式
  `E2P_FLUSH_FORCE` 路径）；主站的字写入**低于 SAFEOP 才受理**，否则回
  `ESC_EEPROM_EMULATION_ACK_ERROR`。

**未验证 / 已知偏差（重要）**：

1. ~~**写路径从未被触发**~~ **已验证 [实测 2026-08-01]**。把 `patch_sii.py` 的
   `REVISION` 从 6 提到 7、重新生成 `core0/SSC/Src/eeprom.h` 并重刷，制造出
   revision 不匹配，core1 日志给出完整的一次首启刷写：

   ```
   RMCS EtherCAT probe (core1), channel version 5
   Stored SII product 0x1 revision 6 (built-in 0x1 / 7).
   Init EEPROM content.
   Init EEPROM content successful.
   EEPROM loading successful, no checksum error.
   SSC up, pd in/out 48/48 bytes
   ```

   **写路径跑通并且立刻读回校验通过**（"no checksum error"），判据关闭。
   紧接着的第二次启动打印 `revision 7 (built-in 7)` -> `No need to init EEPROM
   content.`，**幂等**。之后主站枚举正常：`0  13330:0  PREOP  +  rmcs_stream`。
2. ~~**3.2 那条"首启刷 EEPROM 窗口放在起 USB/CAN 之前"没有做到**。~~
   **已实现 [2026-08-01]**。`App::App()` 现在分三段：先在关中断段里做
   `board_init` / `publish_channel` / `flash_server_init` / led / timer；再**开中断、
   立刻 `release_core1()`**；然后 `wait_for_core1_eeprom(5000)` 等 core1 报告 EEPROM
   收工；最后才在第二个关中断段里起 pd_link / USB / CAN / UART。
   握手用新加的 `XcoreFlashRpc::eeprom_ready`（**channel version 4 -> 5**，字段偏移
   再次变化，core0/core1 必须同时重建），由 core1 在 `ecat_hardware_init()` 返回后
   （成功与失败两条路径都要）置位——失败路径也置位，否则 core0 会为一次根本不会发生的
   刷写白等满 5 秒。超时不致命：继续启动，只是丢掉这层隔离，因为一个不应答的 core1
   不能把 USB 固件连同 DFU 通路一起拖死。
3. 扇区擦除真实耗时未实测（按 4KB/~40ms 经验值设计）；RPC 超时 5s 后**永久停用客户端**
   （避免新参数覆盖在途请求），代价是一次瞬时抖动会让本次启动的 EEPROM 变只读到复位。

### 步骤 4：合并 USB 到 core0 协议栈

单 `HostSession` 子类 + 两个 `try_transmit_*()` 方法，**不引入虚接口**（`try_transmit` 是
每轮主循环都调的热路径，vtable 间接跳转在 XIP 下多一次 cache miss；后端只有两个且编译期
已知）。`uplink_serializer()` / `uplink_enabled()` 保持自由函数，**驱动层零改动**。

**必须避开的陷阱**：环的 `pop` **不是传输边界**，绝不能触发 `finish_downlink_transfer()`。
USB 有短包边界、PD 流没有（framing recovery 只在 link restart 时发生）。两条路径共用一个
deserializer 时，这是最容易踩的坑。

**判据**：USB 回环 p50 达到 100us 量级；USB<->EtherCAT 交替抢链路时环不出现乱序/丢字节。

#### 步骤 4 结果：**功能通过，但 25us 收益未兑现** [实测 2026-08-01]

功能全部达成：

| 项 | 结果 |
|---|---|
| USB 数据面恢复 | 3000/3000 PASS |
| EtherCAT 数据面 | 3000/3000 PASS，0 WKC error |
| 仲裁双向切换 | USB -> EtherCAT(130.3) -> USB(124.7)，**无需重启，两向都干净** |

**但 USB p50 是 124.4us，不是预期的 ~100us。** 这条必须写清楚，因为它推翻了本文档
第 1 节的性能前提。

关键对照（全部主站停止、同一天、同一接线）：

| 镜像 | core1 状态 | USB p50 |
|---|---|---|
| 单核 `app/`（`LIBRMCS_APP_RELEASE_CORE1=OFF`） | **未释放** | **100.0** |
| 步骤 0 探针（core1 跑 EtherCAT，USB 核内直连、不过环） | 运行 | **120.0** |
| 步骤 4 合并 | 运行 | **124.4** |

**结论：这 20us 的代价来自"core1 在运行"本身，不是来自合并、也不是来自跨核环。**
步骤 0 那一版的 USB 路径与单核镜像的数据通路完全相同（`tud_vendor_rx_cb` 直接进协议栈，
不碰任何环），却已经慢了 20us。合并本身只多花 4.4us。

推测是双核争用（AXI 总线 / flash XIP 取指 / cache）[推断，未证实]。要定死需要在 core1
空转与 core1 跑 SSC 两种状态下对比，本轮没做。

**两个被实测证伪的假设，记录以免重走**：

1. ~~"USB 慢是因为主循环每轮读非缓存 SHARE_RAM 环"~~ → 改成 USB 持有链路时完全不碰
   两个环，p50 仍是 124.8。**证伪。**（该改动本身合理，已保留：它消除了 USB 路径上无谓的
   非缓存访问。）
2. ~~"步骤 0 测到 104.0，所以退化发生在步骤 1-4"~~ → 用**同一个步骤 0 二进制**重测得到
   120.0/120.9/119.1，与主站是否运行、网线是否有链路均无关。**当初那次 104.0 是一次性的
   乐观测量，不是可复现基线**——这是我在本次迁移中犯的最有误导性的一个错误：拿单次
   测量当基线，后面所有对比都会被它带偏。

**对整个迁移的含义**：迁移的核心动机是 USB 路径拿回 25us（DESIGN.md 3.4）。
**该收益在双核布局下拿不到**，因为 core1 一旦运行就要付约 20us。也就是说：
如果 USB 是主力传输，**单核 `app/` 镜像仍然是最快的**；本迁移的价值退化为
"EtherCAT 与 USB 共存于一个镜像、且 DFU 不用搬"这一工程收益，不再是延迟收益。

这个结论应当反馈到 DESIGN.md 3.4 的决策判据里。

### 步骤 5：收尾

`down` 环缩到 1024；hybrid/native 变体的固定区对调（**第一阶段只做 stock stream**，
否则同时翻转两套环 + 两套 epoch 语义的调试面太大）；同步重写 README.md 架构图与
DESIGN.md 第 3 节。

#### 步骤 5 进展 [2026-08-01]

| 项 | 状态 |
|---|---|
| `down` 环缩到 1024 | **已完成**（`common/xcore_channel.hpp` 的 `kXcoreDownRingSize`） |
| README.md / DESIGN.md 第 3 节与 3.4 | **已完成**：两份文档开头都加了"你在看哪一套布局"的提示块，DESIGN.md 3.4 补了归因修正 |
| 烧录脚本 | **已完成**：`./flash-ecat-swap.sh`（`CORE1=0` 出单核 USB 镜像，`CAN_DIAG=1` 带 CAN 遥测） |
| hybrid / native 变体对调 | **未做**，仍是本次迁移唯一的功能缺口。生产周期控制推荐的 352 字节固定 PDO 目前只有旧 `ecat/` 布局有 |

**核对调布局的回归结论 [实测 2026-08-01]**，全部在同一镜像、同一次启动上取得：

| 项 | 结果 |
|---|---|
| 首启 SII 刷写（rev 6 -> 7） | 成功，读回无校验错；第二次启动幂等不刷 |
| 刷写窗口位置 | USB 枚举比复位晚约 13 秒，确认刷写发生在 USB/CAN 之前 |
| 主站枚举 | `0  13330:0  PREOP  +  rmcs_stream` |
| EtherCAT 数据面（IgH，双流 4000 f/s × 15 s） | 60000/60000 × 2，0 损坏，**577973 周期 0 WKC error**；p50 166.1 / 166.2 us（两条 CAN 总线同时满载，与单总线 queue-free 的 130us 不可直接比） |
| USB 数据面（同镜像，EtherCAT 之后） | 59996/59996 × 2，0 损坏，仲裁切换正常 |
| CAN 转发压测（16000 f/s 双流 × 90 s） | 1439882/1439883 × 2，0 丢帧、0 停摆、`recovered=0` |

最后一行尤其重要：它是在**核对调布局**下跑的，也就是第 6 节担心的"USB + CAN 同核"
那一列真正进了生产数据通路之后的结果。

## 4.6 被实测推翻的假设（勿重走）

这次迁移里有六条看似合理、实际错误的判断。**它们的价值不亚于成功的部分**——每一条
都曾指向一个方向的工作，写在这里是为了让后来者（包括未来的自己）不必再花一遍代价。

| # | 曾经的判断 | 实测结果 | 教训 |
|---|---|---|---|
| 1 | USB 那 25us 是"跨核环 + 泵调度"的代价，把 USB 与 CAN 放同核就能拿回 | **错。** 步骤 0 的 USB 通路与单核镜像逐字相同（不碰任何环），仍慢 20us。代价来自 **core1 在运行**（双核争用），与环无关 | 差值有了不等于归因对了。**必须做只改一个变量的对照**，否则整个方案的动机可能建立在错误的因果上 |
| 2 | 门铃踢 vendor 泵能压低 USB 延迟（迁移前） | p50 零收益，只改善尾部 | — |
| 3 | 反转门铃能压低 EtherCAT 延迟（迁移后） | p50 零收益。对调后 core1 只跑 EtherCAT，主循环极紧，"下一次 MainLoop"本来就快 | 优化的前提（"主循环慢"）会被架构改动本身消灭。**换了架构要重新检查优化还成不成立** |
| 4 | 合并后 USB 变慢是因为主循环每轮读非缓存 SHARE_RAM 环 | 改成 USB 持链时完全不碰环，p50 纹丝不动 | — |
| 5 | `clock_gptmr0` 不在任何时钟组，是与 CAN 同类的 bug | SDK 的 ECAT port 层自己做了分组（`hpm_ecat_hw.c:213`），现有固件没这个 bug | agent 的结论要复核到行号 |

| 6 | 单核镜像的 CAN 闩死是 PLIC claim/complete 在中断嵌套下漏了一次，**需要 JTAG 才能定死** | **两处都错。** 该 PLIC 源是 level 触发（`trigger[2]=0`），level 网关加线为高不可能静默，这直接证伪了"漏 claim"；真凶是 MCAN ISR 只在进入时清一次 `IR`。而"需要 JTAG"也不成立——现场是靠 USB 带内通道读出来的，全程没用调试器 | 硬件机制的推断，**必须落到寄存器读数上再下结论**；"读不到现场"常常只是还没想到带内通道 |

还有一条**方法论错误**，比上面任何一条都更该记住：

**"步骤 0 的 USB p50 = 104.0"是单次测量，被当成了基线。** 用同一个二进制重测三轮实为
120.0 / 120.9 / 119.1，与主站是否运行、网线是否有链路都无关。在那之后的每一次对比都被这个
数带偏，直到步骤 4 才因为差距过大而暴露。**基线至少测两轮，且必须在同一天、同一条件下
重测后才能引用。**

## 5. 风险清单（按"最可能让迁移失败"排序）

| # | 风险 | 严重度 | 首个可验证信号 |
|---|---|---|---|
| 1 | **core1 printf 打穿 core0 向量表**（3.3） | 高 | core1 上刻意 printf 一次，看 core0 是否立刻挂 |
| 2 | **e2p 索引表跨核不一致 / flash 委托设计**（3.2） | 高 | 首启 1024 次写是否全成功且立刻可读回 |
| 3 | **时钟组归属**（ESC0/GPTMR0/ETH0/CAN/UART1） | 高 | ESC 寄存器读回非 0；`ecat_time_ms` 在涨 |
| 4 | USB/环所有权重构 | 中 | 交替抢链路时环不乱序 |
| 5 | flash 擦写窗口压住 core0 的 USB/CAN | 中 | 运行期触发一次 EEPROM 写，看 USB 是否掉枚举 |
| 6 | 中断优先级与门铃方向重排 | 中 | PDI ISR 周转 p99 不劣于迁移前 |
| 7 | ~~PE03/04/05 引脚冲突（RGB LED vs ESC0_CTR）~~ **已排除，非风险** | — | 见下 |

风险 7 的核对结论 [2026-08-01]：**冲突不存在，本项作废**。PE03/04/05 确实有 ESC0_CTR
复用（`hpm_iomux.h`，ALT 11），但本板 `init_esc_pins()` **一次都没写过这三个 pad**——
HPM6E\*Y\* 片内 PHY 版本的四路 CTR 落在 **PA25 / PA28 / PC20 / PC21**，与 `board.h` 的
`BOARD_ECAT_*_CTRL_INDEX` 一致；SDK 的 ecat port 层只写 ESC 的 `IO_CFG` 寄存器、不碰
IOC pad。PC20/PC21 被 ESC 接管是**设计意图**（EtherCAT RUN/ERROR 灯，`board_park_leds_off()`
先驱到 OFF 再交给 ESC），不是冲突。原判断来自 EVK 遗留 pinmux 时代的旧结论。
| 8 | 构建系统改造 | 低 | 失败即编译失败，不留隐患 |
| 9 | 体积 | 低 | `ram_core1.ld` 的 ASSERT 不触发 |

两个容易被顺手带错的细节：

- 新的 EtherCAT 子项目**不能**沿用 `ecat/core1/CMakeLists.txt` 那套
  `-Wall -Wextra -Wpedantic`——SSC 生成码和 SDK port 层过不了 `-Wpedantic`。
- `hpm_ecat_hw.c` 里 `clock_add_to_group(ECAT_TIMER_GPTMR_CLK, 0)` 的组号是**硬编码 0**，
  不是漏配（现有固件因此没有 GPTMR 问题）。迁移后它会由跑在 core1 的代码把 gptmr0 加进
  group0；因为 group0 绑的 CPU0 恒在运行，**仍然有效**，改成 group1 是整洁性问题而非
  阻塞项。

## 6. 前置阻塞项：单核镜像的 CAN 闩死 —— **已定位并修复 [实测 2026-08-01]**

> **状态：解除。** 根因不是 PLIC claim/complete 漏掉，而是 **MCAN ISR 的中断标志确认
> 顺序**：`IR` 在进入时清一次、随后排空 FIFO，排空期间到达的帧会把 `RF0N` 重新置上，
> 而此后**没有任何代码再清它**。于是中断线一直高、控制器一直"有请求"，接收却永远
> 停摆。修复见下面 6.1；6.2 起是原始记录，保留备查。

### 6.1 根因、修复与验证 [实测 2026-08-01]

**闩死现场**（`host/examples/can_stall_probe.cpp` 从运行中的板子读出，16000 f/s 双流）：

| 观测 | 值 | 含义 |
|---|---|---|
| `IR` | `0x0001000f` | `RF0N` 置位，且 `IE` 使能它 → MCAN 中断线**是高的** |
| `RXF0S` | fill=32, `F0F`=1, `RF0L`=1 | RX FIFO0 满并开始丢帧 |
| `PSR` | busoff=0, ep=0, ew=0, act=rx | **控制器和总线完全正常**，帧还在进来 |
| PLIC trigger type | `0` | 该中断源是 **level 触发**，不是 edge |
| PLIC pending | `0` | 网关没有挂起请求 |
| ISR 进入计数 | 冻结，直到复位 | 中断再也没有被送达 |

线是高的、网关是 level 的，pending 却是 0——这个组合排除了"控制器坏了""总线断了"
"批次池满了"（`alloc_fail` 全程 0，且主机 keepalive ack 一直正常返回，说明上行通路
健康），只剩下确认顺序。

**修复**（`app/src/can/can.cpp`）：ISR 改成"处理 → 重读 `IR` → 只要还有使能位就再来
一轮"的循环，保证**只在观察到 `IR` 无使能位时才返回**，即返回时线一定是低的。
`kEnabledInterrupts` 提为一个常量，同时供 `mcan_enable_interrupts()` 和循环条件使用，
两者不可能再漂移。

**残余窗口与兜底**：只加循环时，16 kHz 下仍在约 600 秒内复现过一次
（原来是 10 秒内必现，约 60 倍改善）。剩下的窗口在"ISR 最后一次读 `IR`"与"SDK 包装层
写 PLIC completion"之间。因此 `Can::poll()` 作为主循环看门狗保留：健康路径上只是一次
MMIO 读加一次比较，只有当**连续两次主循环观察到线是高的且期间 ISR 没有进入过**时才
补一次 `intc_m_complete_irq()`——PLIC 规定对未在服务的源写 completion 是空操作，
所以误判无害。

**验证**（两对回环全接，`can_stall_probe`，每档 90 秒，中间不复位）：

| 每流帧率 | 14000 | 16000 | 18000 | 19000 |
|---|---|---|---|---|
| 结果 | 干净 | 干净 | 干净 | 干净 |
| 每流累计 | 1.26 M | 1.44 M | 1.62 M | 1.70 M |

丢帧 0、损坏 0、`alloc_fail` 0、`recovered` 0。**19000 f/s 双流 = 合计 38000 f/s，
已经超过当初 28000 的失效点约 36%。** 另有 900 秒 16 kHz 长跑，见本节末尾。

诊断手段本身已入库，可复用：固件侧 `app/src/diag/can_diag.*`
（`-DLIBRMCS_CAN_DIAG=ON` 开启，把 ISR 计数、MCAN 状态寄存器、PLIC pending/enable/
trigger 打成一条 UART0 上行帧），主机侧 `host/examples/can_stall_probe.cpp` 解码并在
转发停摆的瞬间打印前后快照。**本板没有接 FT2232、也没装 OpenOCD，整个定位过程没有用
到任何调试器**——原文"需要 JTAG 停在现场读 PLIC 才能定死"这一判断不成立。

### 6.2 原始记录（保留备查）

**新布局的 core0 = USB + CAN 同核，正是下表中会闩死的那一列。迁移会把这个故障从一个
没人用的实验镜像搬到生产数据通路上。**

同一条递进序列（`usb_canfd_stress`，每档 20s，中间不复位）：[实测 2026-07-31]

| 每流帧率 | 4000 | 8000 | 12000 | 14000 | 16000 |
|---|---|---|---|---|---|
| 单核 `app/`（USB+CAN 同核） | 干净 | 干净 | 干净 | **rx 中途停住** | **rx=0 闩死** |
| 双核 `ecat/`（现状） | 干净 | 干净 | 干净 | 干净 | 干净 |

已排除的：

- **不是会话层/上行缓冲**。闩死后换新进程重连（新 nonce -> `activate_session()` ->
  `transmit_buffer_.clear()` 把索引与批次全重置）**依然 rx=0**，卡点在会话层以下。
- **不是"RX FIFO 没排空"**。`Can::irq_handler()` 进门就 write-1-to-clear，然后
  `while (handle_uplink(...)) {}` 无条件抽干 RX FIFO0——上行缓冲满时帧被丢弃，但 FIFO
  条目照样消费掉了。

  > **这一条恰恰指着真凶，却把它排除了 [2026-08-01 修正]**。"进门就 write-1-to-clear"
  > 被当成了安全性的论据，实际上它正是 bug 本身：清完之后的排空期间再置上的 `RF0N`
  > 无人清理。当时看的是"FIFO 有没有被排空"（有），没看"标志有没有被清干净"（没有）。
  > 排除法的每一条都要检查它排除的到底是不是同一件事。

剩下最可能的：**PLIC 的 claim/complete 在中断嵌套下漏了一次**。单核镜像上
USB(2)/CAN(3)/UART(1) 全挤在同一个 PLIC 上互相抢占，双核镜像的 core1 只有 CAN/UART/timer
几乎不嵌套；这种故障的特征全中（只哑一个中断源、永久、复位才好）。**需要 JTAG 停在
现场读 PLIC 才能定死**，无法从主机侧证明。[推断，待验证]

~~**结论：这一项必须先定位并修掉，迁移后的固件才能上车。**~~ 已于 2026-08-01 定位并
修复，见 6.1。当时"剩下最可能的是 PLIC claim/complete 漏了一次"这条推断**方向错了**：
PLIC 侧确实有一个残余窗口（6.1 的看门狗就是为它留的），但真正让接收永久停摆的是
MCAN ISR 的中断标志确认顺序。教训与 4.6 节同类——**推断出的机制要用现场寄存器验证过
才能当结论**，而"读不到现场"往往只是还没想到用带内通道把现场送出来。
