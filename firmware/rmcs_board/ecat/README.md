# RMCS EtherCAT Stream Bridge (HPM6E00EVK, dual-core)

将 librmcs 协议字节流通过 EtherCAT 过程数据转发的从站固件（P1 阶段：回环验证）。
同步模式/协议选型论证与延迟优化路线见 `DESIGN.md`。

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

- **stream-over-PDO**:双向各一个 128 字节 PDO(`[seq:u8][ack:u8][len:u16][payload:124B]`),
  停等 ARQ 补偿 SyncManager 三缓冲的 latest-wins 语义(见
  `core/include/librmcs/ecat/pd_stream.hpp` 头注释,host SOEM transport 与固件共用)。
  CoE 对象字典将其描述为 32 x UNSIGNED32 数组(0x6000 输入 / 0x7010 输出,单条 PDO
  映射项最大 255 bit,故拆分);SM 长度由 SSC 严格校验(必须精确等于 128)。
- **主站模式**:free-run + busy-poll(SOEM,建议 8-16kHz 轮询 + 2-3 帧流水线)。不需要 DC。
- **跨核环**:真 acquire/release 原子(`common/xcore_ring.hpp`),不同于 app/src 单核版的
  signal_fence 方案;依赖 board_init_pmp() 将 SHARE_RAM 配为非缓存 + AMO。

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
     安装 128 字节流对象字典覆盖、重写 SII/EEPROM 镜像):
     ```bash
     firmware/rmcs_board/ecat/tools/import_ssc.sh <生成的 Src 目录>
     # 默认路径 ~/Downloads/ecat_io/SSC/Src
     ```
   - 项目特有内容全部在版本库内(`core0/ssc_overrides/digital_ioObjects.h`、
     `tools/patch_sii.py`),重新生成 SSC 时无需再改 SSC Tool 工程;
   - SII 中 Revision 由脚本抬升(当前为 2),已烧过旧镜像的板卡上电后会自动
     刷新仿真 EEPROM。开发期沿用 HPMicro 示例 Vendor ID;产品化需申请 ETG
     Vendor ID。

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
双核启动、跨核环。吞吐≈124B × 有效轮询率;将 SOEM 轮询间隔与 PD 尺寸做参数
扫描即可得出延迟/吞吐曲线,与 USB 版对比。

host 侧配套工具(SOEM v1.4.0,可选组件;若 SOEM 未装系统目录,先
`cmake -B build && cmake --install build` 装到本地前缀并用 CMAKE_PREFIX_PATH 指过去):

```bash
cmake --preset linux-debug -S host -DLIBRMCS_ENABLE_SOEM=ON \
      -DCMAKE_PREFIX_PATH=$HOME/3rd_party/soem-1.4.0/install
cmake --build host/build
sudo ./host/build/examples/ecat_stream_latency <网口名> [秒数]
# 输出:回环字节校验 + RTT p50/p90/p99 + 吞吐
```

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

## 尚未决定/后续阶段

- **session policy**:OP 重入时环内残留数据的取舍(冲刷需要跨核握手,当前仅
  bump `link_epoch` 通知 core1)——与协议会话层一起在 P2 上板时定。
- P3:按实测决定 SM-synchron(PDI ISR)与中断优先级微调;FoE 固件升级。
