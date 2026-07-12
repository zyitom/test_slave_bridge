# EtherCAT 延迟进一步优化路线(纯分析,本文不改代码)

> **已被 `LATENCY_ROADMAP_V2.md` 接替(2026-07-13)**:P0a/P0b/P1 均已实施,最终实测、
> 踩坑记录与新的剩余空间清单见 v2。本文保留作历史推导。

> **6e8y 现状**:PDO 已从本文旧基线的双向 128B 缩到双向 48B,
> ARQ payload 从 124B 缩到 44B。下面的 128B 计算保留为历史基线。

> **2026-07-05 实测修正(先读这段,它改了下面的优先级)**:正式集成后做了同口径对照
> (见 `EVALUATION.md` "like-for-like" 一节)。真相是:**单帧 RTT ~43us,但应用看到的
> *流* RTT = 172us = 4.0 x 单帧**,因为一个字节要穿过 ~4 个帧周期做 ARQ + 从站双核 echo 往返。
> 而且 IgH 和 SOEM AF_XDP 的*流* RTT 打平(172 vs 165us)——换主站驱动对流延迟没帮助。
> 所以应用级延迟公式是 **流RTT ≈ N_echo周期 x 帧周期**,两个因子都是杠杆、且相乘:
> 缩小 PDO 压"帧周期",压缩 ARQ-echo 压"N",各约 2x,叠起来约 4x。下面据此重排了优先级。

前置:IgH 原生驱动的**单帧** RTT 是 **42.5us(p50)**(见 `EVALUATION.md`)。这份文档回答
"固件端和上位机端还能怎么再降延迟"。先把这 42.5us 单帧拆开(它是"帧周期"这个因子),
再谈"N_echo 周期"这个因子。**结论先行:降应用级*流*延迟,最高杠杆是 (P0a) 缩小 PDO 压帧周期
和 (P0b) 压缩 ARQ-echo 周期数,两者相乘;换主站驱动(SOEM->IgH)对流延迟无效。**

---

## 0. 先算账:42.9us 花在哪(这是全篇的地基)

`igh_latency_bench` 测的是**纯 EtherCAT 帧往返**(写输出 PDO -> 等 WKC complete),
不含从站 echo。一帧带 2x128B 过程数据,线上大致:

```
前导+SFD 8  以太头 14  ECAT 头 2  数据报x2 (10hdr+128+2wkc)x2=280  FCS 4  IPG 12
-> 约 320 字节 = 2560 bit
100Mbit 串行化 = 2560 / 100e6 = 约 25.6 us
```

也就是说 **42.9us 里约 25.6us(~60%)纯粹是把这 320 字节在 100Mbit 线上时钟出去的时间**,
剩下约 17us 才是 ESC 直通转发 + 网卡收包 + 主站处理 + DC。

三条硬约束,决定了优化方向:
- **100Mbit 是 EtherCAT 标准锁死的**(100BASE-TX)。HPM6E80 片内 ESC 也是 100Mbit。
  换千兆需要 EtherCAT G 从站+主站,现有硬件做不到——**串行化速率没法改,只能改字节数**。
- 串行化时间和过程数据大小**线性**相关。128B->更小,RTT 和周期率**同时**改善。
- 以太网最小帧 64 字节。PDO 小到一定程度帧被填充到 64B 封顶,串行化地板约 6.7us。

---

## 1. 最高杠杆:把 PDO 右尺寸(唯一能砍掉 ~18us 的一招)

旧基线两个方向都是固定 128B(SSC 工程里 32x32bit 的字节数组 PDO,ESI 固定映射),
但 ARQ 每周期实际只搬 124B,延迟测试里业务帧才 64B。**如果 RMCS 真实每周期下行指令/
上行反馈远小于 124B(多半如此:几个电机 x 每个几字节),这 128B 是在为空气付串行化钱。**

预估收益(需实测确认):

| 每方向 PDO | 线上字节 | 串行化 | 帧 RTT 估计 |
|---|---|---|---|
| 128B(旧基线) | ~320 | 25.6us | 42.9us(实测) |
| 64B | ~190 | 15.2us | ~32us |
| 32B | ~130 | 10.4us | ~28us |
| 接近 64B 最小帧地板 | ~84 | 6.7us | ~22us |

**右尺寸 PDO 有希望把帧 RTT 从 43us 砍到 ~22-28us,近乎腰斩,且没有任何"黑科技"风险。**

代价与注意:
- 代价是每周期可搬载荷变少 -> stop-and-wait ARQ 的**吞吐上限**下降(124B x 22kHz ≈ 2.7MB/s
  -> 28B x ~28kHz ≈ 0.8MB/s)。要按真实业务的 **p99 每周期载荷**来定尺寸,不是按峰值。
- **上下行可以不对称**:SM2/SM3 尺寸独立,指令和反馈谁小就把谁调小。
- 改动是跨层协同、必须一致:SSC Tool 工程 + ESI 文件的 PDO 字节数、`RMCS_PD_CHUNK_SIZE`
  (`rmcs_pd.h`)、`kPdChunkSize`(`core/include/librmcs/ecat/pd_stream.hpp` 的
  header/payload 拆分)、以及 `soem.cpp` / `igh.cpp` 两个后端的 PDO 描述与尺寸校验。
  ESI 变了从站 EEPROM 要重刷。属于"一次性伤筋动骨、长期受益"的改动,不是这次顺手做。
- 如果不同工况载荷差异大,可考虑**多套 ESI/PDO profile**(小尺寸低延迟档 / 大尺寸高吞吐档),
  但 EtherCAT PDO 是固定映射,运行时不能变,只能重配重刷——权衡后大概率单一右尺寸即可。

---

## 1b. 与 P0 并列的最高杠杆:压缩 ARQ-echo 周期数(固件/协议侧)

实测 **流 RTT = 4.0 x 单帧 RTT**。这 4 个周期是一个字节走完
`主站发 -> 从站 core0 收 -> 跨核到 core1 -> core1 回 -> 跨核回 core0 -> 写进 ESC -> 主站下一帧读`
外加**上下行 ARQ 各自的 stop-and-wait seq/ack 握手**。把 4 压到 2,流 RTT 直接减半——和缩小
PDO 的收益是**相乘**关系(2x 帧周期 x 2x 周期数 ≈ 4x)。

值得查的点(需要抓 `PdStreamEndpoint` 的 seq/ack 时序 + 真实链路 RTT 分解来定位):
- **上下行 ARQ 是否串行等待**:如果 uplink 的 chunk 必须等 downlink 的 ack 先回才推进,
  一个往返就被拆成两个半往返、各自吃一个周期。看能否让上下行 ARQ 在同一帧里**双向并进**
  (EtherCAT 一帧本来就同时读写,读回和写出是同一次交换)。
- **门铃是否真的省下了那一个周期**:`ecat_appl.c` 的 `rmcs_input_refresh` 号称"回复即刻进
  ESC",但如果 core1 的 echo 产出赶不上本帧的 InputMapping 窗口,回复还是要等下一帧。用
  `ethercat` 抓帧或在 core1 打时间戳,确认门铃命中率;没命中就落到主循环粒度,白丢一个周期。
- **ARQ 每周期只搬 124B 的 chunk 粒度**:64B 业务帧一个 chunk 装得下,但 seq/ack 的往返是
  按 chunk 记的;确认没有"必须两个 chunk 才凑齐一次可推进"的边界效应。

这条和 P0a(PDO 右尺寸)是当前**仅有的两个能显著降流 p50 的方向**,都在固件/协议侧,都需要
先做一次带时间戳的 RTT 分解把 4 个周期归因清楚,再动手。**不是这次顺手做,但优先级最高。**

## 2. 上位机 / 主站侧(能小步快跑、风险低)

这些是 17us "非线上" 部分里能抠的,单条几百 ns~几 us,累加起来有意义,而且**都在
`igh.cpp` 内部,不碰从站**。

### 2.1 DC 同步三件套降频(IgH 后端专属,值得先试)
`igh.cpp`/参考 bench 每个 cycle 都调
`application_time` + `sync_reference_clock_to` + `sync_slave_clocks`。后两个会往过程数据
帧里**追加 DC 寄存器写数据报**——即每帧多几字节 + 多个 WKC,恰好压在最贵的线上时间上。
但 DC 漂移是慢变量,参考时钟同步不需要 22kHz 的频率。
- **想法**:`application_time` 保持每 cycle 喂(否则 "No application time" 告警回来),
  但 `sync_reference_clock_to`/`sync_slave_clocks` 改成每 N 个 cycle 一次(N=8~16 起测)。
- **验证**:`ethercat`/wireshark 抓帧确认降频后帧变短且从站稳在 OP;本从站自己就是参考钟,
  `sync_slave_clocks` 在单从站下近乎空操作,降频很安全。
- **收益**:每帧少 1~2 个数据报的线上时间 + 少量 CPU,估计 ~1-3us,零硬件改动。

### 2.2 `transmit_ring_` 换成无锁 SPSC 环(消除优先级反转)
现在 `LockedByteRing` 用 `std::mutex`。cycle 线程是 SCHED_FIFO 80,应用发送线程通常更低优先级;
应用线程持锁瞬间被抢占,会**阻塞 RT cycle 线程**——典型优先级反转,正是 `max` 尾部离群的
嫌疑之一。固件侧的 `XcoreRing`(`firmware/.../common/xcore_ring.hpp`)已经是无锁 SPSC 范本,
主站这侧照搬一个单生产者/单消费者无锁环即可,热路径零锁。收益主要在**尾延迟稳定性**,不是 p50。

### 2.3 系统调优做成开机持久(稳尾巴,不降 p50)
governor=performance、C-state 关到 POLL、cycle 核 `isolcpus`/`nohz_full`/`rcu_nocbs`、
网卡 IRQ 亲和——目前全是运行时设置,**重启即失效**(这次会话前机器就已经重启回默认了,
`enp2s0` 又变回普通网卡)。落到内核 cmdline + tuned/systemd,让 42us 的 p50 不再被偶发
几十毫秒的调度/唤醒离群污染。命令清单见 `../soem/NIC_TUNING.md`。

### 2.4 网卡 PCIe ASPM / EEE(尾延迟)
i225/i226 的 PCIe ASPM 和 EEE(节能以太)会引入唤醒/协商延迟,体现在尾部。busy-poll 已经
绕开中断合并,但 ASPM 让描述符访问偶尔多等一次链路唤醒。关掉 NIC 的 ASPM 与 EEE 收尾巴。

---

## 3. 固件 / 从站侧

**先说结论:从站这侧已经做得相当到位,剩余可挖空间不大。** 现状已具备的低延迟设计:
- **片内 ESC(HPM6E80)**,过程数据经片上总线原生访问,不是外挂 SPI ESC(省掉几十 us 的
  SPI PDI 传输)。
- **热代码进 ILM**:`ecat_appl` / `ecatslv` / `ecatappl` / `hpm_ecat_hw` / `pd_glue` /
  memcpy/memset 由链接脚本 `hpm6e80_flash_uf2_ilm_hot.ld` 放进指令本地存储,不走 XIP flash 等待态。
- **SM-synchron 事件驱动**:输出在 SM2 写事件的 PDI ISR 里当场消费,不等 SYNC0。
- **上行门铃(HPM_MBX0)**:core1 产出回复即刻 poke core0,`rmcs_input_refresh` 立刻写进 ESC
  输入镜像,把"回复就绪->进 ESC"从主循环轮询粒度(几十 us)压到中断延迟,**每次请求/响应省一整个
  poll 周期**。
- **跨核环** 非缓存 SHARE_RAM + 真 acquire/release,core1 loopback 是紧凑 busy-poll。

还能挖的(收益递减,列出供权衡):
- **跨核 echo 的拷贝成本**:down/up 环在非缓存 SHARE_RAM 上,124B 逐字非缓存访问比缓存慢。
  真实链路 RTT(含 echo,非 bench)里这部分可量化;若成为瓶颈,可评估"缓存 + 显式
  dcache flush/invalidate"或 DMA 搬运,但小块(124B)通常不划算,**建议先测再决定**。
- **确认 core1 热路径也在 ILM**:core0 有 `ilm_hot` 链接脚本,core1 的 echo/协议路径是否同样
  ILM 常驻值得核对(本次没看到 core1 的 hot 链接脚本);不在的话把 host_link 热路径也放进去。
- **PDO 右尺寸(第 1 条)的固件侧配合**:SSC 工程 + ESI 字节数是这条的从站落点。

**别切 DC SYNC0 模式**:有人会想"上 DC 分布式时钟同步 = 更实时"。对**这个 stop-and-wait 的
即时应答模式反而更慢**——SYNC0 会把 PDO 拷贝推迟到下一个 SYNC0 边沿,除非相位完美对齐。
当前 SM-sync"帧一到就消费"才是延迟最优,保持不变。

---

## 4. 别走的弯路(明确的 dead ends,省得来回踩)

- **换千兆 / 换网卡**:标准 EtherCAT = 100BASE-TX,HPM6E80 ESC 也是 100Mbit,串行化速率
  锁死。想快只能减字节(第 1 条),不能加带宽。i225/i226 的 2.5G 能力在 EtherCAT 上用不上。
- **多帧在途 / pipelining**:`inflight>1` 提的是**吞吐**,单帧延迟不降反可能因排队上升。
  请求/响应场景 `inflight=1` 已是延迟最优。
- **DC SYNC0 同步模式**:见 3 节,对这个即时应答模型是负优化。
- **zero-copy AF_XDP**:已证在本内核 igc 上收路径坏(见 `EVALUATION.md`),且 IgH 原生驱动
  已整层绕开 socket 栈,这条路没有意义了。

---

## 5. 优先级与预期收益汇总

目标量是**流 RTT ≈ N_echo 周期 x 帧周期**(实测 4 x 43us = 172us)。两个因子都是杠杆:

| 优先级 | 动作 | 侧 | 打的因子 | 预期收益 | 风险/成本 |
|---|---|---|---|---|---|
| **P0a** | **PDO 右尺寸**(按 p99 载荷,上下行可不对称) | 固件+主站 | 帧周期 | 帧周期 ~43->~22us,**流 RTT 随之 ~2x** | 跨层协同、重刷 ESI,一次性大改 |
| **P0b** | **压缩 ARQ-echo 周期数**(上下行并进/门铃命中/chunk 粒度) | 固件/协议 | N_echo | N 从 ~4 压到 ~2,**流 RTT ~2x** | 需先做带时间戳 RTT 分解 |
| P1 | 系统调优开机持久化 | 系统 | 尾巴 | 稳 max(实测未调优时 max 479ms) | 低,纯配置 |
| P1 | DC sync 三件套降频(仅 IgH) | 主站 | 帧周期 | ~1-3us + 稳定性 | 低,需抓帧验证 OP 稳定 |
| P2 | `transmit_ring_` 无锁 SPSC | 主站 | 尾巴 | 除优先级反转 | 低,有固件范本可抄 |
| P2 | NIC 关 ASPM/EEE | 系统 | 尾巴 | 稳 max | 低 |
| P3 | core1 热路径入 ILM(若未) | 固件 | N_echo | echo 路径几 us | 低 |
| P3 | 跨核环缓存策略/DMA | 固件 | N_echo | echo 段 | 中,先测再定,小块多半不划算 |

**注意**:P0a x P0b 相乘,两个都做流 RTT 有望 172us -> ~44us(~4x)。

**一句话**:换主站驱动(SOEM->IgH)对**流**延迟无效(实测打平),**别再往主站侧找 p50**。
想显著降流 p50 只有两条路,都在固件/协议侧:**减字节(P0a)** 和 **减 echo 周期数(P0b)**,
且相乘。下一步先做一次带时间戳的真实链路 RTT 分解,把那 4 个周期和每周期载荷分布量出来,
再据此定 PDO 尺寸 + 改 ARQ 时序。P1/P2 那几条低风险的可顺带做,专治偶发的几十毫秒尾巴
(实测未调优时 max 冲到 479ms,调优后降到 49ms,但仍需开机持久化)。
