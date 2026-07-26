# RMCS EtherCAT 转发桥（HPM6E00EVK，双核）

> **文档类型**：背景说明 + 操作指引
> **适用范围**：`firmware/rmcs_board/ecat/`，HPM6E8Y 上的 EtherCAT 桥固件
> **状态**：现行有效
> **相关文档**：[DESIGN.md](DESIGN.md)（选型论证） · [LINKX_HW_ACCEL_PLAN.md](LINKX_HW_ACCEL_PLAN.md)（硬件加速规划） · [../AGENTS.md](../AGENTS.md) · [../BUILD_ENVIRONMENT.md](../BUILD_ENVIRONMENT.md)

## 摘要

本文件讲 EtherCAT 桥固件的**架构、构建、验证与烧录**：双核怎么分工、过程数据怎么承载
librmcs 字节流、怎么跑 P1 回环验证、怎么烧到板子上。

**为什么这么设计**（同步模式选型、应用层协议选型、延迟优化路线）在
[DESIGN.md](DESIGN.md)；**下一步准备怎么用硬件加速**在
[LINKX_HW_ACCEL_PLAN.md](LINKX_HW_ACCEL_PLAN.md)。

将 librmcs 协议字节流通过 EtherCAT 过程数据转发的从站固件（P1 阶段：回环验证）。

## 架构

```
CPU0 (EtherCAT 域)                     CPU1 (现场总线域)
+---------------------------+          +---------------------------+
| Beckhoff SSC (MainLoop)   |          | P1: 无损回环 (main.cpp)    |
| ESC PDI / SM 事件          |          | P2+: MCAN/UART + 协议栈    |
| pd_stream 停等 ARQ         |          |                           |
+------------+--------------+          +------------+--------------+
             |      SHARE_RAM (16K, 非缓存+AMO)      |
             |  down ring (4K)  ------------------>  |
             |  <------------------   up ring (8K)   |
```

- **stream-over-PDO**:双向各一个 48 字节 PDO(`[seq:u8][ack:u8][len:u16][payload:44B]`),
  停等 ARQ 补偿 SyncManager 三缓冲的 latest-wins 语义(见
  `core/include/librmcs/ecat/pd_stream.hpp` 头注释,host SOEM transport 与固件共用)。
  CoE 对象字典将其描述为 12 x UNSIGNED32 数组(0x6000 输入 / 0x7010 输出,单条 PDO
  映射项最大 255 bit,故拆分);SM 长度由 SSC 严格校验(必须精确等于 48)。
- **主站模式**:free-run + busy-poll(SOEM,建议 8-16kHz 轮询 + 2-3 帧流水线)。不需要 DC。
- **跨核环**:真 acquire/release 原子(`common/xcore_ring.hpp`),不同于 app/src 单核版的
  signal_fence 方案;依赖 board_init_pmp() 将 SHARE_RAM 配为非缓存 + AMO。
- **热路径在 ILM**:core0 用定制链接脚本(`core0/hpm6e80_flash_uf2_ilm_hot.ld`)
  把 EtherCAT 数据面(PDI ISR、SSC 周期层、ESC 访问、PD glue、memcpy/memset)
  放进 ILM 执行(启动时从 flash 拷贝),消除 XIP 取指 cache miss 造成的周转
  抖动;CoE/mailbox/SDO 冷路径留在 flash。SDK 迁移后需与原版 flash_uf2.ld 对比。
- **从站同步模式**:主站不写 0x1C32/0x1C33 且不开 DC 时,SSC 自动进入
  SM-synchron(`ecatslv.c` StartInputHandler):SM2 写事件触发 PDI 中断,
  出站消费 + ack + 上行重建全部在 ISR 内完成——无需主站任何配置。
- **上行即时刷新(跨核事件驱动)**:SM-synchron 只在 SM2 ISR 里映射输入,而
  core1 的回复总是晚于该 ISR 几微秒才就绪,固定错过一班轮询。为把"回复就绪
  到写进 ESC"的周转从 SSC 主循环粒度(混有 mailbox/CoE 慢路径,几十 us 抖动)
  压到中断延迟,core1 每次向上行环 push 成功后即敲 `HPM_MBX0B`
  (`core1/src/main.cpp` uplink_doorbell_ring,push 与敲钟之间加 full `fence`
  保证非缓存环写先于设备写可见);core0 收到 `HPM_MBX0A` 中断
  (`ecat_appl.c` rmcs_uplink_doorbell_isr)立即映射上行——回复几微秒内进
  ESC,主站下一帧即可带走。MBX 优先级(1)严格低于 ESC PDI(4),敲钟绝不
  抢占在途 EtherCAT 帧;`rmcs_input_refresh` 内 `DISABLE_ESC_INT` 反向挡住
  PDI ISR,两处 `PDO_InputMapping` 不交错;三缓冲 SM 保证原子交换。
  `pAPPL_MainLoop` 钩子(rmcs_input_refresh_mainloop,敲钟中断被屏蔽以防抢占)
  保留为兜底,覆盖"ack 刚放开 ARQ 但无新敲钟"这类无在途事件可复触发的边角。
  该 ISR 与刷新路径经 core0 定制链接脚本落在 ILM,免 XIP 取指抖动。host 侧
  soem transport 对称地在交付回调后留 ~3us 响应窗口,再省一个周期。

## 周期控制推荐模式:Hybrid fixed PDO + reliable stream

28 个电机(4 路 CAN,每路 7 个)以 1kHz 周期更新时,推荐构建 `RMCS_ECAT_HYBRID_PD`
固件并使用 IgH host 的 `RMCS_ECAT_MODE=hybrid`。每个方向的 PDO 是 352 字节:

- 336 字节固定区:4 x 7 个 12 字节 CAN mailbox,每槽独立 `seq`,语义为
  latest-wins。一个控制 tick 是完整快照;主站尚未采样时,新快照直接
  替换旧快照,不产生 ARQ 队列深度抖动。
- 16 字节可靠区:`[seq:u8][ack:u8][len:u16][payload:12B]`,继续承载 session、配置、
  UART、扩展帧、RTR 以及非周期 CAN 流量。当前 librmcs/CAN 硬件路径整体只支持
  0..8 字节 payload,不接受 CAN-FD DLC > 8。
- SDK 原有 `start_transmit()` 始终走可靠 stream,行为不变。周期控制显式使用
  `start_cyclic_transmit()`,每个 tick 最多创建一个 builder。该 builder 是严格批次:
  每路最多 7 个 standard、non-RTR、0..8 字节 CAN 帧;超额、不兼容或非 CAN 字段
  返回失败并使整个 fixed 快照失效,不会自动混入 stream。配置和事件流量
  必须使用单独的 `start_transmit()`。

这种分工同时保留固定 PDO 的常数管线延迟和配置通道可靠性。它消除的是
传输排队抖动,不是 host 调用相位:异步 `start_cyclic_transmit()` 仍可能在任意
`0..Tpdo` 相位被周期线程采样。硬实时控制应把控制 tick 与 EtherCAT 周期锁相;
`host/src/transport/igh/reference/ecat_hybrid_cyclic_bench.cpp` 在同一 IgH 周期线程内
发送并采样全部 28 槽,用于测量真实闭环分布。多从站要求共同执行时刻时
再启用 DC SYNC0;单桥回环不需要 DC。

```bash
HYBRID=1 BUILD_ONLY=1 ./flash-ecat.sh

cmake --preset linux-release -S host -DLIBRMCS_ENABLE_IGH=ON -DBUILD_EXAMPLES=ON
cmake --build host/build
RMCS_ECAT_BACKEND=igh RMCS_ECAT_MODE=hybrid sudo -E \
    ./host/build/examples/ecat_board_test ignored
```

352 字节 PDO 在 100Mbit EtherCAT 上增加固定序列化时间,但换来一个周期覆盖全部
28 个命令且无消息排队。最终端到端上界仍受 CAN 位速率、仲裁和总线负载
约束;固定 PDO 不能消除 CAN 段的排队,因此必须用上述 28 槽基准在目标接线
和总线负载下验证 tick 完成时间与尾部抖动。

固定上行槽不携带 MCAN 的硬件 SOF timestamp,回调中的时间应视为主机收到
PDO 的时间。扩展帧/RTR/非周期 CAN 经独立 stream 发送时,与 fixed 批次之间
不承诺跨路径顺序;控制 tick 不应混用两条路径。

`RMCS_ECAT_NATIVE_CAN` 仅用于隔离的延迟实验,不应部署到生产总线。它沿用
stock 48 字节 SII 身份,但把相同字节解释成 CAN mailbox;stock stream 主站
可能进入 OP 后发送语义不兼容的数据。生产周期控制只使用独立 352 字节
映射的 hybrid 模式。

## USB 协处传输(同固件双接口,数据面互斥)

core0 除了 ESC 还挂着一个 USB 设备(原本只做 DFU 刷机)。因为跨核环
(`down`/`up`)承载的是**与传输无关的原始协议字节流**,给这个 USB 设备再加一个
vendor bulk 数据接口、直接对接同一对环,就得到"一个固件、两种传输"——host 用
EtherCAT 或 USB 都能跟 core1 的 CAN/UART 说话,**但两个 host 数据客户端不能并发**。
代码全在 core0,core1 一行不动。

```
              core0
  EtherCAT master --(ESC PDO, ARQ)--\
                                      >--[仲裁]--> down/up ring <--> core1 (CAN/UART + 协议栈)
  USB host --(vendor bulk EP1)-------/
```

- **USB 不需要 ARQ**:bulk 本身可靠有序,所以 USB 路径只是把 bulk 字节和环对拷。
  USB ISR 运行 `tud_task()` 保障枚举/DFU;vendor 数据 pump 在 core0 主循环运行,并在
  访问 tinyusb FIFO 时屏蔽 USB IRQ,确保只有一个数据 pump 上下文。
- **仲裁(谁占用用谁,不是同时使用)**:同一对环同一时刻只由一种传输驱动。仅插 USB
  线或仅枚举不会抢占数据面;host 在 vendor OUT 上
  发数据 → `tud_vendor_rx_cb` 置 USB active,ESC 的 PDO 钩子随即变惰性(忽略输出、
  输入发空闲 chunk);USB 拔出(`tud_umount_cb`)或 EtherCAT 进入 OP
  (`rmcs_pd_reset`)则交还给 EtherCAT。Hybrid 固定区在 USB ownership 期间持续
  消费但丢弃旧命令,切回前排空旧反馈并清锁存图;可靠区 reset ARQ 端点并
  bump link epoch,core1 据此重启会话。切换前应先退出旧 host 程序:若 USB 和
  IgH 数据程序并发,
  USB keepalive 会反复发送 vendor OUT 并重新取得 ownership,EtherCAT 会看到空闲 PDO。
  见 `src/rmcs_pd.h` 的 `rmcs_pd_set_usb_active` 等。
- **复合 USB 描述符**:vendor(接口 0,bulk EP `0x01`/`0x81`,HS 512B)+ DFU-RT
  (接口 1)。接口 0 = vendor 与 host `transport::usb`(认领接口 0)及独立 USB app
  的布局一致;**DFU-RT 保留**,所以任何镜像都能通过 DFU 刷回,不会变砖。
- host 侧:`host/include/librmcs/board/rmcs_board_hpm6e8y.hpp`(USB,`a11c:a904`,
  4 路 CAN)+ 例程 `host/examples/usb_canfd_stress.cpp`,与 `ecat_canfd_stress`
  同校验,可直接对比 USB vs EtherCAT。
- **状态**:已上板验证 USB→IgH→USB 和 IgH→USB→IgH 切换。正常协议 CAN-FD
  回环中,USB 与 IgH 各 50000 次 queue-free RTT 均为 50000/50000,无超时/损坏;
  IgH 为 0 WKC error。DFU runtime 和正常 USB session 也已验证。

## 构建前提

1. RISC-V 工具链 + SDK 环境变量:
   ```bash
   export GNURISCV_TOOLCHAIN_PATH=<toolchain-root>
   export HPM_SDK_BASE=$(pwd)/firmware/rmcs_board/bsp/hpm_sdk
   ```
2. **生成 Beckhoff SSC 代码**(许可限制,不入库):
   - ETG 会员账号下载 SSC Tool(免费),按 SDK 例程文档
     (`$HPM_SDK_BASE/samples/ethercat/ecat_io/README_zh.rst`)用 **原样的例程配置**
     (`SSC/ECAT_IO.esp`,无需修改 PDO/对象字典)生成从站代码;
   - 运行导入脚本完成全部项目适配(复制/CRLF 归一化、打 SDK PDI 补丁、
     安装 48 字节流对象字典覆盖、重写 SII/EEPROM 镜像):
     ```bash
     firmware/rmcs_board/ecat/tools/import_ssc.sh <生成的 Src 目录>
     # 默认路径 ~/Downloads/ecat_io/SSC/Src
     ```
   - 项目特有内容全部在版本库内(`core0/ssc_overrides/digital_ioObjects.h`、
     `tools/patch_sii.py`),重新生成 SSC 时无需再改 SSC Tool 工程;
   - SII 中 stream Revision 为 3,hybrid Revision 为 5;从旧 stream 升级到 hybrid
     时会自动刷新仿真 EEPROM。从 hybrid 回退到 stock/native 时,较低的 revision
     不会自动覆盖已存的 revision 5,必须先提高 stock revision 并重新生成 SII。
     开发期沿用 HPMicro 示例 Vendor ID;产品化需申请 ETG Vendor ID。

## 构建(先 core1 后 core0)

```bash
cmake -G Ninja -S firmware/rmcs_board/ecat/core1 -B build-ecat-core1 \
      -DBOARD=hpm6e00evk -DCMAKE_BUILD_TYPE=debug
cmake --build build-ecat-core1     # 生成 core0/src/sec_core_img.c

cmake -G Ninja -S firmware/rmcs_board/ecat/core0 -B build-ecat-core0 \
      -DBOARD=hpm6e00evk -DCMAKE_BUILD_TYPE=debug
cmake --build build-ecat-core0     # 产出可烧录的 core0 镜像(内嵌 core1)
```

## P1 验证方法

core1 是无损回环:主站(SOEM busy-poll)通过 RxPDO 发送任意字节流,应从 TxPDO
按序、按字节精确地收回。验证覆盖:ESC 收发、SM 事件路径、ARQ 重传/背压、
双核启动、跨核环。吞吐≈44B × 有效轮询率;将 SOEM 轮询间隔与 PD 尺寸做参数
扫描即可得出延迟/吞吐曲线,与 USB 版对比。

host 侧配套工具(SOEM v1.4.0,可选组件;若 SOEM 未装系统目录,先
`cmake -B build && cmake --install build` 装到本地前缀并用 CMAKE_PREFIX_PATH 指过去):

```bash
cmake --preset linux-release -S host -DLIBRMCS_ENABLE_SOEM=ON -DBUILD_EXAMPLES=ON \
      -DCMAKE_PREFIX_PATH=$HOME/3rd_party/soem-1.4.0/install
cmake --build host/build
sudo ./host/build/examples/ecat_stream_latency <网口名> [秒数] [绑定核] [在途帧数]
# 输出:回环字节校验 + RTT p50/p90/p99 + 吞吐;transport 每 5s 打印实际轮询率
```

测延迟前的检查单(帧 RTT ≈ 2-3 个轮询周期,轮询率低 = 延迟差的直接原因):

1. 固件与 host 都必须是 release 构建(-O0 的 PDI ISR 慢数倍);
2. 看 transport 打印的 cycle rate:直连 i226 应达 10-20kHz;若只有 1-2kHz,
   查 NIC 中断合并(`ethtool -C <if> rx-usecs 0 tx-usecs 0`)、EEE
   (`ethtool --set-eee <if> off`)、CPU governor(performance)、
   是否 USB 网卡(USB 适配器按 USB 微帧批处理,天生毫秒级,不可用);
3. 绑定核参数 + `isolcpus` 隔离核压尾延迟;在途帧数用默认 1 才是纯 RTT,
   调大测的是停等 ARQ 上的排队吞吐。

transport 实现:`host/src/transport/soem/soem.cpp`(实现 `transport::Transport`
接口,独立 busy-poll 线程,与固件共用 `librmcs::ecat::PdStreamEndpoint`);
上位机侧建议用 `options.thread_setup` 绑定隔离核并设 SCHED_FIFO。

## 烧录(hpm6e80ivm1)

双核芯片不需要特殊烧录:flash 里只有 core0 一个镜像,core1 程序以 C 数组内嵌
其中,由 core0 上电拷入 core1 ILM 后释放。仓库根目录:

```bash
./flash-ecat-bootloader.sh   # 一次性:OpenOCD + 板载 FT2232 烧 DFU bootloader
./flash-ecat.sh              # 日常:USB DFU 烧应用(协议镜像)
LOOPBACK=1 ./flash-ecat.sh   # P1 回环镜像(配 ecat_stream_latency)
HYBRID=1 ./flash-ecat.sh     # 28 槽周期 CAN + 可靠配置流
```

## P2 全协议栈(已实现,待上板)

固件侧:core1 默认镜像即 librmcs 协议应用(MCAN4 + UART1 + RGB LED,会话
握手与 USB 版字节一致)。host 侧:`protocol::Handler` 已有 EtherCAT 构造
入口(传网口名),板卡类 `librmcs/board/rmcs_board_ecat_bridge.hpp`
(CAN0/UART0,API 形态与 USB 板一致):

```bash
sudo ./host/build/examples/ecat_board_test <网口名> [秒数]
# 会话建立 + 周期发送 CAN0/UART0;PY06<->PY07 短接可见 UART 回显
```

## 后续阶段

- 在目标 4 路 CAN 接线和实际总线利用率下运行 28 槽锁相基准,据实测决定
  SM-synchron(PDI ISR)、中断优先级和控制 tick 相位。
- 多从站同步控制验证 DC SYNC0;FoE 固件升级。
