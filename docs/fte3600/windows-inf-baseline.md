# Windows INF 分析：第一份基线样本

分析日期：2026-10-04。范围：仓库 `scripts/install-firmware.sh` 引用的单个驱动包；本报告不是全部 FTE3600 驱动版本或机型的覆盖清单。本步骤只分析 INF，未执行或安装 Windows 驱动，未修改 libfprint 实现。

## 来源与可复核标识

- [Microsoft 托管的 CAB 原始下载地址](https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2026/08/e684f740-91ac-4458-9097-09850eaedf9d_4f80a6cb0c9e453d4619667af7c92eadeb165e0f.cab)
- CAB SHA-256：`2c3380810f40ca2152fdce3f0f237441521f65fee80cbe802a2def4ca01d6cb5`
- INF：`ftWbioUmdfDriverV2.inf`
- INF SHA-256：`b64c8ebe2ec65f47996fa8f27e8f68f9c91076b7cb915839aafde51f6240c1dd`
- INF 第 11 行：驱动日期 `07/05/2025`，版本 `2.0.3.102`。日期是 INF 声明值，不是下载日期；下载地址中的年月也不等于驱动版本日期。
- 提供商：`FocalTech Electronics(ShenZhen)Co.,Ltd`（第 9、264 行）。
- 类别：`Biometric`；声明目录文件 `ftWbioUmdfDriverV2.cat`（第 7–10 行）。本步骤未验证目录签名或成员签名。
- 本地原始样本保存在工作区 `work/windows-driver-baseline/`，位于源码仓库之外。报告仅记录分析所得的事实与定位信息。

CAB 包含 7 个文件：一个 INF、一个 CAT、`ftWbioUmdfDriverV2.dll`、`ftWbioEngineAdapter.dll`、`ftWbioSensorAdapter.dll`、`ftWbioStorageAdapter.dll` 和 `focalFpSrvcDeamon.exe`。没有独立命名的固件文件；这不排除二进制内嵌固件。

## 完整设备映射（仅限此 INF）

`[Manufacturer]` 第 15 行引用 `Standard` 的两个目标修饰版本。以下全部是生效条目，不包含注释。按 [Microsoft INF Models 语法](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-models-section)，安装节之后的首个 ID 是硬件 ID，其后才是兼容 ID；本样本各条目没有额外兼容 ID。

| INF 行 | Models 节 | 硬件 ID | 安装节基名 | 对应主安装节 |
| --- | --- | --- | --- | --- |
| 18 | `Standard.NTamd64` | `ACPI\FTE3600` | `SPIdevice_Install` | `SPIdevice_Install.NT` |
| 19 | `Standard.NTamd64` | `USB\VID_2808&PID_9338` | `USBdevice_Install` | `USBdevice_Install.NT` |
| 22 | `Standard.NTamd64.10.0...22631` | `ACPI\FTE3600` | `23H2_SPIdevice_Install` | `23H2_SPIdevice_Install.NT` |
| 23 | `Standard.NTamd64.10.0...22631` | `USB\VID_2808&PID_9338` | `23H2_USBdevice_Install` | `23H2_USBdevice_Install.NT` |

结论：4 条映射，2 个唯一硬件 ID。两个 Models 节均为 AMD64 目标；第二个带 `10.0...22631` 目标修饰。`23H2_` 是厂商安装节命名，不是另一种硬件型号。设备显示名称均解析为 `FocalTech Fingerprint reader`（第 268–269 行）。

`ACPI\FTE3600` 走 SPI 安装路径，USB ID 走 WinUSB 路径。这是此驱动包声明的设备适配范围，不能据此将 USB 设备认定为 FT9361，或将相同 ACPI ID 下的全部设备视为相同芯片、引脚布局及固件。

## 为第二步建立的注册表追踪入口

本节记录引用关系，不对尚未分析的键值作硬件语义解释。

| 安装路径 | 主安装节 AddReg | `.NT.hw` AddReg |
| --- | --- | --- |
| SPI，两个目标版本 | `FTFP_AlgInfo.AddReg` | `Biometric_Device_AddReg`、`DriverPlugInAddReg`、`DatabaseAddReg` |
| USB，两个目标版本 | `FTFP_AlgInfo.AddReg` | 上述三个节，另加 `usb_device_include` |

定位：USB 第 37–90 行，SPI 第 97–139 行；注册表定义在第 92–94、209–228、231–260 行。必须保留同一节中重复出现的 `AddReg=` 指令，不能用普通单值 INI 字典覆盖前面的条目。

此 INF 没有按 A1、GPD Pocket 3、Medion E3224 或其他整机型号拆分的 Models/AddReg 分支，也没有可直接提取的 GPIO 引脚映射、SPI 初始化寄存器序列或固件选择表。已观察到的显式配置分支是传输路径和 Windows 目标版本。机型差异是否来自其他 OEM INF、ACPI 资源、运行时芯片识别或 DLL 内部逻辑，仍待取证。

第二步的本地键值提取已完成，见 [注册表差异报告](windows-registry-baseline.md) 和 [结构化清单](windows-registry-baseline.json)。其中记录根、子路径、名称、类型标志、原始值、字符串展开值、来源行、适用安装路径及 `Include` / `Needs` 依赖；外部系统 INF 尚未展开。仅分析本 INF 不能声称得到安装后的完整注册表状态。

## 与现有 Linux 实现的关系

当前 `libfprint/drivers/fte3600.h` 的设备表匹配 `FTE3600`，图像参数为 FT9361 的 64 × 80；现有代码和硬件验证范围见仓库 `status.md`。这些是现有 Linux 实现的信息，不是本 INF 对芯片型号的证明。

后续顺序：先完成注册表差异清单，再定位固件和初始化数据并记录偏移、长度、哈希、选择条件与证据，最后根据已确认的技术规范扩展 libfprint。技术事实说明和独立实现需要保留来源；提取出的固件不能仅因被提取就标记为开源或获得再分发授权。厂商二进制仍保留在源码仓库之外，延续 `clean-room.md` 的现有边界。
