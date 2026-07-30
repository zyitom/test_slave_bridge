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

结论:**core0 = 主机侧域(EtherCAT SSC/ESC 与 USB,两者运行时互斥),core1 = 现场
总线域(MCAN/UART + librmcs 协议)**,以 SHARE_RAM 双环连接(非缓存 + AMO,
acquire/release 原子)。论证:

1. **关键路径无跨核惩罚**:ESC PDI 是 core0 的内存映射外设,MCAN 中断落在 core1,
   两侧各自本核操作;唯一跨核开销是环上一次 memcpy + release store(百纳秒级),
   相对 62.5-125us 的轮询周期可忽略。
2. **抖动隔离是主要收益**:SSC MainLoop 混有 mailbox/CoE/状态机慢路径。若 CAN 与其
   同核,一次 SDO 处理就能让 CAN 帧排队;分核后 core1 的最坏执行时间可控——这比
   吞吐翻倍更重要。
3. **USB 与 EtherCAT 同核,是因为它们互斥,不是因为它们相关**。必须区分两种关系:
   - `USB ⊥ EtherCAT`:**互斥**,同一时刻只有一个主机数据面持有环(仲裁机制见
     `README.md` "USB 协处传输"一节)。
   - `USB ∥ CAN`:**永远并发**。CAN 是同一条数据通路的另一端,主机灌进来的每个
     字节最终都要变成 CAN 帧,两者必然同时达到峰值。

   把**互斥的一对**放同一核 = 构造性零争抢;把**永远并发的一对**拆到两核 = 干扰
   最小。当前布局恰好命中这个配对,这才是 core0 同时承载两种主机传输的实际理由
   (而不是"USB 是附属功能所以随便挂在 core0")。
4. **否决的替代切分**:(a) 全在 core0 单核:失去抖动隔离,未来多路 CAN 中断挤压
   SSC;(b) 协议栈也放 core0、core1 只管外设:协议(反)序列化本就该贴着数据源,
   现状已是最优形态;(c) 按方向切分(上/下行各一核):两核都要碰 ESC 和 CAN,
   引入锁与仲裁,纯负收益;(d) **USB 下放 core1(与 CAN 同核)**:见 3.1,当前否决
   但触发条件明确。
5. 环尺寸(down 4K / up 8K)按"上行遥测 > 下行指令"的流量不对称设计;
   `link_epoch` 只通知不冲刷,session 语义归协议会话层(见"未决问题")。

### 3.1 备选切分 (d):USB 下放 core1(当前否决,触发条件已列)

形态:core0 只跑 EtherCAT(SSC + ESC),core1 = USB + MCAN/UART + 协议。EtherCAT
模式的数据通路不变;USB 模式下主机字节不再过环,直接在 core1 本地进协议栈。

**优点**

1. **USB 路径消灭的不只是跨核 hop,更是非缓存访问**。SHARE_RAM 被 PMP 映射为
   `MEM_TYPE_MEM_NON_CACHE_BUF`,环上每次读写都是非缓存内存访问;(d) 让
   USB -> 协议 -> CAN 全程留在 core1 的可缓存内存里。高吞吐时这一项比那次
   memcpy 的账面数字更值钱。[推断,未实测]
2. **仲裁状态机少一层**:`XcoreChannel::usb_active` 的跨核 ownership 广播可删,
   core1 的 CAN RX ISR 不再需要读它,少一个跨核共享状态即少一类竞态。
3. **代码收敛(可能是最实在的收益)**:`firmware/rmcs_board/app/` 已经是 USB + CAN
   同核的单核固件,(d) 的 core1 约等于 `app/` 加一个 EtherCAT ring 后端。当前
   `app/` 与 `ecat/core1/` 是两套并行演进的实现,(d) 提供了合并的机会。
4. core0 职责单一化,只剩 SSC + ESC。

**缺点**

1. **`USB ∥ CAN` 是永远并发的一对**(见第 3 条),把它们放同核等于让干扰在满负载
   时必然发生,而不是偶发。
2. **中断优先级从"好排"变成没有好解的取舍**:core1 目前只有 CAN/UART/timer,层次
   清晰;加入 USB 后,USB 被饿死 -> 主机侧超时丢包,CAN 被抢占 -> 现场总线 p99
   恶化,两个目标都不能让步。
3. ~~**core1 的 RAM 预算是硬约束,可能直接否掉方案**~~ -> **实测排除,不是阻碍**。
   core1 确实是纯 RAM 镜像(链接脚本 `ram_core1.ld`,由 core0 在启动时拷入 core1
   ILM,`sec_core_img_size = 84472`,**没有 flash XIP 兜底**),所以加 TinyUSB 是直接
   吃 ILM 余量。但实测余量很宽:[实测 2026-07-27,hybrid 变体]

   | 区域 | 容量 | core1 已用 | 占用 |
   |---|---|---|---|
   | ILM `0x00040000` | 256 KB | 70880 B(`.start`+`.vectors`+`.text`+`.eh_frame`+`.fast`) | **27.0%** |
   | DLM `0x00240000` | 256 KB | 35088 B(`.fast_ram`+`.heap`+`.stack`) | 13.4% |

   ILM 尚余 **约 187 KB**,DLM 余约 222 KB;TinyUSB 栈加描述符、FIFO 与 HS 512B 端点
   缓冲是几十 KB 量级,放得下。core1 镜像变大会同步撑大 core0 flash 里内嵌的
   `sec_core_img` 数组,但 core0 FLASH 当前只用 163468 B / 3968 KB(4.02%),同样不构成
   约束。**结论:(d) 的否决理由是缺点 1、2、4、5、6,不是内存。**
4. **USB 控制器是单一硬件,归属互斥 -> DFU 必须一起搬**。不存在"DFU 留 core0、
   vendor 去 core1"的切法。DFU-RT、`BootMailbox::request_enter_dfu()` 的重启路径、
   bootloader 交互都要迁到 core1 并重新验证——**刷机路径失效等于变砖**,迁移风险
   不可低估。
5. **失去"换主机传输不动 core1"这一性质**(现状见 `README.md`:"代码全在 core0,
   core1 一行不动")。(d) 之后任何 USB 侧改动都要碰硬实时核,每次都需重验实时性,
   这是持续的维护成本而非一次性代价。
6. **USB 模式下 (d) 退化为事实单核**:core1 扛全部负载,core0 只剩一个没有主站的
   SSC 空转。当前切分在两种模式下两个核都在做实事。

**结论与触发条件**

代价的大小取决于 **USB 的实际占空比**——缺点 1、2 只在"USB 与 CAN 同时满负载"时
兑现:

- USB 仅作调试/备用通道、生产跑 EtherCAT:该场景几乎不发生,缺点 1、2 大幅缩水,
  只剩缺点 3、4 的一次性工程代价,此时优点 3 的代码收敛可能划算。
- USB 与 EtherCAT 平级、都要满负载:(d) 不划算,缺点 1、2 是持续性的。

**当前否决**。重评估的前置数据原本有两项,其中一项已完成:

1. **待做**:用 `host/examples/usb_canfd_stress.cpp` 分别对 `app/`(单核)与 `ecat/`
   (现状)跑同一压力,差值即 (d) 在 USB 路径上能拿回的**上限**。差值小则 (d) 的性能
   理由不成立,只剩代码收敛一条。需要板子。
2. ~~核算 core1 ILM 余量~~ **已完成**(见缺点 3):ILM 占用 27.0%,余约 187 KB,
   放得下 USB 栈,**不构成阻碍**。

### 3.2 常见误读:"USB 模式下 SSC 不运行"

**互斥的是数据面,不是 CPU 执行**。`core0/src/ecat_main.c` 的主循环无条件调用
`MainLoop()`,没有 `usb_active` 守卫;仲裁只让 PDO 钩子变惰性
(`rmcs_pd_set_usb_active`)。

但**这不构成 USB 路径的性能负担**:USB 模式下没有主站,从站停在 INIT,
`bEcatOutputUpdateRunning` / `bEcatInputUpdateRunning` 均为 FALSE,
`PDO_OutputMapping()` / `PDO_InputMapping()` 不执行,邮箱为空使 `COE_Main()` 立即
返回,`ECAT_Main()` 只走状态机空转分支。此时 `MainLoop()` 退化为**几次 ESC 寄存器
读加几个立即返回的调用**。[推断,未实测]

第 4 节延迟预算中"MainLoop 慢路径几十 us"的量级**只适用于有主站驱动的 EtherCAT
模式**,不可套用到 USB 模式——两者混用会得出"USB 被 SSC 拖累、应优化 MainLoop"
这一错误结论(该结论曾被提出并已撤回)。

### 3.3 与 HPM SDK `samples/multicore` 的逐项对照(已消化,勿重复调研)

对照日期 2026-07-27,SDK 版本 v1.12.0。结论:**该目录能提供的机制已全部评估,无遗漏**。

| SDK 示例 | 机制 | 本项目 |
|---|---|---|
| `hello` | `multicore_release_cpu()` + `sec_core_img` 内嵌次核镜像 | **已采用**,同一形态(`core0/src/ecat_main.c` + `core0/src/sec_core_img.c`) |
| `mbx` | MBX 硬件邮箱 + 中断 | **已采用**为上行门铃(core1 敲 `HPM_MBX0B`,core0 吃 `HPM_MBX0A`) |
| `mbx` 的下行方向 | core0 -> core1 也用中断通知 | **有意不用**:core1 忙轮询 `down` 环的延迟低于中断进出开销 |
| `mbx` FIFO 模式 | `TFMA`/`RFMA`,一次搬 4 word | **有意不用**:门铃语义只需单 word,环才是 source of truth,发送失败可直接忽略(已有 poke 在途) |
| `erpc` 系列 | eRPC / rpmsg 远程调用 | **否决**:序列化开销与 rpmsg 传输层同"最低延迟字节流"的目标相反 |
| `console_coremark`、`lvgl_coremark` | 跑分演示 | 无关 |

两处本项目**超出** sample 水平、需要保留的实现细节:

1. **门铃前的 full fence**。`mbx` sample 没有这一步。`XcoreRing::try_push` 结尾的
   release store 只保证 payload 排在索引之前,**不保证**该非缓存 store 排在随后的设备
   寄存器写之前;缺这道 `fence` 会让 core0 看到门铃却读到旧数据。见
   `core1/src/main.cpp` 的 `uplink_doorbell_ring()`。
2. **SHARE_RAM 的 PMP 非缓存 + AMO 映射**,以及跨 hart 为何不能沿用单核那套
   `atomic_signal_fence` 配方——`samples/multicore` 完全没有覆盖这一层,由本项目自行
   补齐,论证写在 `common/xcore_ring.hpp` 的头注释。

附带发现(非 multicore,但与本桥相关):SDK v1.12.0 起 `arch/riscv/intc/hpm_interrupt.h`
自带 SEGGER SysView 埋点(`TRACE_EXT_ISR_ENTER/EXIT`,未定义 `CONFIG_SEGGER_SYSVIEW`
时展开为空)。配合 `samples/segger_sysview/baremetal` 可以零改动拿到**双核 ISR 级时间
线**,对第 4 节的延迟项标定比在 ISR 里打时间戳再 printf 更合适。尚未启用。

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
