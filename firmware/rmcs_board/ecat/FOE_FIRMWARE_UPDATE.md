# FoE 固件升级：设计、实施与实测

> **文档类型**：背景说明 + 过程记录
> **适用范围**：`firmware/rmcs_board/`，HPM6E8Y 核对调布局下经 EtherCAT FoE 升级固件
> **状态**：现行有效（下载与安装两条路径已上板验证；读回未通，见第 7 节）
> **相关文档**：[README.md](README.md)（EtherCAT 桥架构与烧录） · [CORE_SWAP_MIGRATION.md](CORE_SWAP_MIGRATION.md)（为什么 EtherCAT 在 core1） · [DESIGN.md](DESIGN.md)（传输选型） · [../../../ENV.md](../../../ENV.md)（SEGGER 工具安装）

## 摘要

本文件记录 **2026-08-14** 这一轮工作：给 HPM6E8Y 的核对调布局加上**经 EtherCAT FoE
升级固件**的能力，包括为什么这么切分、实际写了什么、实测数字，以及**走错的路**。

读完能做到：用 IgH 主站把一份固件推进板子并让 bootloader 装上；知道哪些参数不能填错；
知道哪些方向已经被实测否掉、不必重走。

**为什么 EtherCAT 在 core1、USB 在 core0** 不在这里，在
[CORE_SWAP_MIGRATION.md](CORE_SWAP_MIGRATION.md)；本文只在第 1 节说明这个既有布局
如何决定了 FoE 的切分。

## 本文导航

| 章节 | 内容 |
|---|---|
| [1. 布局决定的切分](#1-布局决定的切分) | FoE 为什么天生跨两个核，分界线在哪 |
| [2. flash 布局](#2-flash-布局) | 暂存区放哪、为什么放那 |
| [3. 崩溃安全契约](#3-崩溃安全契约) | 写入顺序，以及每一步中断会发生什么 |
| [4. 代码落点](#4-代码落点) | 新增/修改了哪些文件 |
| [5. SSC 重新生成](#5-ssc-重新生成) | 开 FoE 需要改的 4 个配置项与操作步骤 |
| [6. 实测结果](#6-实测结果) | 时序数字、两条路径的上板验证 |
| [7. 被实测推翻的判断](#7-被实测推翻的判断) | 走错的路，勿重走 |
| [8. 遗留与后续](#8-遗留与后续) | 读回失败、JTAG 疑点、其余可加的特性 |

---

## 1. 布局决定的切分

FoE **不是一个"放哪个核"的选择题**，它天生被劈成三段，而分界线是既有布局定死的：

```
主站 --FoE--> core1 SSC --MBX1 flash RPC--> core0 写暂存区
                                                  ↓ 提交
                                          冷复位 → bootloader 安装
```

| 阶段 | 落在哪 | 为什么没有选择余地 |
|---|---|---|
| FoE 状态机、BOOT 态门禁、收分片 | **core1** | SSC 在 core1，mailbox 路径已经在那儿 |
| 写 flash | **core0** | core1 是纯 RAM 镜像，NOR 归 core0（XPI0），`ecat_flash.h` 的 RPC 已存在 |
| 装进 app 槽 | **bootloader** | core0 正从 app 槽 XIP 取指，**擦掉它等于抽掉自己脚下的地板** |

第三条是整个设计的关键。它把问题从"EtherCAT 要不要能写 flash"变成
"**EtherCAT 只需要把字节推到暂存区**"——而推字节这件事 RPC 早就在做了（仿真 SII
EEPROM 的擦写走的就是它）。

> **推论**：所以不需要为 FoE 把 EtherCAT 搬回 core0。搬回去反而更糟——擦除时 core0
> 关中断几十毫秒，EtherCAT 若也在 core0，从站在这期间不应答主站，而 FoE 恰恰要求
> 从站全程在线。详见 [CORE_SWAP_MIGRATION.md](CORE_SWAP_MIGRATION.md)。

## 2. flash 布局

```
0x80000000  bootloader              124 KB     实际用 93672 B（73.8%）
0x8001F000  app metadata              4 KB
0x80020000  app 槽                 1.875 MB    实际用约 194 KB
0x80200000  仿真 SII EEPROM          64 KB
0x80210000  暂存 metadata             4 KB     ← 新增
0x80211000  暂存镜像               约 1.93 MB  ← 新增
0x80400000  flash 尾（4 MB）
```

**放在 2 MB 以上那块空地，而不是从 app 槽里对半分**，理由是**一个现有边界都不用动**：
`BOARD_APP_FLASH_END_OFFSET`、app 的链接脚本、bootloader 接受的镜像范围全部不变。
app 槽里对半分则要同时改三处，而且 app 的 `_flash_size` 本来就和
`BOARD_APP_FLASH_END_OFFSET` 不自洽（链接脚本按 4 MB 算，只是靠 app 小才没撞上），
不值得再往那个坑里加东西。

接受的镜像上限取 **app 槽容量**（1966080 B）而不是暂存区容量：暂存区更大，
收一个装得进暂存却装不进 app 槽的镜像，只会把失败推迟到 app 槽已经擦掉之后。
`common/foe_staging.hpp` 里算出这个上限，`bootloader/src/flash/layout.hpp` 用
`static_assert` 交叉验证两条推导一致。

## 3. 崩溃安全契约

暂存记录 16 字节，靠**最后写 `state` 字**作为提交屏障：

```
magic  ← begin() 时写，标记"正在写入"
state  ← 最后写，写进去之前整个暂存区读作"不存在"   ★屏障
image_size
reserved
```

每一个中断点都落在"忽略"或"重做"上：

| 掉电时刻 | 结果 |
|---|---|
| 写镜像途中 | `state` 从未写入 → 忽略 |
| `image_size` 与 `state` 之间 | `state` 从未写入 → 忽略 |
| 提交之后、安装之前 | Ready → **下次启动安装** |
| **安装途中** | 暂存记录仍在 → **下次启动重装** |

最后一行成立的前提是：**暂存记录只在 app metadata 提交之后才清除**。擦掉 app 槽到
metadata 提交之间，app 槽是半写的，**暂存记录是唯一能重建它的东西**。提前清就是这条
路唯一的变砖方式——`bootloader/src/flash/staging.hpp` 把这一点写在注释里，不要改动顺序。

`Writer` 会跳过内容已相同的扇区，所以重装是廉价的，不是全量重写。

## 4. 代码落点

**新增**

| 文件 | 职责 |
|---|---|
| `common/foe_staging.hpp` | 暂存记录格式、地址、上限。**app 与 bootloader 共用**，所以放 `common/` |
| `common/foe_staging_writer.hpp` | 写入时序。flash 原语注入，让 FoE（走 RPC）和将来的 USB 自测（走本地）**共用同一份顺序**——要一致的不是传输而是顺序 |
| `bootloader/src/flash/staging.hpp` | 安装：校验暂存 → app metadata begin → 拷贝 → finish → 再校验 → 最后清暂存 |
| `ecat/core1_ecat/src/ecat_foe_support.{h,cpp}` | 四个 SSC 回调的实现 |
| `ecat/tools/HPM_ECAT_RMCS_Config.xml` | SSC Tool 配置（见第 5 节） |

**修改**

| 文件 | 改动 |
|---|---|
| `boards/hpm6e8y/board.h` | 暂存区地址宏 |
| `app/src/xcore/flash_server.cpp` | 单窗口 → 窗口表。**只加窗口不拆检查**：core1 拿的是主站数据，绝不能有权指定任意地址 |
| `bootloader/src/flash/validation.hpp` | `validate_image_at(addr,size,max)` 泛化，暂存镜像走**同一套**校验 |
| `bootloader/src/main.cpp` | 跳转判定**之前**调用安装 |
| `ecat/core1_ecat/CMakeLists.txt` | 自动检测 `foeappl.c` 是否存在 → `RMCS_SSC_HAS_FOE` |
| `ecat/core1_ecat/src/ecat_appl.c` | `APPL_StopMailboxHandler` 里触发复位 |
| `ecat/core1_ecat/src/ecat_main.c` | 主循环轮询 commit + 复位 |
| `ecat/tools/patch_sii.py` | `REVISION` 7 → 8 |

**没有用 SDK 的 `hpm_ecat_foe.c`**，两个独立理由：

1. 它硬编码要求密码 `0x87654321`。**IgH 的 `ethercat foe_write` 没有密码选项**
   （`-p` 是 `--position`），恒发 0，于是 IgH 主站的每一次下载都会被拒。那份胶水是
   照 TwinCAT 写的。
2. 它内联编程 NOR，core1 做不到。

自己实现四个回调约 120 行，同时接受密码 0 和 `0x87654321`。**密码不是安全边界**——
明文在总线上，只当打错字的护栏；真正的边界是 bootloader 的 SHA-256。

**构建自动检测**：`RMCS_SSC_HAS_FOE` 由 `foeappl.c` 是否存在决定，所以**一份源码同时
适配有 FoE 和无 FoE 两种 SSC**，重新生成之后不用改任何构建文件。

## 5. SSC 重新生成

开 FoE 需要用 SSC Tool 重新生成从站代码（许可限制，不入库）。配置文件已备好：
`ecat/tools/HPM_ECAT_RMCS_Config.xml`，它是从 SDK 的 `HPM_ECAT_IO_Config.xml` 脚本
生成的，只改 4 处，逐项对齐 SDK 自己的 `HPM_ECAT_FOE_Config.xml`：

| 设置 | 原 | 新 |
|---|---|---|
| `FOE_SUPPORTED` | 0 | **1** |
| `BOOTSTRAPMODE_SUPPORTED` | 0 | **1** |
| `MAILBOX_SUPPORTED` | （无） | **1** |
| `MAX_MBX_SIZE` | （无，默认 0x80） | **0x10C** |

**故意没动的**（动了会毁掉现有 stream PDO）：`APPLICATION_FILE` 保持 `digital_io.h`
——FoE 例程把它改成 `foe.h` 是因为那是**它自己的应用**；FoE 的协议模块是另外三个文件。
`PRODUCT_CODE`、`MAX_PD_INPUT/OUTPUT_SIZE` 同样保持。

步骤：

```
1. SSC Tool → Tool → Options → Configurations → 添加
     firmware/rmcs_board/ecat/tools/HPM_ECAT_RMCS_Config.xml
2. File → New → Custom → 选 "RMCS EtherCAT bridge (IO + FoE bootstrap) <HPMicro>"
3. Tool → Application → Import → bsp/hpm_sdk/samples/ethercat/ecat_io/SSC/digital_io.xlsx
     ★ 是 IO 那个，不是 foe.xlsx：我们的对象字典由 import_ssc.sh 在生成之后覆盖
4. Project → Create new Slave Files → 输出目录 → Start
5. firmware/rmcs_board/ecat/tools/import_ssc.sh <生成的 Src 目录>
```

**判据**：生成目录的 `Src/` 下必须出现 `bootmode.c`、`ecatfoe.c`、`foeappl.c` 三个文件。
**这三个在不在，是判断配置有没有生效的唯一可靠依据**，比在界面上翻设置可靠。

**SII revision 必须提**（本次 7 → 8）。仿真 EEPROM 只在"内置 revision > 已存"时才刷新，
停在 7 的话主站会按旧 SII 枚举：mailbox 仍按 128 字节、而且**根本不提供 BOOT 选项**。
实测刷新后主站看到的变化：

```
0x018:  07 → 08     SII revision
0x028:  全 0 → 填充  bootstrap mailbox：recv @0x1000/0x80, send @0x1080/0x80
0x038:  04 → 0C     mailbox 协议掩码：bit2(CoE) + bit3(FoE)
```

## 6. 实测结果

### 6.1 载荷格式（填错就白测）

FoE 要送的**既不是 `.bin` 也不是 `.dfu`**：

```
193912 B   *.bin      ← 没有哈希后缀
193948 B   FoE 载荷    ← .bin + 36 B（"HASH" 魔数 0x48415348 + SHA-256）  ★送这个
193964 B   *.dfu      ← 再 + 16 B DFU 后缀
```

```bash
python3 -c "d=open('....dfu','rb').read(); open('fw.img','wb').write(d[:-16])"
sudo ethercat foe_write -o app fw.img
```

DFU 烧录用 `.dfu` 是因为 `dfu-util` 自己剥掉那 16 字节——**两条路最终写进 app 槽的
字节完全一样**。`[实测]`

### 6.2 时序

| 量 | 值 |
|---|---|
| 单次扇区擦除 | **22–27 ms** |
| FoE 块大小 | **116 B** = mailbox 128 − MBX 头 6 − FoE 头 6 |
| 擦除节奏 | 每约 35 块一次（4096/116） |
| 20000 B 下载 | 173 块，5 次擦除，擦除合计 127 ms，**1.39 s** |
| 60000 B 下载 | **3.82 s** |
| 193948 B 真镜像 | **11.7 s** |

`[实测 2026-08-14]`

### 6.3 两条路径的上板验证

**测试 A —— 非法镜像必须被拒（fail-closed）**

送 20000 B 随机数据 → `rc=0`（板子收下并暂存）→ 退出 BOOT → 冷复位
（USB device 066 → 067）→ **复位后仍是 `RMCS Agent`，不是 DFU Bootloader**。

bootloader 校验失败（UF2 签名与 SHA-256 都不对）→ 丢弃暂存记录 → 照常启动现有固件。
**坏镜像不会毁掉能用的 app。**

**测试 B —— 真镜像必须被安装**

送 193948 B 真镜像 → `rc=0`，11.7 s → 退出 BOOT → 冷复位（067 → 068）。

判据用 `DIAG_USB` 当标记：板上原本是**带**诊断的镜像（core1 日志经 USB 送出），
FoE 送的是**不带**诊断的。结果 **core1 日志 13 行 → 0 行**，只可能是新镜像真的进了
app 槽并启动。复位后 EtherCAT 从站正常回到 PREOP。

**主机侧逻辑测试**：`StagingWriter` 的写入顺序、先擦后写、提交屏障、非顺序 offset 拒绝、
超尺寸拒绝、空提交拒绝——**29 项全通过**（用带 NOR 语义的假 flash：擦→0xFF、写→按位与，
所以漏擦会表现为数据损坏而不是静默通过）。

### 6.4 使用注意

- **必须确认真的进了 BOOT 再发**。`ethercat states BOOT` 之后主站要几秒才转移完，
  等 2 秒就发会得到 `rc=1`。`[实测]`
- **复位发生在主站退出 BOOT 时**，不是写完最后一块时。见第 7 节第 1 条。

## 7. 被实测推翻的判断

这一轮走错了四次。**每一条都指向过一个方向的工作，记在这里是为了不必再花一遍代价。**

| # | 曾经的判断 | 实测结果 | 教训 |
|---|---|---|---|
| 1 | 提交后立刻置复位标志即可 | **错。** 板子在最后一块的 ack 发出去之前就冷复位，主站报 `FOE_RECEIVE_ERROR`、USB 重新枚举、EtherCAT 链路掉。dmesg 里 USB 比 FoE 错误早 4 ms 断开，就是这个签名 | 复位必须等主站**退出 BOOT**（`APPL_StopMailboxHandler`）。我在注释里写明了这一点然后在代码里绕过了自己的设计 |
| 2 | 用 FoE 的 BUSY 数据报解决 ack 超时 | **错。** BUSY 是协议为"从站忙于编程 flash"专设的，返回值落在 `[FOE_MAXBUSY_ZERO, FOE_MAXBUSY]` 即可。但**IgH 的下载状态机不认它**——加上去之后传输更早失败（`FOE_PROT_ERROR`，且只到 `download opened`），去掉后恢复 | 协议支持不等于**这个主站**支持。已撤销，只在注释里留结论 |
| 3 | "擦除太慢"导致中间块超时 | **错。** 实测中间块（擦除 + 写）一直是好的，**只有最后一块**失败——它多了 commit 的两次写。修法是把 commit 挪到主循环，回调里只置 pending 标志立刻返回 ack | 我在**没有任何实测数字**的情况下推理了两轮"擦除太慢"。第一次测出 22–27 ms 是在改了三次实现之后 |
| 4 | 板子 bootloader 是旧的（`savebin` 读回差 87994 字节） | **错。** 复位 halt 后 **XPI 未配置**，普通内存读读 XIP 窗口全是 0xFF，拿它比对得出的差异是假的。J-Link 自己的 flash loader 走另一条路，它报的"只有 8192 字节需要改"才可信 | **验证手段本身要先被验证。** 读不到不等于内容不对 |

还有一条方法论错误，比上面任何一条都值得记：

**我在关键路径上没有观测就反复改实现。** 中间块本来就不打日志，所以"日志停在
`download opened`"既可能是第一块失败，也可能是中间块全部正常而最后一块没走到——
这两种情况需要完全不同的修法。补上每块日志和擦除计时之后，**一次就定位了**。

## 8. 遗留与后续

### 8.1 FoE 读回（upload）失败

`ethercat foe_read app` 返回 `FOE_MBOX_FETCH_ERROR`。已定位到很细：

- 回调**被调用了**（`foe: read req name_size=3 pw=0x0 blk=116 [app.]`）
- 所有检查通过，**第一块 116 字节的 DATA 确实发出去了**——IgH 的 dmesg 里能看到真实数据字节
- 失败在后续的 ACK → 续传环节
- SSC 的偏移处理（`u32FileOffset = nextState` 再累加）和我方 `read_data` 逻辑都核对过是对的

**影响**：不影响升级功能，但留下一个缺口——下载的**字节内容**没有硬件端到端佐证，
只有主机侧逻辑测试 + 板端字节计数 + 安装后的行为差异（第 6.3 节）。

一个已修的相关坑：读回必须**先 invalidate core1 的 D-cache**。core0 通过 RPC 编程了那些
字节并只 invalidate 了**自己的** cache；core1 直接解引用 XIP 地址会读到擦除前的内容，
表现为"刚提交的记录读作不存在"。`ecat_foe_support.cpp` 的 `read_staged()` 负责这件事，
**不要绕过它去用 `foe::staging_record_is_ready()`**（那个函数直接解引用）。`[实测]`

### 8.2 JTAG 连接的疑点（未定论）

**观察**（两次复现）：app 运行中 J-Link 报 `TotalIRLen = ?` 识别不到链；
`dfu-util -e` 让板子停在 bootloader 之后，`TotalIRLen = 5`，找到 1 个 TAP，连接成功。

**我曾把原因说成"app 复用了调试引脚"，这是错的**：`boards/hpm6e8y/board.c` 与
`board.h` grep 不到任何 JTAG 引脚配置，OTP 里 `DBG_PORT_LOCK = 0` 也没锁调试口。

**更可能的原因（假设，未验证）**：DFU 模式下 core1 未被释放，链上只有 1 个 TAP；
app 跑起来 core1 被释放就是 2 个 TAP，而当时用的是 `-jtagconf -1,-1` 自动探测。
`TotalIRLen = 5` 正好是单个 RISC-V TAP 的 IR 长度，与此吻合。而且
`ozone/hpm6e8y-core1.jdebug` 的注释本来就要求"让板子先跑起来再 attach"——
若"app 一跑 JTAG 就死"，那条指引根本不成立。

**待验证**（需插回探针）：app 运行状态下改用显式链位置，例如
`-jtagconf 0,0` 与 `-jtagconf 5,1`（或直接用 `HPM6E8YxGNx_CPU1` 设备）。
任何一条连上，就说明是链配置问题，"JTAG 只在 DFU 模式下能用"这条应当作废。

**眼下可用的做法**（站得住）：`dfu-util -e` 之后 J-Link 能连，烧 bootloader 就是这么
完成的。代价是**只能调 bootloader，调不了运行中的 app**。

### 8.3 其余可加的 EtherCAT 特性（本次未做）

按"对一个 4×CAN 电机桥的实际价值"排序，FoE 是其中第 4 项：

1. **安全态**。`APPL_StopOutputHandler()` 现在是空的——主站掉线/掉出 OP 时电机保持
   最后一条指令。SM 看门狗会正确把从站踢到 SAFEOP，**只是没人告诉 CAN 该停**。
   工作量很小，价值最高。
2. **DC / SYNC0**。板级 `AGENTS.md` 的选型结论：EtherCAT 换来的不是速度而是确定性和
   跨板同步。**不用 DC，这套投入换到的只有"一根线菊花链"。**
3. **线缆冗余（环形拓扑）**。补的正是选型表里 EtherCAT 唯一输给多块 USB 板的一栏
   （"链路断则下游全掉"）。从站侧几乎不用改，ESC 端口回环是硬件自动的。
4. **FoE**（本文）。
5. `0x1C32/0x1C33` 同步管理参数（与 DC 捆绑）。
6. Emergency + `0x10F3` 诊断历史（补 CAN TX 静默丢帧、UART 配置被拒等已知盲区）。
7. 动态 PDO 映射（消掉 stock/hybrid 的 SII revision 陷阱）。
8. Station Alias + 真实 Device ID（`APPL_GetDeviceID()` 现在硬编码返回 `0x5`，多板会认错）。

**不建议**：EoE（成本高，现有两条带内诊断通道已覆盖）、SoE/AoE/VoE（无对应需求）。
