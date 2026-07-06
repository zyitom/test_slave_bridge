# EtherCAT 主站延迟调查记录:从 "找不到从站" 到 IgH 原生驱动

这是一份过程记录,不是实现文档(实现规格见 `DESIGN.md`)。目的是把排查过程和每一步的
证据留下来,避免以后重新踩一遍坑,也避免以后有人问"为什么不干脆用 SOEM 就行了"时,
翻不到当初的对比数据。

## 起点:AF_XDP zero-copy 模式下 "No EtherCAT slave found on the network"

现象:`host/third_party/soem-afxdp/nicdrv.c`(SOEM 的 AF_XDP 替换驱动)在 zero-copy 模式下
打印绑定成功(`ZERO-COPY` 横幅),但 `ec_config_init` 广播发现帧超时,一个从站都找不到。

排查过程(详见对话历史,这里只记结论):
- 网卡/系统配置(队列收拢、offload、C-state、governor)逐一核对,均正常——完整清单见
  `host/src/transport/soem/NIC_TUNING.md`。
- 用 `RMCS_XDP_COPY=1` 强制走 COPY 模式,能正常发现从站、稳定跑 22.6~22.8kHz,证明从站、
  接线、固件、SOEM 协议逻辑全部没问题。
- 用 `ethtool -S enp2s0` 的硬件级计数器定位:zero-copy 模式下 `tx_queue_0_packets` 正常
  增长(发送没问题),但 `rx_queue_0_packets` 全程一动不动,busy-poll 开/关都一样(排除了
  PREEMPT_RT 忙轮询饿死 NAPI 线程的猜测)。
- 结论:igc 驱动在这颗内核(`6.8.1-1015-realtime`)上的 zero-copy 接收路径本身有问题,
  在应用层代码触及范围之下。

## CPU/系统调优

顺带发现并修复:governor 是 `powersave`、C-state 没关(`intel_idle` max_cstate=9),这两项
和 zero-copy 问题无关,但直接影响延迟尾部(体现为 rtt `max` 出现几十到几百毫秒的离群值,
是 CPU 从深度睡眠唤醒或者被其他任务抢占的典型症状)。已改成 `performance` + 关闭 C1 以上
睡眠态,完整清单和验证命令见 `host/src/transport/soem/NIC_TUNING.md`。

**这两项是运行时设置,重启会恢复默认,目前没有做成开机自动生效。**

## SOEM 两种方案的实测对比

同一块 rmcs_board 从站,100Mbit,64 字节负载,`enp2s0`(igc),已完成上述 CPU 调优:

| 配置 | cycle rate | rtt p50 | rtt p90 | rtt p99 | rtt max | corrupt 帧 |
|---|---|---|---|---|---|---|
| SOEM + 纯 AF_PACKET(不用 XDP) | 21.0 kHz | 373.0 us | 386.1 us | 399.6 us | 2162.9 us | 2 |
| SOEM + AF_XDP `RMCS_XDP_COPY=1` | 22.8 kHz | 164.7 us | 175.2 us | 252.9 us | 82248.4 us | 0 |

结论:AF_XDP 的收益主要来自 busy-poll 免中断等待,不是 zero-copy 本身(zero-copy 对
64~128 字节的小包能省下来的只是一次内存拷贝,量级在几十到几百纳秒)。zero-copy 修不好的
情况下,COPY 模式已经是明显更优的选择,不要为了追求 zero-copy 放弃 AF_XDP 换回纯 socket。

## 换个方向:IgH EtherCAT Master 原生驱动

用户问"是不是只能换网卡了",顺着这个思路查了 IgH EtherCAT Master(和 SOEM 平级的另一套
开源主站协议栈)的原生驱动方案——它不是在 socket 层面优化,而是把网卡驱动整个接管替换掉,
主站状态机跑在内核模块里,直接操作驱动的收发环,完全绕开 socket/AF_XDP/BPF。

### 可行性确认

- 官方仓库 `gitlab.com/etherlab.org/ethercat`,最新稳定 tag 是 `1.6.9`(2026-03-26 发布)。
- `igc`(i225/i226)的 6.8 内核支持是 **2026-05-07** 才合并进 `stable-1.6` 分支的(commit
  "Add igc for Kernel 6.8.2" by Bjarne von Horn),比 1.6.9 还新——必须从 git 主干构建,
  不能用官方打包的 release。
- 本机内核是 `6.8.1-1015-realtime`,和驱动针对的 `6.8.2` 版本号很接近(Ubuntu realtime
  内核在 6.8.1 基础上打实时补丁),实测编译一次性通过,没有 API 不兼容问题。
- 本机环境本来就绿灯:精确匹配的内核 headers 已装好、Secure Boot 已关闭(自定义内核模块
  不用签名就能加载)、编译工具链齐全。
- IgH 在 PREEMPT_RT 内核下是主流部署方式之一(同一批提交历史里有专门为 Xenomai/实时内核
  修的 bug),没有架构性冲突。

### 实际编译 + 安装 + 接管网卡(已在 TL101 上完成,详细命令见 `DESIGN.md` 前置条件一节)

1. `git clone` 主干、`./bootstrap`、`./configure --enable-igc --with-linux-dir=...`、
   `make modules` —— 编译一次通过,生成 `ec_igc.ko`、`ec_master.ko`。
2. `make modules_install` 后 `depmod` 因为缺 `System.map` 被静默跳过,手动
   `sudo depmod -a $(uname -r)` 补上,不然 `modprobe` 报 "Module ec_master not found"。
3. `make install` 装好用户态 `libethercat`、`ethercat` 命令行工具、`ethercat.conf` 配置模板、
   `ethercat.service`(systemd unit,已安装但未 enable)。
4. 配置 `ethercat.conf`:`MASTER0_DEVICE="00:90:27:e7:75:35"`(enp2s0 的 MAC),
   `DEVICE_MODULES="igc"`。
5. **动手前确认了安全性**:`enp2s0` 是 `172.16.0.1/24`,SSH 和默认路由都走 `wlp4s0`(WiFi),
   `enp2s0` 上没有任何存活连接——接管它不会断连接。
6. `sudo /usr/local/sbin/ethercatctl start`:标准 `igc` 模块被卸载,`ec_igc`/`ec_master`
   接管。dmesg 直接看到 `EtherCAT: Accepting 00:90:27:E7:75:35 as main device`,然后
   `1 slave(s) responding`,从站自动进入 `PREOP`——内核模块自己的空闲态扫描做到的,应用程序
   还没写。

**副作用**:`DEVICE_MODULES="igc"` 接管这台机器上**所有** igc 网卡,`enp3s0`(本来就
DOWN、没在用)也被一起接管了。

**回滚方式**:`sudo /usr/local/sbin/ethercatctl stop` —— 对称卸载 `ec_igc`/`ec_master`,
重新加载标准 `igc`,`enp2s0`/`enp3s0` 恢复成普通网卡。`ethercatctl` 脚本本身就是这么设计的,
不需要额外脚本。

### 踩的坑:分布式时钟(DC)

第一次写基准测试卡在 PREOP/SAFEOP+ERROR 上不去 OP,dmesg 线索:
`EtherCAT WARNING 0: No application time received up to now, but master already active.`
这个从站被自动选为 DC 参考时钟,ecrt 要求每个 cycle 在 `ecrt_master_send()` 之前调用
`ecrt_master_application_time()` + `ecrt_master_sync_reference_clock_to()` +
`ecrt_master_sync_slave_clocks()`,漏了就永远到不了 OP。参考 IgH 源码里的
`examples/dc_user/main.c` 才找到这个调用顺序。详见 `DESIGN.md`。

### 独立基准测试结果(不是正式集成,只是验证收益够不够大)

代码:`reference/igh_latency_bench.cpp`(跑通的独立程序,不参与 CMake 构建)。用真实的
PDO 布局(从 `ethercat pdos -p 0` 读出来的,SM2/SM3 各 128 字节),stop-and-wait 单帧在途,
和之前 SOEM 测试完全同口径(同一颗 CPU7、SCHED_FIFO 80、同一块从站)。

两次独立运行:

| 运行 | cycle rate | rtt p50 | rtt p90 | rtt p99 |
|---|---|---|---|---|
| 第一次 | 22.1 kHz | 42.9 us | 44.5 us | 50.7 us |
| 第二次 | 22.1 kHz | 43.0 us | 44.6 us | 49.4 us |

对比 SOEM 最好的结果(AF_XDP COPY 模式 164.7us),p50 再降了约 **3.8 倍**。`max` 出现过
49ms 左右的离群值(两次运行都有,个位数次/22 万次 cycle),和 SOEM 各配置下都出现过的大尾巴
是同一类现象(推测是系统级调度卡顿,不是这套驱动架构特有的问题),不影响 p50/p90/p99 这些
真正反映架构差异的数字。

### 结论 + 下一步

收益远超预期(不是"省一次内存拷贝"级别的提升,是整层内核网络栈被绕开的提升),值得正式
集成进 librmcs。正式集成的实现规格见 `DESIGN.md`。

## 正式集成后的 like-for-like 实测(2026-07-05,重要修正)

`igh.cpp` 写完并集成进 `ecat_stream_latency`(`RMCS_ECAT_BACKEND=igh`)后,在同一台机器、
重新应用了 CPU 调优(governor=performance、深度 C-state 关闭;`isolcpus=7` 本来就在内核
cmdline 里)之后,做了一次**同口径**对照。这一步暴露了前面"4x"结论的一个方法学问题。

| 测法 | 口径 | p50 | p99 | max | 周期率 | corrupt |
|---|---|---|---|---|---|---|
| `igh_latency_bench`(裸 ecrt,单帧) | **单帧 RTT** | 42.5us | 52.4us | 50.0ms | 22.2kHz | - |
| `ecat_stream_latency` + `igh`(新后端) | **流 RTT(ARQ echo)** | 172.0us | 262.8us | 49.2ms | 22.2kHz | 0 |
| `ecat_stream_latency` + SOEM AF_XDP(历史) | 流 RTT(ARQ echo) | 164.7us | 252.9us | 82.2ms | 22.8kHz | 0 |

关键观察:

1. **三者周期率几乎一样(~22kHz,即帧周期 ~45us)。** stop-and-wait 下帧周期≈单帧 RTT,
   所以 SOEM AF_XDP 的**单帧** RTT 其实也是 ~44us,不是 164.7us——164.7us 是它的**流** RTT。
2. **流 RTT = 4.0 x 单帧 RTT**(172.0 / 42.5 = 4.05)。一个字节要穿过 ~4 个 EtherCAT 帧周期
   才能经 ARQ + 从站双核 echo(core0->core1->core0)往返回来。这 ~4 周期的乘数是**协议/固件
   固有**的,和主站用哪套驱动无关。
3. 因此**在应用/流这一层,IgH 原生驱动相对 SOEM AF_XDP 没有可见收益**(172us vs 165us,在
   run-to-run 噪声范围内)。AF_XDP 的 busy-poll 早就把 socket 栈那点延迟消掉了,真正的瓶颈
   是 100Mbit 帧周期(~45us)+ ~4 个 ARQ-echo 周期,这两项换主站驱动都不动。

**"IgH 比 SOEM 快 4x" 是把 IgH 的单帧微基准(42.9us)和 SOEM 的整流测试(164.7us)放一起比
出来的,不是同口径。** 同口径下:单帧层面两者都 ~43-44us(打平,IgH 也许有微弱边际优势);
流层面两者都 ~170us(打平)。

对实现本身的正面验证:`igh.cpp` 的流测试周期率(22.2kHz)和裸 ecrt bench(22.2kHz)**完全
一致**,说明 transport 的 cycle_loop **没有引入任何每周期额外开销**(DESIGN.md 里担心的"慢到
接近 SOEM"没有发生;之所以流 RTT 和 SOEM 接近,是因为瓶颈在 ARQ echo,不在 transport 效率),
而且 0 corrupt / 能到 OP / DC 三件套正确,功能与可靠性达标。

**结论修正 + 对路线图的影响**:降**流** RTT 的最大杠杆不是换主站驱动,而是
(a) 缩短帧周期(PDO 右尺寸,但只影响 ~4 个周期里每个的一部分),更重要的是
(b) **压缩那 ~4 个 ARQ-echo 周期数**(core0<->core1 的往返 + 上下行 ARQ 各自的 seq/ack 握手)。
若能把 echo 从 ~4 周期压到 1-2 周期,流 RTT 大致减半——这是固件/ARQ 协议侧的活,不是主站侧。
详见 `LATENCY_ROADMAP.md`(据此结论已相应调整优先级)。IgH 后端保留为可选项:它单帧更干净、
绕开内核网络栈,在别的使用模式(比如未来非 stop-and-wait、多帧在途的高频控制)下仍可能有价值,
但对当前这个 stop-and-wait 流桥,不必期待它带来流延迟下降。

### 想做同一天的 SOEM 对照,结果被从站状态污染(操作性发现)

上表 IgH 的两行是**干净数据**:本次开机后 IgH 是第一个碰这块从站的主站,从站是干净上电态,
0 corrupt、22.2kHz(和裸 bench 完全一致)。但当我 `ethercatctl stop` 把网卡还回标准 igc、
想用 SOEM 跑一遍同口径对照时:

- 第一次:从站直接掉到 **SAFE-OP+ERROR(AL status 0x001B)**,220/220 帧 corrupt,跑不起来。
- 第二次(第一次的 teardown 把从站复位回 INIT 后):能上 OP、0 corrupt,但**只有 4.0kHz /
  p50 488us**,远差于历史干净值 22.8kHz / 164.7us——每个 SOEM cycle 阻塞 ~250us 等从站翻帧,
  是**从站侧变慢**,不是主站侧。

原因:IgH 用 DC 配了从站,SOEM 是 free-run,**在不给从站板子断电重上电的情况下切换主站栈**,
从站 ESC 里残留的 DC/watchdog 配置没被干净复位,导致 SOEM 这侧要么报错要么慢。所以**同一天的
SOEM 数字是被污染的,不可用**;干净的 SOEM 基线还得用历史值(164.7us,那是在干净上电从站上测的)。

**操作性结论:两套主站栈之间切换,必须给从站板子断电重上电,否则从站会处于降级/报错态。**
这也是为什么上表 SOEM 那行标的是"历史"——不是偷懒,是当天没法在同一块被 IgH 碰过的从站上
拿到干净 SOEM 数。

## 当前系统实际状态(重要,别忘了)

本次会话(2026-07-05,写完 igh.cpp + 跑完测试)结束后,TL101 处于以下状态:

- **`ethercat` 服务当前是停止的**:已 `ethercatctl stop`,`enp2s0`/`enp3s0` 是标准 `igc` 普通
  网卡,`/dev/EtherCAT0` 不存在。要跑 IgH:`sudo /usr/local/sbin/ethercatctl start`(接管网卡,
  等约 4-8s 链路 UP 后从站进 PREOP)。
- **从站板子当前处于被 IgH->SOEM 切换污染的降级态**(最后一次是 SOEM 慢速跑)。下次不管用哪套
  栈跑之前,建议先给从站板子**断电重上电**,拿干净基线。IgH 因为会重新 DC 配置从站,start 后
  多半能自愈到干净态;SOEM 则更依赖从站是干净上电态。
- `ethercat.service` **没有** enable,重启机器会恢复成标准 `igc`(除非之后手动
  `systemctl enable ethercat`)。
- `host/third_party/soem-afxdp/nicdrv.c` 已经换回 AF_XDP 版本(不是纯 AF_PACKET),
  `/home/helios/3rd_party/soem-1.4.0/oshw/linux/` 下两份备份都在
  (`nicdrv.c.afpacket.bak` 和 `nicdrv.c.afxdp.bak`)。
- IgH 主站源码克隆在 `/tmp/.../scratchpad/igh/ethercat`(临时目录,**会话结束后大概率被
  清理**,不要依赖这个路径长期存在;真正需要的产物——头文件、库、命令行工具、systemd
  unit、内核模块——都已经 `make install` 到 `/usr/local/` 和 `/lib/modules/` 下,不依赖
  临时目录)。
- CPU governor(`performance`)和 C-state(CPU 2/6/7 上关到只剩 POLL)是运行时设置,
  重启失效,需要的话参考 `host/src/transport/soem/NIC_TUNING.md` 里的命令重新跑一遍,或者
  做成开机脚本(目前没做)。
