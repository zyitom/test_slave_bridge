# 外部工具下载入口

> **文档类型**：硬件参考（外部资源速查）
> **适用范围**：全仓库，需要从厂商官网下载的工具
> **状态**：现行有效
> **相关文档**：[AGENTS.md](AGENTS.md#开发机环境路径约定重要先读这条再看任何路径) · [firmware/ch32_board/AGENTS.md](firmware/ch32_board/AGENTS.md) · [firmware/rmcs_board/BUILD_ENVIRONMENT.md](firmware/rmcs_board/BUILD_ENVIRONMENT.md)

## 摘要

本文件只放**外部下载入口**，即那些不能入库、必须从厂商官网取的工具。装到哪、
环境变量怎么设，见对应板子的 `AGENTS.md`；本文件不复述安装步骤。

## 下载清单

| 工具 | 用途 | 下载地址 |
|---|---|---|
| CH32 编译器（MounRiver Studio / MRS Toolchain） | `ch32_board` 的 RISC-V 工具链与 OpenOCD | <https://www.mounriver.com/download> |

### CH32 编译器（MounRiver）

原始记录：

```
CH32编译器
https://www.mounriver.com/download
```

补充说明：

- 这个包里同时提供 **RISC-V GCC 工具链**和 **WCH 版 OpenOCD**（含 `wch-riscv.cfg`
  等 cfg 文件），`ch32_board` 的烧录和调试都依赖它。
- MRS 包内并排放着三套 GCC，前缀和可用性各不相同，选哪一套见
  [firmware/ch32_board/AGENTS.md](firmware/ch32_board/AGENTS.md) 的"芯片与工具链"一节。
- `ch32_board` 默认并不用这套工具链编译，而是复用 rmcs_board 的 HPM 工具链
  （原因是 FLASH 余量，同上文档有实测数据）；但 OpenOCD 仍取自这个包。
- 安装位置不固定，文档中出现的 `~/3rd_party/MRS_Toolchain_Linux_X64_V240` 是
  **上一台开发机**的实际路径（`[前机路径]`），当前机器未安装，按
  [AGENTS.md 开发机环境路径约定](AGENTS.md#开发机环境路径约定重要先读这条再看任何路径)
  理解即可。

## 其他工具链的获取方式

不在本文件列出、但同样需要外部安装的：

- **HPMicro GNU 工具链**（`rmcs_board`，同时被 `ch32_board` 复用）：获取与安装步骤见
  [firmware/rmcs_board/BUILD_ENVIRONMENT.md](firmware/rmcs_board/BUILD_ENVIRONMENT.md)。
- **`arm-none-eabi-gcc`**（`c_board`、`mc02`）：发行版包管理器直接安装，见
  [README.md 固件编译与烧录](README.md#固件编译与烧录)。
- **`dfu-util`**、**`libusb-1.0-dev`**：发行版包管理器安装，见 [README.md](README.md)。
