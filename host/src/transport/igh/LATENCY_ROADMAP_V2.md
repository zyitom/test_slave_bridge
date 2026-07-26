# EtherCAT 延迟优化全记录 v2（2026-07-12/13 实施与结论）

> **文档类型**：过程记录 + 现行结论
> **适用范围**：`host/src/transport/`、`core/include/librmcs/ecat/`，EtherCAT 端到端延迟
> **状态**：**现行有效**，是延迟话题的权威版本（v1 已降为历史）
> **相关文档**：[LATENCY_ROADMAP.md](LATENCY_ROADMAP.md)（v1，历史推导） · [EVALUATION.md](EVALUATION.md) · [DESIGN.md](DESIGN.md) · [../soem/NIC_TUNING.md](../soem/NIC_TUNING.md)

## 摘要

本文件是 EtherCAT 延迟优化的**现行权威记录**：做了哪些优化、各自量化收益多少、踩了哪
三个 bug、以及"还剩多少空间"的诚实清单。

一句话成绩单：单帧 RTT p50 从 **42.5 us 降到 25.7 us**、周期率从 22.2 kHz 提到
**38.8 kHz**、64B 流 RTT p50 从 172 us 降到 **131.5 us**，且 RTT max 从约 50 ms 的
病态尾巴收敛到 **48.3 us**。详细数据见下面第 0 节。

第 4 节"运维规程"是血泪坑清单，**上手操作这套系统之前先读那节**。

> 本文接替 `LATENCY_ROADMAP.md`(v1,保留作历史)。v1 中的 P0a(PDO 右尺寸)、P1(系统
> 调优持久化)已完成,P0b(压缩 ARQ 周期数)以 go-back-N 的形式完成——本文记录全部实施、
> 踩坑、量化结果,以及"还剩什么"的诚实清单。

## 0. 最终成绩单(全部实测)

| 指标 | 优化前(v1 基线) | 现在 | 来源 |
|---|---|---|---|
| 单帧 RTT p50 | 42.5us(128B PDO) | **25.7us**(48B PDO + 去 DC) | igh_latency_bench |
| 周期率 | 22.2 kHz | **38.8 kHz** | 同上 |
| RTT max(2M 周期) | ~50 ms | **48.3us** | RT throttling 根因修复 |
| 64B 流 RTT p50 / p99 | 172 / 257us | **131.5 / 137.0us** | ecat_stream_latency |
| 饱和吞吐(每方向) | ~815 KiB/s 上限 | **1053 KiB/s** | inflight=8 |
| 16B 单 chunk 消息 RTT | — | **~78us**(3 周期) | 16B 变体实测 |
| 正确性 | — | 41 万帧 0 corrupt;仿真 20% 丢帧 0 错误 | 多轮 |

延迟公式(实测拟合,P = 每方向 PDO payload 字节数):

```
帧周期 T(P) ≈ 0.32 x P + 10.7 us        (P=48 时 T=26us,与实测吻合)
流 RTT ≈ N_cycles x T                    (N 由消息大小与 ARQ 节奏决定)
```

单帧 25.7us 的构成:~22-24us 是 100BASE-TX 串行化(标准锁死,只能减字节)+ ~1-2us ESC
直通 + **~3-5us 主站软件(每周期 8 次 ioctl + 驱动)**。距物理地板只剩 ~1.7us。

## 1. 系统层(已完成,开机持久)

`rmcs-ecat-tuning.service` + `/usr/local/sbin/rmcs-ecat-tune`(systemd oneshot,已 enable):

- governor=performance(全核);CPU7 仅保留 POLL,CPU0-6 关 C2/C3 保留 C1;
- **`kernel.sched_rt_runtime_us=-1`——这是历史上 "max ~50ms" 尾巴的根因**:默认 950000
  会把 SCHED_FIFO 忙轮询线程每秒强制罚停 50ms。修复后 204 万周期 max 48.3us;
- 所有硬件中断亲和移出隔离核 7(isolcpus 只隔离任务不隔离中断);
- timer_migration=0、nmi_watchdog=0;
- igc 中断合并清零。**坑**:igc 的 rx/tx 共用一个 ITR,`ethtool -C ... rx-usecs 0 tx-usecs 0`
  报 EINVAL,必须只传 `rx-usecs 0`,两者一起归零。

尚未做(都要改 GRUB + 重启,攒一次做):`isolcpus=6,7 nohz_full=6,7 rcu_nocbs=6,7`(给应用
线程第二个隔离核)、`irqaffinity=0-5`、可选 `mitigations=off`(本机大多 Not affected,预计
仅 0.5-1us,低优先级)。

## 2. 主站层(已完成)

**DC 整体移除**(比 v1 2.1 的"降频"更彻底):`igh.cpp` 在 activate 前调用
`ecrt_master_select_reference_clock(master, nullptr)` 显式声明不用 DC 参考时钟,master 便
不再自动选举从站为参考钟,application_time/sync_reference_clock_to/sync_slave_clocks
三件套整体免掉(v1 说"不喂就进不了 OP"只在参考钟被自动选举的前提下成立)。收益:每周期
少 3 次 ioctl + 每帧少 2 个 DC 数据报,33.9 -> 38.8 kHz(-3.7us/帧)。

已探明、按性价比排序的主站侧剩余空间:
- `LockedByteRing` 换无锁 SPSC(治优先级反转尾巴,固件 `xcore_ring.hpp` 有范本);
- cycle 循环搬进内核模块(ecrt 内核 API,`ec_mini.ko` 是现成例子):消灭全部 ioctl,
  预计 2-4us + 尾部确定性;开发/调试成本高,仅在需要最后几微秒时做。

**明确的死路**(实测/已论证,别再踩):换网卡(串行化占大头,i225 的 2.5G 在 EtherCAT 上
用不上)、升内核/驱动版本(无收益有回归风险,AF_XDP zero-copy 是前车之鉴)、DC SYNC0 同步
模式(对即时应答模型是负优化)、多帧 pipelining 打单帧延迟(打吞吐可以,打延迟不行)、
换主站栈(IgH 与 SOEM 流延迟打平:131.5 vs ~150us 量级;SOEM 作可移植后备,实测健康,
p50 差 ~16us = 6 周期 x 帧周期差,max 差 3 倍)。

## 3. 协议层:go-back-N(本轮的主角)

### 3.1 为什么

stop-and-wait 的 ack 要坐下一帧才能回到发送端,每 chunk 固有 2 个帧周期的推进节奏。
44B payload 下一个 64B 消息拆 2 chunk,流 RTT = 6 周期;28 条消息的 1kHz tick 突发要
9 chunk = 468us 排队。窗口化把节奏压到 1 chunk/帧,且**不动 PDO/ESI/EEPROM、不丢字节流
的普适性**(这是相对双槽 PDO/混合 PDO 方案的决定性优势,本项目是无下位机的通用桥)。

### 3.2 设计(`core/include/librmcs/ecat/pd_stream.hpp`,两端共享)

- 窗口 kPdWindow=2(恰好覆盖 2 帧的 ack 往返,更深只多 RAM 和重放深度);
- 线上 4B 头格式不变;ack 升级为**累计语义**(最后一个按序消费的 seq);
- 接收端**只收 `seq == next(rx_ack)`**,重复与跳号一律拒收并扣住 ack;
- **每帧一个新 chunk 的信用**:只有 `on_peer_chunk`(意味着一帧过去了,上一镜像必然已被
  该帧读走)才授予 staging 信用——这是窗口 >1 在 latest-wins 三缓冲上安全的关键不变量;
- ack 停滞 `kGoBackStallFrames=4` 帧(真丢帧的特征)才触发**整窗按序重放**;
- 在飞且无新数据时**重画最新已发送 chunk**,绝不用 idle 镜像覆盖数据镜像。

### 3.3 三个 bug 的教训(都有实测/仿真数据,写下来防止重犯)

1. **空闲即重画最旧 chunk** -> 旧协议接收端(不查序)把重传当新数据双重消费,混版本互通
   实测 12333 corrupt。改成仅停滞后重放,混版本在无损链路上互通(0 corrupt/9.8 万帧)。
   教训:协议升级必须推演"新发送端 x 旧接收端"的每条路径。
2. **go-back 只重放队头就抢跑新 chunk** -> 恢复退化(仿真 1% 丢帧下行掉到 0.14
   chunks/cycle)。改成整窗顺序重放(`send_offset_`),1% 丢帧 0.99。教训:仿真丢帧
   路径,不要只测无损。
3. **idle 镜像覆盖未读数据镜像** -> 从站 ISR/门铃的帧间额外 build 用 idle 把主站还没读的
   数据盖掉,每条消息的尾 chunk 都靠 4 帧超时慢修(6.9-8.2 周期/帧,比旧协议还慢)。
   线上探针(pd_stream_probe)逐周期 trace 抓到。教训:仿真必须建模从站的真实调用模式
   (ISR + 门铃 + 主循环的帧间额外 build)——`pd_stream_sim.cpp` 现在就是这么写的。

### 3.4 安全性结论

go-back-N 与 stop-and-wait 提供**完全相同**的无损保证(按序、无重、无丢;TCP/HDLC/LTE RLC
同族)。本轮所有 corruption 均另有根因(旧二进制 vendor id、上述实现 bug、跨核环脏状态),
无一源自窗口语义。仿真 20% 丢帧率 0 字节错误。

**混版本警告:两端必须同版**。真丢帧时新发送端的重放会被旧接收端双重消费。无损链路上
混版本可以互通(方便过渡),但不可长期混跑。

### 3.5 加固路线(建议,未实施)

1. **跨核环索引自愈**(固件一行):`readable()` 发现 `in - out > 容量` 即视为损坏清零——
   直接杀死 4.2 节的故障类,对协议应用也是白赚;
2. **仿真进 CI**:`pd_stream_sim.cpp` 作为第一个测试目标(仓库目前无任何测试),断言
   "交付==发送" @ 随机丢帧 + 随机帧间 build;
3. **协议版本位**:len 字段只需 6 bit,偷 2 bit 放版本号,混版本从"静默损坏"变"链路不通"。

## 4. 运维规程(血泪坑,按出现频率排)

1. **被打断的饱和 loopback 测试会永久损坏跨核环状态**(累计索引损坏 -> `readable()` 天文
   数字 -> core1 无限吐旧数据;12KB 的环"吐出"1.7MB 就是这个)。loopback 镜像没有 session
   层,`rmcs_pd_reset` 设计上不清环,此状态**跨 OP 重入存活,直到断电**。
   **规程:上一次运行若被打断/饱和 -> 板子断电重启 -> `pd_stream_probe N 0 drain` 确认
   安静 -> 再测。** 协议应用有 session 握手,不受此影响。
2. **改过从站身份/映射的提交,测试前必须重编 host**(vendor id 0x511->0x1A81 的提交后,
   18:02 的旧二进制卡 SAFEOP:`slave_online=0`、wc=0、无错误标志,查一晚上不如 rebuild)。
3. IgH master 与 SOEM 不能同时用网卡:`ethercatctl start/stop` 切换;AF_XDP 前置
   (单队列/关 offload)见 `../soem/NIC_TUNING.md`,IgH 原生驱动不需要。
4. 固件构建版本号带 `.dirty` 是正常的(构建重生成 `sec_core_img.c`)。
5. dmesg 的 `NNNNN working counter changes - now 0/3`(约 2x 周期率)是忙轮询
   stop-and-wait/窗口模式的正常现象(WC 每周期 0<->3 翻转),不是错误。

## 5. 业务层分析:4x7=28 电机 @1kHz CAN FD 一发一收

每 tick 每方向 28 x 13B = 364B;每路总线 14 帧。

- **CAN 总线是墙**:FD 8B 载荷 ~60us/帧,14 帧 ≈ 840us/ms,利用率 84%,余量近零;
  64B 载荷物理不可行(~2.1ms > 1ms)。**EtherCAT 侧任何优化都够不着这堵墙**。
- 全系统性价比最高的一张牌:**电机协议层的多电机打包命令帧**(如一帧带 4 电机:
  每路 9 帧 ≈ 540us,全链最坏 ~740us,余量 26%);其次反馈降频、数据段提到 8M。
- EtherCAT 侧:go-back-N 后 28 条突发下行 ~250-380us(原 468us),与 CAN 发送流水重叠;
  上位机打包顺序应对 4 路总线 **round-robin 交错**(否则后拿到命令的总线空转,队头阻塞)。

## 6. 架构选项库(何时升级、升什么)

| 方案 | 延迟形态 | 代价 | 触发条件 |
|---|---|---|---|
| 现状:44B 单流 + go-back-N | 零星 52-78us;突发 ~250-380us | 已付 | — |
| tick-sized PDO(payload=一个 tick 的量) | 突发 1 chunk 直达 | 帧变长伤零星(T(P) 线性),ESI 重刷 | 突发主导且零星不重要 |
| 双槽/更大窗口 + 中等 PDO(如 92B+W2) | 零星 83us,突发 166us | ESI 重刷,帧 +~15us | 零星与突发都要 |
| **混合固定 PDO**(28 电机字段 + 小 ARQ 流) | 周期数据恒 ~1-1.5T,零排队零抖动 | ESI/SSC/双端大改;需 tick 计数器门控防止 CAN 重发 | 业务定型、要确定性 |
| CoE 动态 PDO 映射(Enable PDO Assign/Config) | 同上且尺寸随拓扑现场决定 | SSC 侧开发量最大 | 通用桥要产品化 |

要点:**latest-wins 恰好是周期设定值/反馈想要的语义**(丢一拍被下一拍覆盖、重复读幂等),
所以混合 PDO 的固定区免 ARQ 不是"不安全",是语义正确;事件/配置永远走保留的 ARQ 流。
USB 栈(1023B PacketBuilder 聚合 + 64 URB 深流水 + 硬件事务 ACK)是同构参照:上层聚合
早已存在且传输无关,EtherCAT 侧欠的只是容器/节奏,go-back-N 补的就是节奏。

## 7. 还有提升空间吗(诚实清单,按"每微秒成本"排序)

**便宜且值得(建议做)**
1. 安全加固三件套(3.5 节)——不降延迟,但把这周踩的坑永久锁死;
2. 上位机 4 路 round-robin 打包(队头阻塞,纯 host 一处逻辑);
3. GRUB 攒一次:isolcpus=6,7 + irqaffinity(应用线程尾巴)。

**中等(看需求)**
4. 从站回显尾相位微优化:实测 0.66 chunks/cycle(理论 1.0)、5 周期/64B(理论 4)——差距
   在 ISR/门铃 staging 信用与 core1 产出的错位,可再抠 ~10-20% 吞吐 / ~1 周期延迟;
5. SPSC 环替换(尾巴)。

**贵(业务真需要再动)**
6. 混合固定 PDO:流 RTT 的下一个 2x 在这里(131 -> ~70us 且零抖动),也是 28 电机
   1kHz 的终局形态;
7. 内核态 cycle 应用:最后 2-4us 的 p50。

**不在 EtherCAT 手里**
8. CAN 总线 840us/ms 的墙:打包命令/反馈降频/提波特率,收益比以上全部加起来都大。

一句话:**链路层(系统/主站/协议)已经收敛**——单帧距物理地板 1.7us,流节奏已窗口化,
尾巴 p99=137us。剩余的数量级收益只存在于两处:**架构层(混合 PDO)和业务层(CAN 打包)**。

## 8. 工具索引

- `reference/pd_stream_sim.cpp`:双端仿真(含丢帧 + 固件调用模式),协议改动前必跑;
- `reference/pd_stream_probe.cpp`:线上逐周期 seq/ack/len 探针 + drain 模式(排水/验静);
- `reference/igh_latency_bench.cpp`:单帧 RTT 基准;
- `host/build/examples/ecat_stream_latency`:端到端流 RTT(配 LOOPBACK=1 固件,
  `RMCS_ECAT_BACKEND=soem|igh`,参数:接口 秒数 绑核 inflight);
- 系统调优:`systemctl status rmcs-ecat-tuning`(开机自动)。
