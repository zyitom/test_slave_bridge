# IgH EtherCAT Transport —— 实现规格(交给实现者/模型的任务书)

本文档是**待实现**功能的规格说明:`host/src/transport/igh/igh.cpp`,作为
`host/src/transport/soem/soem.cpp`(SOEM 后端)之外的第二个 `Transport` 实现,
使用 IgH EtherCAT Master 的原生 `igc` 驱动。这不是替换 SOEM,是新增一个可选后端。

## 为什么做这个(背景,决策依据见 `EVALUATION.md`)

同一块 rmcs_board 从站、同一台机器上实测的往返延迟对比:

| 传输方案 | cycle rate | rtt p50 | rtt p90 | rtt p99 |
|---|---|---|---|---|
| SOEM + 纯 AF_PACKET | 21.0 kHz | 373.0 us | 386.1 us | 399.6 us |
| SOEM + AF_XDP COPY 模式 | 22.8 kHz | 164.7 us | 175.2 us | 252.9 us |
| **IgH 原生 igc 驱动(独立基准测试,非正式集成)** | 22.1 kHz | **42.9 us** | **44.5 us** | **50.7 us** |

IgH 原生驱动直接接管网卡、把主站状态机放进内核模块,完全绕开 socket/AF_XDP/BPF
这一整层内核网络栈,这是延迟大幅下降的原因,不是简单的参数调优能达到的。

## 前置条件(系统级,已在 TL101 这台机器上完成,换机器要重做一遍)

1. **必须从 git 主干构建**,不能用官方最新 tag(`1.6.9`,发布于 2026-03-26):
   `igc` 驱动的 6.8 内核支持是 2026-05-07 才合并进 `stable-1.6` 分支的,比 1.6.9 还新。
   ```bash
   git clone https://gitlab.com/etherlab.org/ethercat.git
   cd ethercat
   ./bootstrap
   ./configure --enable-igc --with-linux-dir=/usr/src/linux-headers-$(uname -r)
   make -j"$(nproc)" modules
   make -j"$(nproc)"
   sudo make modules_install
   sudo depmod -a $(uname -r)   # modules_install 里的 depmod 常因缺 System.map 静默跳过,必须手动补一次,
                                 # 否则 modprobe 会报 "Module ec_master not found"
   sudo make install
   sudo ldconfig                # 让 /usr/local/lib/libethercat.so 进入动态库缓存
   ```

2. **配置 `/usr/local/etc/ethercat.conf`**:
   ```
   MASTER0_DEVICE="<enp2s0 的 MAC 地址,ip link show enp2s0 查>"
   DEVICE_MODULES="igc"
   ```
   **注意**:`DEVICE_MODULES="igc"` 会接管这台机器上**所有**用 `igc` 驱动的网卡,不只是目标
   网卡(TL101 上 `enp3s0` 也被一起接管了)。部署前确认没有其他 igc 网卡在被使用。

3. **启动/停止**:
   ```bash
   sudo /usr/local/sbin/ethercatctl start   # 接管网卡,enp2s0 从 `ip link` 里消失
   sudo /usr/local/sbin/ethercatctl stop    # 对称回滚,网卡恢复成普通 igc/net_device
   ```
   `/usr/lib/systemd/system/ethercat.service` 已经装好但**默认没有 enable**,开机不会自动接管
   网卡。要不要 `systemctl enable ethercat` 由部署时决定(接管是独占的,机器上跑这套服务期间
   `enp2s0`/`enp3s0` 不能再当普通网卡用)。

4. **权限**:`/dev/EtherCAT0` 是 `root:root 0600`。当前方案和 SOEM 一样直接用 `sudo`
   跑,没有配置 udev 规则开放给普通用户组,如果需要免 sudo 运行,后续可以加一条 udev 规则。

5. 验证前置条件是否就绪:`sudo /usr/local/bin/ethercat slaves -v` 应该能看到从站,
   `Vendor Id: 0x00001A81`,`Product code: 0x00000001`,状态至少是 `PREOP`。

## 从站的 PDO 布局(已从真实从站读出,固定映射,不支持 CoE 动态配置)

来源:`sudo /usr/local/bin/ethercat pdos -p 0`(必须在 `ethercatctl start` 之后跑,针对
真实从站现场核对,不要凭这份文档凭空假设——如果换了从站固件版本或换了板子,重新跑一遍确认)。

```
SM0: 0x1000, 128B, mailbox out (CoE)
SM1: 0x1080, 128B, mailbox in  (CoE)
SM2: 0x1100, 48B, RxPDO 0x1600 "OutputStream"  -- 12 x 32bit entries, 0x7010:01 .. 0x7010:0C
SM3: 0x1400, 48B, TxPDO 0x1a00 "InputStream"   -- 12 x 32bit entries, 0x6000:01 .. 0x6000:0C
```

`ethercat slaves -v` 显示 `Enable PDO Assign: no`、`Enable PDO Configuration: no`——
从站不支持通过 CoE 改 PDO 映射,**只能照抄现有布局,不能自定义**。`Vendor Id: 0x00001A81`,
`Product code: 0x00000001`。

`ecrt_slave_config_pdos()` 用下面这套结构体描述(12 个 subindex 是连续的 32-bit 字,不需要
逐个注册,只注册每个方向的第一个 entry 拿到基址偏移量,后面按 48 字节连续内存处理,和
`soem.cpp` 里 `ec_slave[1].outputs`/`inputs` 指针的用法完全一致):

```c
ec_pdo_entry_info_t out_entries[12];  // {0x7010, i+1, 32} for i in 0..11
ec_pdo_entry_info_t in_entries[12];   // {0x6000, i+1, 32} for i in 0..11
ec_pdo_info_t pdo_out[] = {{0x1600, 12, out_entries}};
ec_pdo_info_t pdo_in[]  = {{0x1a00, 12, in_entries}};
ec_sync_info_t syncs[] = {
    {0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE},
    {1, EC_DIR_INPUT,  0, NULL, EC_WD_DISABLE},
    {2, EC_DIR_OUTPUT, 1, pdo_out, EC_WD_ENABLE},
    {3, EC_DIR_INPUT,  1, pdo_in,  EC_WD_DISABLE},
    {0xff, EC_DIR_INVALID, 0, NULL, EC_WD_DEFAULT},
};
// ecrt_slave_config_reg_pdo_entry(sc, 0x7010, 1, domain, NULL) -> off_out(输出基址偏移)
// ecrt_slave_config_reg_pdo_entry(sc, 0x6000, 1, domain, NULL) -> off_in(输入基址偏移)
// domain_pd + off_out 开始的 48 字节 == outputs_,domain_pd + off_in 开始的 48 字节 == inputs_
```

## 最大的坑:分布式时钟(DC),不做这个从站永远卡在 PREOP/SAFEOP+ERROR

`ethercat slaves -v` 显示这个从站支持 64-bit 分布式时钟,而且被主站自动选为**参考时钟**
(dmesg: `Using slave 0 as DC reference clock`)。ecrt 的从站状态机在有 DC 参考时钟时,要求
应用每个 cycle 在 `ecrt_master_send()` 之前调用:

```c
ecrt_master_application_time(master, now_ns);       // 喂当前时间
ecrt_master_sync_reference_clock_to(master, now_ns); // 同步参考时钟(就是这个从站自己)
ecrt_master_sync_slave_clocks(master);               // 把参考时钟广播给总线上的从站
```

**漏掉这三行,从站会卡在 PREOP,或者到 SAFEOP 后报 `SAFEOP + ERROR`,`ecrt_master_state()`
的 `al_states` 永远到不了 `OP`(bit 3)。** 这是调试基准测试时踩过的坑,dmesg 里的线索是
`EtherCAT WARNING 0: No application time received up to now, but master already active.`。

参考实现:`host/src/transport/igh/reference/igh_latency_bench.cpp`(独立跑通的基准测试,
**不参与编译**,只作为写正式 transport 时的调用顺序参考,里面每一步 ecrt 调用都是跑通过的)。

## 状态转换方式和 SOEM 不一样

SOEM 是显式的:`ec_slave[0].state = EC_STATE_OPERATIONAL; ec_writestate(0); ec_statecheck(...)`。
ecrt 是隐式的:只要应用开始按上面的顺序(带上 DC 三件套)循环调用
`ecrt_master_send()`/`ecrt_master_receive()`,主站内部状态机会自动把从站从 PREOP 推到
SAFEOP 再到 OP。实现里需要一个启动等待循环,轮询 `ecrt_master_state(master, &ms)`,检查
`ms.link_up && ms.slaves_responding && (ms.al_states & (1u << 3))`,超时按 5 秒设,参考基准
测试里的写法。

工作计数器等价物:`ecrt_domain_state(domain, &ds)` 里的 `ds.wc_state == EC_WC_COMPLETE`,
对应 SOEM 里 `wkc >= expected_wkc_` 的判断。

## 要复用 `soem.cpp` 里的这些东西,不要重新发明

- `LockedByteRing`、`CallbackSink`、`SoemBuffer`(改名 `IghBuffer` 之类)这几个和 SOEM API
  完全无关的辅助类,原样搬过来。
- `librmcs::ecat::PdStreamEndpoint` 的用法(`build_own_chunk`/`on_peer_chunk`)、ARQ 语义、
  "OP 重入时 `endpoint_.reset()`" 这条规则,和 SOEM 版本保持一致。
- `cycle_loop()` 的整体结构(响应窗口 `kResponseWindow`、wkc 错误计数/恢复阈值、统计日志
  节奏)照抄,只替换掉"发送/接收/拿 wkc"这几行为 ecrt 调用。
- `Transport` 虚接口(`host/src/transport/transport.hpp`)签名不变,构造函数签名参考
  `soem::create_transport(interface_name, options)`,但 IgH 场景下"interface_name"其实只是
  用来解析 MAC 地址或者干脆走 `ethercat.conf` 里已经配好的 master 0——需要决定这个参数怎么
  传(建议:忽略 interface_name 或用它做一次一致性校验,因为 IgH 的目标设备是在
  `ethercat.conf` / `ecrt_request_master(0)` 层面决定的,不是像 SOEM 那样在 `ec_init(ifname)`
  时才绑定)。

## 析构 / 清理

`ecrt_master_deactivate(master)` + `ecrt_release_master(master)`,对应 SOEM 析构里的
`ec_slave[0].state = EC_STATE_INIT; ec_writestate(0); ec_close();`。确保进程异常退出路径
(构造失败的 `close_on_failure` 模式,`soem.cpp:141` 那种 `FinalAction`)也覆盖到,不要在
构造失败时把 master 泄漏在已请求状态。

## 构建系统集成

参考 `host/CMakeLists.txt` 里 `LIBRMCS_ENABLE_SOEM` 那一段(约第 100-130 行),新增:

```cmake
option(LIBRMCS_ENABLE_IGH "Build the EtherCAT (IgH) transport" OFF)
if(LIBRMCS_ENABLE_IGH)
    find_package(ethercat CONFIG QUIET)   # /usr/local/lib/cmake/ethercat/ethercat-config.cmake
    if(ethercat_FOUND)
        target_link_libraries(${PROJECT_NAME} PUBLIC ethercat)
    else()
        find_path(ETHERCAT_INCLUDE_DIR ecrt.h REQUIRED)
        find_library(ETHERCAT_LIBRARY ethercat REQUIRED)
        target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${ETHERCAT_INCLUDE_DIR})
        target_link_libraries(${PROJECT_NAME} PUBLIC ${ETHERCAT_LIBRARY})
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE LIBRMCS_ENABLE_IGH=1)
    message(STATUS "EtherCAT (IgH) transport enabled")
endif()
```

`igh.cpp` 顶部用 `#if defined(LIBRMCS_ENABLE_IGH)` 包住全部内容,和 `soem.cpp` 的
`#if defined(LIBRMCS_ENABLE_SOEM)` 模式一致,未启用时编译成空翻译单元。

SOEM 和 IgH 两个选项应该能同时开启(不同的 `namespace`,不同的 `create_transport`),互不
干扰,由调用方(examples)决定连哪一个。

## 验证清单

1. `sudo /usr/local/sbin/ethercatctl start`,`sudo /usr/local/bin/ethercat slaves` 确认从站在线。
2. `cmake --preset linux-debug -S host -DLIBRMCS_ENABLE_IGH=ON && cmake --build host/build`。
3. 跑一个和 `ecat_stream_latency.cpp` 等价的例子(或者直接改造它,加一个后端选择参数),
   对照 `EVALUATION.md` 里的基准数字:期望 p50 在几十微秒量级,明显低于 SOEM+AF_XDP-COPY 的
   164.7us。正式 transport 因为多了 `PdStreamEndpoint`/ARQ 的开销,比裸 ecrt 基准测试慢一些
   是正常的,但不应该慢到接近甚至超过 SOEM 的水平——如果出现这种情况,先怀疑 cycle_loop 里
   是不是引入了不必要的锁等待或者忘了带上 DC 三件套调用。
4. 用现有的 corrupt/wkc 错误计数机制确认 ARQ 在这个新后端下依然可靠(0 corrupt 帧)。

## 明确不在这次范围内(不要顺手做)

- 冗余(redundant NIC)支持——SOEM 版本也没做,保持范围一致。
- udev 规则 / 免 sudo 运行。
- `systemctl enable ethercat` 开机自启——部署时再决定,不要在代码里假设服务已经 enable。
- 把 SOEM 后端删掉或改成默认——IgH 是新增的可选项,`LIBRMCS_ENABLE_SOEM` 现有行为不能变。
