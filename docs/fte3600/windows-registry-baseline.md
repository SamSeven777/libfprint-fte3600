# Windows 注册表配置分析：2.0.3.102 基线

分析日期：2026-10-04。承接 [INF 设备映射](windows-inf-baseline.md)，仅分析同一份 `ftWbioUmdfDriverV2.inf`，SHA-256 为 `b64c8ebe2ec65f47996fa8f27e8f68f9c91076b7cb915839aafde51f6240c1dd`。

完整结构化记录见 [windows-registry-baseline.json](windows-registry-baseline.json)：包含每条定义的来源节、行号、原始字段、展开后的路径、类型标志、值、操作以及全部四条安装路径的引用关系。此处是静态安装声明分析，未安装驱动或读取实际设备注册表。

## 已确认的差异

| 安装路径 | 硬件 ID | 本地 AddReg 声明数 | 不同目标值数 |
| --- | --- | ---: | ---: |
| `SPIdevice_Install` | `ACPI\FTE3600` | 42 | 41 |
| `23H2_SPIdevice_Install` | `ACPI\FTE3600` | 42 | 41 |
| `USBdevice_Install` | `USB\VID_2808&PID_9338` | 44 | 43 |
| `23H2_USBdevice_Install` | `USB\VID_2808&PID_9338` | 44 | 43 |

计数按单个安装路径、单个设备实例计算。`Exclusive=1` 在第 234、243 行重复出现，根、子路径、名称、类型和值相同，因此不会增加不同目标值的数量。两种 Windows 目标版本的本地 AddReg 定义一致；USB 比 SPI 仅多两项本地声明。全部 44 条源定义在四条路径上形成 172 次引用。

这里的“一致”仅指此 INF 的显式 AddReg。系统依赖节、服务安装、WDF 策略、已有注册表状态和运行时写入仍可能造成安装后状态不同。相同 HKR 相对路径在 SPI 和 USB 设备上指向各自的设备实例，不是同一绝对键。

## 路径、类型与数值规则

- `A` = `HKLM\System\CurrentControlSet\Control\focalFp`。来自 `%ServiceRoot%` 的第 271 行定义；名称虽然叫 ServiceRoot，展开后实际为 **Control**，不是 Services。这 17 项是机器级共享配置。
- `H` = 安装设备的硬件键。此样本的 HKR 条目全部通过 `.NT.hw` 引用，具体设备实例路径未知，不编造绝对路径。
- `D` = `HKLM\System\CurrentControlSet\Services\WbioSrvc\Databases\{91CF558A-2540-4C3D-9A85-4AD392FDE4DA}`。这是显式的全局数据库配置路径。
- `0x00010001` / `0x10001` / `%REG_DWORD%` 均表示 DWORD；省略 flags 表示字符串；`0x00010008` 表示多字符串追加且避免重复。十进制原文按十进制解读，`0x` 前缀按十六进制解读。表中类型表示 INF 声明类型，特殊设备属性最终如何存储不由此表证明。

HKR 作用域和类型规则依据 [Microsoft AddReg 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addreg-directive)。原始数值拼写、原始 flags 和归一化值均保留在 JSON 中。

## 厂商参数：四条路径共用

来源节 `FTFP_AlgInfo.AddReg`，全部在路径 `A` 下，全部为 DWORD。以下只是键名与声明默认值；没有根据名称推导算法公式、单位或硬件寄存器。

| 行 | 名称 | 十进制值 |
| --- | --- | ---: |
| 210 | AlgMaxTemplates | 42 |
| 211 | EnrollMaxTemplates | 18 |
| 212 | EnrollScore | 100 |
| 213 | VerifyLevel | 15 |
| 214 | UpdateLevel | 17 |
| 215 | NonFingerDetect | 0 |
| 216 | AppLockEnable | 1 |
| 218 | CheckEnable | 0 |
| 219 | ValidEreaScore | 75 |
| 220 | ValidEreaCenterRate | 200 |
| 221 | QualityScore | 30 |
| 222 | CondScore | 25 |
| 224 | Center | 8 |
| 225 | BottomEdge | 2 |
| 226 | LeftEdge | 3 |
| 227 | RightEdge | 3 |
| 228 | TopEdge | 2 |

保留厂商原始拼写 `ValidErea*`。不能把 `Center` / `*Edge` 直接当作像素裁剪尺寸，也不能将匹配阈值直接移植到当前 libfprint 的独立匹配实现；需要先确认读取函数及后续数据流。

## 设备配置：四条路径共用

来源节 `Biometric_Device_AddReg`。

| 行 | 路径 | 名称 | 类型 | 声明值 |
| --- | --- | --- | --- | --- |
| 232 | H | DeviceCharacteristics | DWORD | `0x0100`（256） |
| 233 | H | Security | SZ | `D:P(A;;GA;;;BA)(A;;GA;;;SY)` |
| 234 | H | Exclusive | DWORD | 1 |
| 235 | H | SystemWakeEnabled | DWORD | 1 |
| 236 | H | DeviceIdleEnabled | DWORD | 1 |
| 237 | H | UserSetDeviceIdleEnabled | DWORD | 1 |
| 238 | H | DefaultIdleState | DWORD | 1 |
| 239 | H | DefaultIdleTimeout | DWORD | 5000 |
| 240 | H\WDF | WdfDirectedPowerTransitionEnable | DWORD | 1 |

这是 Windows 设备安装/电源策略配置，不构成 SPI 寄存器表，也不能据此推导 Linux 下的复位延时或 GPIO 极性。保留 `Security` 字符串内的分号，不能误当成 INF 注释截断。

## WinBio 插件配置：四条路径共用

来源节 `DriverPlugInAddReg`。

| 行 | 路径 | 名称 | 类型 | 声明值 |
| --- | --- | --- | --- | --- |
| 243 | H | Exclusive | DWORD | 1（与第 234 行同值重复） |
| 244 | H\WinBio\Configurations | DefaultConfiguration | SZ | `"0"` |
| 245 | H\WinBio\Configurations\0 | SensorMode | DWORD | 1 |
| 246 | H\WinBio\Configurations\0 | SystemSensor | DWORD | 1 |
| 247 | H\WinBio\Configurations\0 | SensorAdapterBinary | SZ | `ftWbioSensorAdapter.DLL` |
| 248 | H\WinBio\Configurations\0 | EngineAdapterBinary | SZ | `ftWbioEngineAdapter.DLL` |
| 249 | H\WinBio\Configurations\0 | StorageAdapterBinary | SZ | `ftWbioStorageAdapter.DLL` |
| 250 | H\WinBio\Configurations\0 | DatabaseId | SZ | `91CF558A-2540-4C3D-9A85-4AD392FDE4DA` |

`DefaultConfiguration` 是字符串 `"0"`，不是 DWORD 0。上述 GUID 是配置标识，不是芯片 ID。

## WinBio 数据库配置：四条路径共用

来源节 `DatabaseAddReg`，路径均为 `D`。

| 行 | 名称 | 类型 | 声明值 |
| --- | --- | --- | --- |
| 253 | BiometricType | DWORD | `0x00000008`（8） |
| 254 | Attributes | DWORD | `0x00000001`（1） |
| 255 | Format | SZ | `CDAE92F1-5B32-4a91-94A8-56ACA204B3B9` |
| 256 | InitialSize | DWORD | `0x00000020`（32） |
| 257 | AutoCreate | DWORD | 1 |
| 258 | AutoName | DWORD | 1 |
| 259 | FilePath | SZ | 空字符串 |
| 260 | ConnectionString | SZ | 空字符串 |

空字符串是明确的声明值，不是缺失数据。

## USB 独有的两项

来源节 `usb_device_include`，只由两条 USB 安装路径的 `.NT.hw` 引用（第 46、71 行）。

| 行 | 路径 | 名称 | 类型/操作 | 声明值 |
| --- | --- | --- | --- | --- |
| 93 | H | LowerFilters | MULTI_SZ，若不存在则追加 | `WinUsb` |
| 94 | H | WinUsbPowerPolicyOwnershipDisabled | DWORD，设置 | 1 |

第 93 行不是把完整过滤器列表替换成单一 `WinUsb`；最终列表取决于已有内容及其他安装操作。这两项是 Windows USB 驱动栈差异，不能当作传感器芯片的初始化差异。

## AddReg 之外的安装差异及外部依赖

保留同一节中所有重复出现的 Include/Needs 指令。下表将每节的依赖作为集合列出，不假定相邻两行一定是一一配对关系。外部文件不在本 CAB 中，未使用当前计算机上的系统 INF 代替样本目标 Windows 版本的文件。

| 安装路径 / 节后缀 | Include 文件 | Needs 节 |
| --- | --- | --- |
| SPI / `.NT`、`.NT.hw`、`.NT.Services` | 无 | 无 |
| USB / `.NT` | WINUSB.INF | WINUSB.NT |
| 23H2_SPI / `.NT` | WUDFRD.INF | WUDFRD.NT |
| 23H2_SPI / `.NT.hw` | WUDFRD.INF | WUDFRD.NT.HW |
| 23H2_SPI / `.NT.Services` | WUDFRD.INF | WUDFRD.NT.Services |
| 23H2_USB / `.NT` | WINUSB.INF、WUDFRD.INF | WINUSB.NT、WUDFRD.NT |
| 23H2_USB / `.NT.hw` | WUDFRD.INF、WINUSB.INF | WUDFRD.NT.HW、WINUSB.NT.HW |
| 23H2_USB / `.NT.Services` | WUDFRD.INF、WINUSB.INF | WUDFRD.NT.Services、WINUSB.NT.Services |

普通 USB 的 `.NT.hw` / `.NT.Services` 没有本地 Include/Needs。上述来源行及完整依赖列表见 JSON。依据 [Microsoft DDInstall 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-ddinstall-section)，Needs 指定安装时还需处理的系统 INF 节；没有这些文件就无法展开全部依赖操作。

- 普通 SPI 显式 AddService `WUDFRd`；普通 USB 显式 AddService `WUDFRd`、`WinUsb`、`focalFpSrvcDeamon`。两条 23H2 路径通过系统 INF 引入框架服务，23H2 USB 另显式添加 `focalFpSrvcDeamon`。仅从文件复制清单出现 EXE 不能认定 SPI 安装了该服务。
- 两条 USB 路径指定 `UmdfDispatcher=WinUsb`。两条 SPI 路径和 23H2 USB 指定 `UmdfDirectHardwareAccess`、`UmdfFileObjectPolicy`、`UmdfImpersonationLevel`；普通 USB 没有显式设置这三项。缺少声明不等于运行时取相反值。
- 四条路径的 `UmdfLibraryVersion` 均为 `2.15.0`。UMDF 服务二进制路径从普通安装的 `%12%\UMDF\ftWbioUmdfDriverV2.dll` 改为 23H2 安装的 `%13%\ftWbioUmdfDriverV2.dll`。USB 附加服务的 EXE 路径有对应变化。保留 DIRID 占位符，未臆造系统绝对路径。

这些差异不能被“两个 Windows 目标版本 AddReg 相同”掩盖。本报告没有模拟服务安装或 WDF 指令产生的注册表写入。

## 验证结果与下一步

已核对全部五个 AddReg 定义节：厂商参数 17、设备配置 9、插件配置 8、数据库配置 8、USB 附加 2。提取保留重复 AddReg 引用，核对字符串展开、十六进制转换、空字符串、多字符串追加，以及含分号的安全描述符。四条路径共 172 条引用，两个 Windows 版本之间的本地键值集合一致，USB 与 SPI 差集恰为上述两项。未执行硬件测试，本步骤不涉及驱动代码修改。

**当前未发现此 INF 内的整机型号差异化键值、芯片型号选择键、GPIO 路由或固件/初始化表选择键。** 这不代表这些差异不存在于其他驱动包或运行时逻辑中。

下一步进入二进制静态分析：先记录各文件哈希与 PE 信息，定位这 17 项配置的实际读取位置及芯片识别分支，再核对已知 FT9361 固件的偏移、长度和哈希，并追踪初始化序列的选择条件。仅有字符串命中不能证明配置被使用，候选二进制片段不能直接当成已确认固件。

后续研究已核实主要硬件适配机制，见 [Windows 运行时适配分析](windows-runtime-adaptation.md)。该研究聚焦资源绑定、芯片检测与固件选择；17 项厂商参数的完整读取数据流仍未逐一还原。
