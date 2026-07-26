# EtherCAT 主站网卡调优清单（host/soem 传输层）

> **文档类型**：现行规范（运行环境准备清单）
> **适用范围**：`host/src/transport/soem/`，EtherCAT 主站所在机器的网卡与系统配置
> **状态**：现行有效
> **相关文档**：[../igh/EVALUATION.md](../igh/EVALUATION.md)（选型对比） · [../igh/LATENCY_ROADMAP_V2.md](../igh/LATENCY_ROADMAP_V2.md)（系统调优的持久化做法） · `host/third_party/soem/README.afxdp.md`

## 摘要

本文件是一份**一次性环境准备清单**：机器重装或换机器后，照着过一遍，让 EtherCAT 主站
达到应有的低延迟。内容与 librmcs 代码无关，全部是网卡、内核、CPU 调度层面的配置。

验证环境是 Intel igc（i225/i226）网卡 + Ubuntu `*-realtime` 内核；不同网卡/内核的具体
现象会有差异，但检查思路通用。

> **补充**：本文的调优项如何做成**开机持久生效**，见
> [../igh/LATENCY_ROADMAP_V2.md](../igh/LATENCY_ROADMAP_V2.md) 第 1 节——那里记录了
> 一个"RT throttling 导致 RTT max 出现约 50 ms 尾巴"的根因修复，运行实时任务前务必了解。

本文档记录让 `soem.cpp`(可选 `LIBRMCS_SOEM_AFXDP=ON` 的 AF_XDP nicdrv 变体,见
`host/third_party/soem/README.afxdp.md`)达到低延迟所需的**主站侧网卡/系统配置**。
这些配置和 librmcs 代码无关,是运行环境的一次性准备,机器重装或换机器时按本清单过一遍。

验证环境:Intel igc(i225/i226)网卡,Ubuntu `*-realtime` 内核。不同网卡/内核的具体现象
可能不同,但检查思路通用。

## 结论先行

- 是否需要 zero-copy 才能跑:不需要。`RMCS_XDP_COPY=1`(AF_XDP COPY 模式,仍然走
  busy-poll)相比完全不用 XDP 的纯 `AF_PACKET`,实测 p50/p90 延迟低 2 倍以上(见下方
  “实测对比”)。AF_XDP 真正的收益主要来自 busy-poll 免中断等待,不是 zero-copy 本身——
  对 EtherCAT 这种 64~128 字节的小包,zero-copy 能再省下来的只是一次几百字节的内存拷贝,
  量级在几十到几百纳秒,不是决定性的。
- 换更好的网卡/换驱动版本 **可能**修好 zero-copy(见“已知问题”一节),但对这个帧长度,
  预期收益有限,优先级低于下面这些系统调优项。

## 一次性网卡准备

EtherCAT 收发必须落在 AF_XDP socket 绑定的那个队列上(默认 queue 0),所以先把网卡收拢成
单队列,并关闭可能打乱队列分发/篡改帧内容的 offload:

```bash
IFACE=enp2s0   # 替换成实际接口名

sudo ethtool -L $IFACE combined 1        # 所有 RX 收到 queue 0(AF_XDP 绑定的队列)
sudo ethtool -K $IFACE ntuple off        # 关闭 flow steering,避免帧被分流到其他队列
sudo ethtool -K $IFACE rx off tx off     # 关闭 rx/tx checksum offload
sudo ethtool -K $IFACE gro off           # 关闭 GRO,避免帧被合并/延迟
sudo ethtool -K $IFACE rxvlan off        # 关闭 VLAN offload(EtherCAT 不用 VLAN)
sudo ethtool -C $IFACE rx-usecs 0 tx-usecs 0   # 关闭中断合并,不为省中断攒帧
```

改完 `ethtool -L` 之后建议 down/up 一次接口,确保驱动按新队列数重建 ring:

```bash
sudo ip link set $IFACE down
sudo ip link set $IFACE up
```

验证:

```bash
ethtool -l $IFACE   # Current hardware settings 里 Combined 应为 1
ethtool -k $IFACE   # ntuple-filters/rx-checksumming/generic-receive-offload 均应为 off
ethtool -c $IFACE   # rx-usecs/tx-usecs 应为 0
```

## CPU / 调度调优

主站的 cycle 线程以 `SCHED_FIFO` 高优先级绑核忙轮询,任何该核上的调频延迟、深度睡眠唤醒
延迟都会直接体现成 EtherCAT 周期的尾部延迟(rtt `max`)。

### 内核启动参数(隔离 cycle 线程所在核)

```
isolcpus=<N> nohz_full=<N> rcu_nocbs=<N>
```

`<N>` 替换成 cycle 线程实际绑定的核号(通过程序日志 `cycle thread: pinned to CPU N` 确认)。
这一项是内核命令行参数,改完需要重启生效,通常在装机时就该定好。

### CPU 频率与 C-state(运行时可调,重启失效)

对 cycle 线程所在核、以及网卡 RX/TX 中断实际落在的核(见下方“IRQ 亲和”),都要做:

```bash
CPUS="2 6 7"   # 替换成实际用到的核号

sudo cpupower -c $CPUS frequency-set -g performance

for c in $CPUS; do
  for s in /sys/devices/system/cpu/cpu$c/cpuidle/state*/; do
    name=$(cat "$s/name")
    [ "$name" = "POLL" ] && continue   # state0/POLL 保留,其余深度 C-state 关掉
    echo 1 | sudo tee "$s/disable" >/dev/null
  done
done
```

验证:

```bash
cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor   # 应为 performance
cat /sys/devices/system/cpu/cpu$c/cpuidle/state*/disable      # state0 为 0,其余为 1
```

**这两项是运行时设置,重启会恢复默认(governor 常见默认是 `powersave`,C-state 默认全开)。**
目前还没有做成开机自动生效(systemd oneshot service 或 rc.local),需要的话再补,每次重启
之后如果发现 rtt 的 `max` 异常大(几十毫秒级别),先检查这两项是不是被重置了。

### IRQ 亲和

```bash
grep $IFACE /proc/interrupts
```

确认 `<iface>-rx-0` / `<iface>-tx-0` 实际落在哪个 CPU 上,把这些 CPU 也纳入上面 C-state/
governor 的调优范围。busy-poll(`SO_PREFER_BUSY_POLL`)本身不要求 IRQ 亲和核和 cycle 线程
在同一个核上,但两者都要保持 performance + 无深度睡眠,否则中断处理本身会引入抖动。

## AF_XDP nicdrv 环境变量

见 `host/third_party/soem/README.afxdp.md`。默认走 `RMCS_XDP_COPY=0`(尝试 zero-copy),
在已知有问题的环境下用 `RMCS_XDP_COPY=1` 强制走 COPY 模式:

| 变量 | 默认 | 说明 |
|---|---|---|
| `RMCS_XDP_QUEUE` | 0 | 绑定的 RX/TX 队列号 |
| `RMCS_XDP_COPY` | 0 | 设为 1 强制 `XDP_COPY`,跳过 zero-copy 尝试 |
| `RMCS_XDP_NO_BUSYPOLL` | 0 | 设为 1 关闭忙轮询,退化成中断/softirq 驱动 |
| `RMCS_XDP_BUSYPOLL_US` | 20 | `SO_BUSY_POLL` 值(微秒) |
| `RMCS_XDP_BUSYPOLL_BUDGET` | 8 | `SO_BUSY_POLL_BUDGET`(每次 poll 处理包数) |

## 已知问题:igc + realtime 内核下 zero-copy 收不到包

现象:程序日志打印 `ZERO-COPY` 绑定成功(`xsk_socket__create` 没报错),但 SOEM 的
`ec_config_init` 广播发现帧得不到任何应答,抛 `No EtherCAT slave found on the network`。
`RMCS_XDP_COPY=1` 强制走 COPY 模式后可以正常发现从站并稳定运行。

排查过程锁定的证据:

- `ethtool -S $IFACE` 里 `tx_queue_0_packets` 在 zero-copy 模式下正常增长(发送路径没问题),
  但 `rx_queue_0_packets` 全程一动不动(硬件/驱动层计数器,不是应用层逻辑),说明回帧物理上
  没有被 zero-copy 收包环收到。
- 关闭忙轮询(`RMCS_XDP_NO_BUSYPOLL=1`)现象不变,排除 `SCHED_FIFO` 忙轮询和 PREEMPT_RT
  的 NAPI 调度冲突这个猜测。
- `dmesg` 全程无 igc/xdp 报错,是静默的收包缺失。
- `bpftool net show dev $IFACE` 确认 XDP 是 native/driver 模式,不是 generic 模式伪装成功。
- `nicdrv.c`(`host/third_party/soem/nicdrv_afxdp.c`)里 UMEM/ring/promisc 设置顺序走读
  未发现逻辑错误,和 stock SOEM 的 TX/RX 语义一致。

结论:问题在 igc 驱动的 zero-copy 接收路径本身,在应用层代码触及范围之下,和 `nicdrv.c`
无关。可能是这颗网卡固件版本、这个内核版本(尤其 realtime 补丁集)对 igc ZC 支持不完善。

尚未验证、值得一试但成本较高的方向(未做,仅记录):
- 换 Intel 官方 out-of-tree igc 驱动(比发行版内核自带的新)。
- 临时换非 realtime 的同版本主线内核做对照,判断是不是 realtime 补丁集影响了 igc 的 ZC
  代码路径。
- 换一张 AF_XDP zero-copy 支持更成熟的网卡(如 Intel X710/E810、Mellanox ConnectX 系列)。

## 实测对比(仅供参考量级,不同机器会不同)

100Mbit,单从站,64 字节负载,`enp2s0` (igc),已完成上述 CPU 调优:

| 配置 | cycle rate | rtt p50 | rtt p90 | rtt p99 | corrupt 帧 |
|---|---|---|---|---|---|
| 纯 AF_PACKET(不用 XDP) | 21.0 kHz | 373.0 us | 386.1 us | 399.6 us | 2 |
| AF_XDP `RMCS_XDP_COPY=1` | 22.8 kHz | 164.7 us | 175.2 us | 252.9 us | 0 |

结论:不要为了追求 zero-copy 而放弃 AF_XDP 换回纯 socket——COPY 模式已经是明显更优的选项。
