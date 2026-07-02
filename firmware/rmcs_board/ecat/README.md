# RMCS EtherCAT Stream Bridge (HPM6E00EVK, dual-core)

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

- **stream-over-PDO**:双向各一个 128 字节 PDO(`[seq:u8][ack:u8][len:u16][payload:124B]`),
  停等 ARQ 补偿 SyncManager 三缓冲的 latest-wins 语义(见 `common/pd_stream.hpp` 头注释)。
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
   - ETG 会员账号下载 SSC Tool(免费);
   - 以 SDK 例程配置为底 (`$HPM_SDK_BASE/samples/ethercat/ecat_io/SSC/digital_io.xlsx`),
     将 PDO 映射改为两个 128 字节字节数组对象:0x6000(TxPDO 输入)/0x7010(RxPDO 输出),
     从站名建议 `rmcs_stream`;
   - 生成代码放入 `core0/SSC/Src/`;
   - 打 SDK 补丁:`$HPM_SDK_BASE/samples/ethercat/ecat_io/SSC/ssc_pdi_mask.patch`;
   - 同步修改 ESI(EEPROM)中的 PDO 尺寸并按例程文档烧入仿真 EEPROM。
     开发期可沿用 HPMicro 示例 Vendor ID;产品化需申请 ETG Vendor ID。

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

## 尚未决定/后续阶段

- **session policy**:OP 重入时环内残留数据的取舍(冲刷需要跨核握手,当前仅
  bump `link_epoch` 通知 core1)——与协议会话层一起在 P2 定。
- P2:core1 接入 librmcs core 协议 + MCAN 表;host SDK 增加 SOEM transport。
- P3:按实测决定 SM-synchron(PDI ISR)与中断优先级微调;FoE 固件升级。
