# EtherCAT 转发桥设计决策与延迟优化路线

> **文档类型**：背景说明（选型论证）
> **适用范围**：`firmware/rmcs_board/ecat/`，EtherCAT 转发桥
> **状态**：现行有效
> **相关文档**：[README.md](README.md)（实现现状与操作） · [LINKX_HW_ACCEL_PLAN.md](LINKX_HW_ACCEL_PLAN.md)（硬件加速规划） · [host 侧延迟结论](../../../host/src/transport/igh/LATENCY_ROADMAP_V2.md)

## 摘要

本文件回答的是**为什么**：为什么选这种同步模式、为什么自己设计应用层协议而不用现成的、
双核为什么这样分工、延迟预算花在哪里。每个决策都写了备选方案和排除理由。

**注意本文所有结论都绑定在一组场景约束上**（单从站、转发的是事件而非状态、优化目标是
延迟而非吞吐）——场景变了，结论需要重新评估。约束原文见下方。

本文档记录 rmcs_board EtherCAT 流桥(HPM6E 双核)的选型论证:同步模式、应用层协议、
双核分工,以及按收益排序的延迟优化路线。实现现状见 `README.md`;线格式见
`core/include/librmcs/ecat/pd_stream.hpp` 头注释。

场景约束(所有结论都以此为前提,场景变了结论要重新评估):

- **单从站**:x86 上位机(i226 网口, SOEM)直连一块转发板,无多从站协同;
- **转发的是事件,不是状态**:CAN 帧/UART 字节是一次性事件,丢失或重复都会破坏语义,
  这与经典运动控制"周期性设定值/反馈值"(latest-wins 可接受)本质不同;
- **优化目标是端到端延迟与抖动**,吞吐只需容纳数路 1Mbps CAN(每方向 ~0.5-2 MB/s)。

## 1. 同步模式选型

EtherCAT 从站应用有三种运行模式,对"转发桥"的取舍与教科书场景(运动控制)相反:

| 模式 | 机制 | 对转发桥的延迟影响 | 结论 |
| --- | --- | --- | --- |
| Free-run | 从站主循环自由轮询 PD | 取数延迟 = 主循环周期的随机相位(SSC MainLoop 混有 mailbox 慢路径,估 0~60us 抖动) | **P1 采用**:最简单,先打通 |
| SM-synchron | SM2 写事件触发 PDI 中断,数据到达即处理 | 取数延迟降到中断响应(~2-5us,确定性) | **P3 目标**:单从站转发的延迟最优解 |
| DC-synchron | 全局时钟 SYNC0 沿触发 | **增加**延迟:数据到达后要等到下一个 SYNC0 沿才处理;换来的是多从站间 <1us 的同步性 | 不采用 |

关键认知:**DC 解决的是"多从站同时动作",不是"快"**。单从站转发场景下 DC 纯粹是负收益
(平均多等半个周期)。SSC 已按 `AL_EVENT_ENABLED=1`、`DC_SUPPORTED=1` 生成,三种模式
运行时由主站配置选择,固件从 free-run 切到 SM-synchron 无需重新生成协议栈,只需把
PDO 处理挂进 PDI ISR(SDK 的 `ssc_pdi_mask.patch` 已把 SYNC0/1 从 PDI 中断源中屏蔽,
就是为这条路径准备的)。

主站侧对应选择 **free-run busy-poll**(SOEM 不开 DC):独立线程满速
send/receive_processdata,轮询率即链路调度粒度。8-16kHz 起步;本 PD 尺寸(双向各
48B)下线缆占用明显低于 128B 旧基线,实测丢 wkc 前可继续上探。

## 2. 应用层协议选型

候选方案与否决理由:

1. **stream-over-PDO + 停等 ARQ(采用)**。librmcs 协议字节流原样跑在 PD 上,
   `[seq:u8][ack:u8][len:u16][payload:44B]`。
   - 为什么必须有 ARQ:SyncManager 三缓冲是 latest-wins——同一 chunk 可能被重复读、
     也可能没被读就被覆盖。对状态量无所谓,对字节流是致命的。seq/ack 停等恰好补齐
     "恰好一次"语义,且重传免费(数据留在 PD 镜像里,主站下个周期自然重发)。
   - 最大优点:**上位机与固件的整个 librmcs 协议栈原样复用**,EtherCAT 只是替代 USB
     的又一个 transport(host 侧已按此实现)。CAN/UART/GPIO 的既有语义、工具、示例
     全部不动。
2. **原生 PDO 映射**(每路电机/CAN 通道映射为固定 PD 字段,CiA402 风格)。周期性
   设定值场景的正统做法,无 ARQ 往返;但事件类流量(DM 电机应答、UART、错误帧)
   在 latest-wins 下会丢,等于要为"周期量走 PD、事件量走另一条路"维护两套栈和
   固定 schema,host/固件双侧大改。**否决**;若未来出现真正的高频周期量瓶颈,可做
   混合布局(PD 内划一段 latest-wins 状态区 + 一段流区),接口已预留扩 PD 的空间。
3. **CoE SDO**:mailbox 轮询路径,毫秒级,只配置用。**否决**(数据面)。
4. **EoE / VoE**:同为 mailbox 路径,且 EoE 还要挂网络栈。**否决**。
5. **FoE**:与数据面无关,P3 用于固件升级,保留。

## 3. 双核分工(HPM6E 双核的利用)

结论:**core0 = EtherCAT 域(SSC + ESC),core1 = 现场总线域(MCAN/UART + librmcs
协议)**,以 SHARE_RAM 双环连接(非缓存 + AMO,acquire/release 原子)。论证:

1. **关键路径无跨核惩罚**:ESC PDI 是 core0 的内存映射外设,MCAN 中断落在 core1,
   两侧各自本核操作;唯一跨核开销是环上一次 memcpy + release store(百纳秒级),
   相对 62.5-125us 的轮询周期可忽略。
2. **抖动隔离是主要收益**:SSC MainLoop 混有 mailbox/CoE/状态机慢路径。若 CAN 与其
   同核,一次 SDO 处理就能让 CAN 帧排队;分核后 core1 的最坏执行时间可控——这比
   吞吐翻倍更重要。
3. **否决的替代切分**:(a) 全在 core0 单核:失去抖动隔离,未来多路 CAN 中断挤压
   SSC;(b) 协议栈也放 core0、core1 只管外设:协议(反)序列化本就该贴着数据源,
   现状已是最优形态;(c) 按方向切分(上/下行各一核):两核都要碰 ESC 和 CAN,
   引入锁与仲裁,纯负收益。
4. 环尺寸(down 4K / up 8K)按"上行遥测 > 下行指令"的流量不对称设计;
   `link_epoch` 只通知不冲刷,session 语义归协议会话层(见"未决问题")。

## 4. 端到端延迟预算(@16kHz busy-poll,估算值,待 P1 实测校准)

```
上位机 app -> tx ring          <1us   (mutex + memcpy)
等待下一个轮询相位              0~62.5us, 平均 31us   <- 主要项 A
线缆 + ESC 转发                ~30us(帧上线) + ~1us
core0 取数(free-run)          0~60us 抖动           <- 主要项 B
跨核环 -> core1                <1us
MCAN 发送(1Mbps 经典帧)       ~110us                <- 物理下限,占大头
```

反向路径对称,再加 ARQ 确认一个周期。**架构不是瓶颈,轮询相位(A)、free-run 取数
抖动(B)和 CAN 线速才是**,优化路线因此如下排序。

## 5. 延迟优化路线(按收益/成本排序)

**已完成**

- soft-float ABI(免去每次中断 20 个 FP 寄存器保存,见顶层 CMakeLists 注释);
- ESC 复位随 MCU 整体复位(避免半复位状态);
- host transport 独立 busy-poll 线程 + `thread_setup` 钩子(绑核/RT 优先级由调用方注入);
- **跨核上行敲钟(MBX 事件驱动)**:core1 回复就绪即敲 `HPM_MBX0B`,core0 在
  `HPM_MBX0A` ISR 里立即映射上行,把"回复就绪 -> 写进 ESC"的周转从 SSC 主循环
  粒度(混有 mailbox/CoE 慢路径,几十 us 抖动)压到中断延迟(~1-2us)。这正是
  第 3 节抖动隔离的收尾:瓶颈曾从 core1 的慢路径转移到"core0 主循环粒度拖累了
  转发时机",敲钟把上行发布改为事件驱动后消除了这层抖动。MBX 优先级低于 ESC
  PDI、刷新路径 `DISABLE_ESC_INT` 双向互斥、ISR 落 ILM;`pAPPL_MainLoop` 钩子
  保留为兜底(实现见 `README.md` "上行即时刷新")。

**低垂果实(P1 实测后立即做)**

1. **上位机运行环境**:隔离核(`isolcpus`+`nohz_full`+`rcu_nocbs`)、SCHED_FIFO、
   i226 关中断合并(`ethtool -C <if> rx-usecs 0`)、关 EEE(`ethtool --set-eee <if> off`)。
   预期把项 A 之外的主站侧抖动压到 <5us。PREEMPT_RT 内核对尾延迟(p99)有额外收益。
2. **提高轮询率**:48B PD 下 16kHz 无压力;实测丢 wkc 前可继续上探。

**中等投入(P3,按实测决定)**

3. **SM-synchron(PDI ISR)**:消除项 B(0~60us -> ~2-5us 确定性)。从站单侧改动,
   预期是**从站侧最大的单项收益**。
4. **ARQ 加窗**:停等的有效吞吐是每 ~2 周期 44B(发出后要等下周期的 ack)。若多路
   CAN 遥测把上行打满,把 PD 扩为 N 槽滑窗(如 4x48B=192B,PD 上限已由
   `MAX_PD_*_SIZE` 参数化;ESC RAM 余量以 RAM_SIZE 寄存器 0x0006 实测为准,例程布局
   0x1000-0x14FF 只用了零头)。注意:**加窗提升吞吐,不降低单帧延迟**,且 512B PD
   使帧上线时间 ~90us、轮询率上限降到 ~10kHz——是"吞吐换轮询率"的权衡,没有遥测
   饱和证据前不做。
5. **chunk 内多帧打包**:librmcs 帧远小于 44B 时,一个 chunk 天然承载多帧(pop 是
   字节流语义,已支持);无需改动,列出仅为说明吞吐余量。

**不做/暂不做**

- AF_XDP/DPDK 版主站:raw socket + 隔离核 busy-poll 的 NIC 往返已在 ~10us 量级,
  重写 SOEM 网络层的收益对不起成本;
- DC 同步:见第 1 节,单从站转发是负收益;
- CAN-FD:若下游设备支持 5Mbps FD,帧时间从 ~110us 降到 ~30us——这是**现场总线侧**
  最大的延迟杠杆,但取决于电机/设备生态,不是 EtherCAT 侧的决策。

## 6. 未决问题

- **session policy**:OP 重入时跨核环内残留数据冲刷还是保留(现仅 bump
  `link_epoch`),与协议会话层一起在 P2 定;
- 滑窗 ARQ 的槽数/PD 尺寸,等 P1 的延迟/吞吐扫描数据(`ecat_stream_latency` 输出
  RTT 分位数与吞吐)再定;
- 产品化前需申请 ETG Vendor ID(现沿用 HPMicro 示例 ID)。
