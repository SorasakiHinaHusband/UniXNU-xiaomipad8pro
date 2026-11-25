[English](README.md) | 简体中文

# UniXNU-piano

在小米平板 8 Pro（`piano`，骁龙 8 至尊版 SM8750）上原生运行 macOS 27。本仓库是 [UniXNU](https://github.com/UniXNU/UniXNU) 的设备仓库：
https://github.com/lenzhang/UniXNU-xiaomipad8pro（原名 lenzhang/UniXNU-piano，本地检出仍沿用 UniXNU-piano 这个名字）。
27.3 线的 k273 XNU（k27 源码树加上 `patches/k27` 0053-0074 和 `patches/k273`）作为 HLOS 运行在 EL1，Gunyah 仍在 EL2。
启动链：ABL → BootShim → Mu-Silicium UEFI（FD，即 piano-uefi 的 `pianoPkg` 加上本仓库的改动）→ UniXNU 载荷与菜单 → DarwinBoot → XNU。
UniXNU 核心、Mu-Silicium、piano-uefi、DarwinBoot 和 SoC 模块都是固定版本的依赖（`unixnu.lock`、`feeds.toml`），不是分叉。

相关公开仓库：[UniXNU-sm8750-common](https://github.com/lenzhang/UniXNU-sm8750-common) 是今后共享的 SM8750 部分的占位仓库
（代码目前仍在本仓库，构建固定使用另一个 SM8750 模块）；[veil](https://github.com/lenzhang/veil) 是早期 Linux + Gunyah 客户机路线的冻结存档。
仓库、本地目录和规则的总览见 [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md)。

## 当前状态（2026-11-25）

原生 macOS **27.3（26C5035r）** 既能开机从磁盘启动，也能以 RAM 镜像启动。当前日常安装版本是 **v13.1y12k**（2026-11-25）。
系统识别为 Mac17,11，显示为 "Xiaomi Pad 8 Pro"，芯片为 "Snapdragon 8 Elite"。根卷在 `linux_rootfs`，机主的 Data 存储在 `linux_data`。
日常能耗模式为“高功率”。与 Android 对齐的 CPU/GPU 频率曲线和 PD 降压充电（9 V）默认开启，电荷泵保持关闭。

已安装系统上目前可用的功能：从磁盘启动并保留机主数据、60 Hz 显示、原生触摸屏、键盘保护套及其触控板、Wi-Fi（开机自动连接、开关、
断线后自动重连）、扬声器、电池电量百分比、5 V 和 9 V PD 充电及 80 % 充电上限、自动亮度、Adreno 830 上的 Metal 及分页 GPU 内存、
Photo Booth 与摄像头录像、QuickTime 和 Safari 中的硬件视频解码（H.264 最高 1080p），以及 Safari 的 WebGL 2 和 WebGPU。
尚不支持：蓝牙配对、已安装系统上的合盖与电源键睡眠、Chrome GPU 加速、电荷泵快充、日常 120 Hz、USB 主机设备和外接显示器。

**完整的逐项功能表（含每项结果验证时的版本和日期）：[docs/STATUS.zh-CN.md](docs/STATUS.zh-CN.md)。**
今天的平板测试结果：[docs/results-2026-11-25.md](docs/results-2026-11-25.md)（英文）。

**效果展示：** 各项功能的截图和短片（在平板上直接截取）收录在 [docs/STATUS.zh-CN.md#效果展示](docs/STATUS.zh-CN.md#效果展示)
（第一批原生截图安排在下一次测试启动时拍摄）。

USB-C 调试控制台（`package/PianoDwc3`、`tooling/native/usb-console.py`）和通过数据线的 ssh 都可用；控制台监视器的
`reset fastboot` 命令无需按键即可让平板回到 ABL。日常开发使用 `fastboot boot` 启动 RAM 镜像，它优先于已安装的系统。

各驱动的说明、启动参数和测试级别：[docs/drivers/README.md](docs/drivers/README.md) 和 [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md)。
已安装系统的使用、更新和恢复：[docs/USING.md](docs/USING.md)。更早的公开记录包括
[docs/results-2026-11-19.md](docs/results-2026-11-19.md) 和 [docs/results-2026-11-17.md](docs/results-2026-11-17.md)。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `patches/k27/` | k27 内核补丁 0053-0074（没有 0072），基于 k27 源码树（xnu-12377 + 基线补丁 0001-0052） |
| `patches/k273/` | 27.3 线在 `patches/k27` 之上的内核补丁 |
| `patches/k27-kc/` | KC 构建器的原生布局（最佳适配、`__TEXT` 中无重定位常量），需手动应用到 k27 树（[docs/kc-space.md](docs/kc-space.md)） |
| `patches/xnu/` | UniXNU macOS 27.3 线的设备补丁（目前为空） |
| `package/` | 原生 kext；没有对应启动参数时都不生效 |
| `configs/` | 原生 kext 列表（`k27-native-kexts.txt`）、ADT 标识与显示节点（`k27-adt.toml`、`k27-gop-display.toml`）、27.3 线的内核配置 |
| `dts/piano.dts` | 用于 ADT 的 piano 硬件描述 |
| `tooling/` | `k273/build-k273-image.sh`（27.3 kext → 内核集合 → ADT → RAM 镜像，`K273_KERNEL`）、`k273/build-k273-esp.py`（已安装系统的 27.3 ESP）、`kc/`（KC 构建）、`adt/`、`bootimg/`（RAM 镜像及其校验）、`native/`（测试循环、USB 控制台、输入）、`persist/`（安装到磁盘）、`firmware/`（FD） |
| `firmware/` | 对 piano-uefi 的 `pianoPkg`、Mu-Silicium 和 UniXNU 的改动 |
| `docs/` | 移植笔记、驱动说明（`drivers/`）、计划和证据 |
| `skills/`、`tooling/repro/` | 面向其他设备的代理指南及工具：Apple 输入文件的固定版本、自行下载、根卷重建校验、主机检查 |
| `logs/`、`artifacts/ARTIFACTS.md` | 已提交的验证日志；构建产物和输入的 SHA-256 |
| `private/`、`out/` | 被忽略：密钥、单机校准数据；构建输出 |

## 构建

新设备的完整流程（要求、输入、构建、RAM 启动验证、受保护的安装、回滚、检查）见
[skills/install-piano-macos/SKILL.md](skills/install-piano-macos/SKILL.md)（英文，面向编程代理）。本节是摘要，以该指南为准。

### 主机要求

- **必须是 Apple 芯片的 Mac（M1 或更新）。不支持 Intel Mac。** Xcode 27 的编译器和链接器以及 macOS 27 自带的工具只有 arm64 版本，
  而且有两个构建步骤会运行自己编译出的 arm64e 程序。证据见该指南 §1.1。
- 已测试：macOS 27.2.1 加 **Xcode 27.2**（需要完整的 Xcode，仅有命令行工具不够：内核需要 iphoneos SDK 和 `Kernel.framework`，
  并用 Xcode 的 `ld-classic` 链接）。Homebrew 装在 `/opt/homebrew`，并安装指南 §1.2 列出的软件包、`fastboot`、Python 3.11 或更新版本；
  约 90 GiB 可用磁盘空间；根卷步骤需要在前台终端里使用 `sudo`。
- `bash tooling/repro/check-host.sh` 只读检查主机，每项输出 `ok` / `WARN` / `FAIL` / `GAP`；只有 `0 FAIL` 时才继续。
  请在原生 arm64 shell 中运行，不要在 Rosetta 下运行。

### 源码与固定版本

- 本仓库，加上 `unixnu.lock` / `feeds.toml` 中固定版本的依赖（UniXNU 核心、Mu-Silicium、piano-uefi、DarwinBoot、SoC 模块），
  以及 k27 内核源码树（`PIANO_K27_TREE`，写在环境变量或 `~/.config/piano/env` 中；版本固定在 `unixnu.lock`）。
- 本项目从不分发 Apple 的文件。`tooling/repro/fetch-ipsw.py` 从 Apple 的 CDN 下载固定版本 27.3 IPSW 的组成文件，从 Apple 获取 AEA 密钥，
  并按 `tooling/repro/pins.json` 校验每个文件。
- 有些输入目前仍只存在于原始环境（k27 内核源码树、基础信任包、Data 种子、Turnip 源码树、写入工具中的单机固定值）：
  指南的 "Known gaps" 一节逐项列出并给出可选方案。

### 构建步骤

```sh
bash tooling/repro/fetch-and-build.sh --dry-run   # 列出将要执行的每条命令；不下载、不构建、不写入
bash tooling/repro/fetch-and-build.sh             # host、inputs、systemos、root、kernel、fd、image（也可按名字只跑一步）
bash tooling/repro/fetch-and-build.sh --check     # 按固定值检查每一步的输出（不联网）
```

它执行的步骤，也可以逐个运行：

```sh
bash tooling/k273/prepare-k27-tree.sh          # 27.3 内核源码树及重新链接的内核；打印 K273_KERNEL 的路径
bash tooling/firmware/build-fd.sh               # 从 unixnu.lock 的检出构建 UEFI FD
K273_KERNEL=<上一步打印的路径> bash tooling/k273/build-k273-image.sh   # kext → 内核集合 → ADT → RAM 镜像
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

- **kext 与内核集合：** KC 只链接本仓库的 kext（`configs/k27-native-kexts.txt`）；`tooling/k27/check-native-kexts.py` 在链接前后都会强制检查。
  KC 日志中的通过标志：`native kext rule: ... all this repository's`。
- **根卷镜像在本地生成，从不分发。** 20.9 GB 的根卷由 IPSW 的 OS 卷（逐字节一致，版本固定）加上由 `tooling/k273/make-systemos.sh`
  和 `make-root-image.sh` 重建的 SystemOS 窗口组成，并由 `tooling/repro/verify-root.py` 按固定的文件级清单校验（指南 §3.1）。
- 只构建 27.3 线；已退役的 27.0.1 路线（`tooling/k27/build-native-image.sh`）不再使用。根卷位置：新设备先把根卷放在 `linux_data`
  （`K273_ROOT_PART=linux_data`，指南 §3.2）；参考设备的已安装 ESP 使用 `build-k273-esp.py --root-part linux_rootfs`
  （[docs/persistent-boot.md](docs/persistent-boot.md) §13.8）。

### RAM 镜像的大小上限

RAM 启动镜像**最多 250,000,000 字节**。ABL 会把整个镜像载入 UEFI 的 256 MiB 内核区域；一个 294,952,960 字节的镜像在下载后一直停在
fastboot，而 247,767,040 字节的镜像可以启动（[docs/k273-line.md](docs/k273-line.md) §4）。该上限是 `tooling/bootimg/build-ram-images.py`
中的 `IMAGE_MAX_BYTES`（镜像将要超限时它会压缩载荷的空闲空间），`tooling/bootimg/verify-ram-images.py` 会再检查一次
（两者都读取 `PIANO_IMAGE_MAX_BYTES`；不要调大）。

### 安装前先做 RAM 启动

每个镜像都要先用 `fastboot boot` 启动（RAM 启动不写任何分区），在那里通过后才能写入磁盘：控制台能走到 XNU，USB 控制台有响应，
启动能通过复位保护回到 ABL；桌面镜像还要能正确显示登录窗口，并且触摸、键盘保护套、ssh、Wi-Fi 和电池读数都正常（指南 §4）。
安装只能通过 `tooling/persist/maint-session.py` 进行，每一步都有备份、回读校验和回滚
（[docs/persistent-boot.md](docs/persistent-boot.md)、[docs/install-esp-v3.md](docs/install-esp-v3.md)）。

更多：启动参数与无屏调试见 [docs/bringup-debugging.md](docs/bringup-debugging.md)（§4 说明从 k27 树读取哪些内容）；
当前可用功能见 [docs/STATUS.zh-CN.md](docs/STATUS.zh-CN.md)。

## 启动与测试

日常开发使用 RAM 启动：`fastboot boot` 一个 RAM 镜像（它优先于已安装的系统），由无人值守的循环启动测试、复位并读回测试日志
（`tooling/native/native-loop.sh`，[docs/bringup-debugging.md](docs/bringup-debugging.md) §3）。安装到磁盘
（[docs/persistent-boot.md](docs/persistent-boot.md)）只写该文档列出的区域，每个区域先做经过校验的备份，每次写入后回读校验，
每一步都有回滚；从不写固件分区。请勿手动刷写 boot 或固件分区。

## 原生 kext 规则

原生内核集合只链接本仓库的 kext（`configs/k27-native-kexts.txt`）。`tooling/k27/check-native-kexts.py` 在每次构建的链接前后都强制检查：
不允许来自别处的描述文件，也不允许虚拟机时代的半虚拟化 kext。

## 延伸阅读

- [docs/STATUS.zh-CN.md](docs/STATUS.zh-CN.md)：逐项功能状态，以及每项验证时的版本和日期。
- [docs/USING.md](docs/USING.md)：已安装系统的使用、更新和恢复。
- [skills/install-piano-macos/SKILL.md](skills/install-piano-macos/SKILL.md)：在其他设备上构建和安装（代理指南）。
- [docs/drivers/README.md](docs/drivers/README.md)：所有驱动、其级别和状态。
- [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md)：仓库、本地布局、硬性规则。
- [docs/bringup-debugging.md](docs/bringup-debugging.md)：启动链、无屏调试、测试循环、重新构建。
- [docs/BACKLOG-hardware.md](docs/BACKLOG-hardware.md)：尚待适配的硬件。
- [docs/kc-space.md](docs/kc-space.md)：内核集合中留给原生 kext 的空间、运行时固件加载器。

（以上文档除注明外均为英文。）

## 许可与内容

除非某个软件包的 `LICENSE`、文件的 SPDX 头或 `UPSTREAM` 文件另有说明，本仓库代码采用 BSD-2-Clause-Patent，
Copyright (c) 2026 Leo Zhang（第三方部分保留其原有版权人和许可）。
本仓库从不包含厂商固件、分区镜像和 Apple 载荷（内核集合、信任缓存、由 IPSW 派生的文件），也不包含机密信息、密钥、单机校准数据、
本地网络或存储细节以及机器名。
