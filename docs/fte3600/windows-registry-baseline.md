# Windows registry configuration analysis: the 2.0.3.102 baseline

[Documentation index](README.md)

<a id="windows-注册表配置分析203102-基线"></a>

Analysis date: 2026-10-04. Following the [INF device mapping](windows-inf-baseline.md), this report
examines only the same `ftWbioUmdfDriverV2.inf`, SHA-256
`b64c8ebe2ec65f47996fa8f27e8f68f9c91076b7cb915839aafde51f6240c1dd`.

The complete structured record is in
[windows-registry-baseline.json](windows-registry-baseline.json): it includes each definition's
source section, line number, original fields, expanded path, type flags, value, operation, and
references from all four installation paths. This is a static analysis of installation declarations;
no driver was installed and no physical device registry was read.

The later [engine-adapter cross-check](#engine-adapter-cross-check-2026-10-05)
adds selected binary data-flow evidence without changing the INF counts above.

<a id="已确认的差异"></a>

## Confirmed differences

| Installation path | Hardware ID | Local AddReg declarations | Distinct target values |
| --- | --- | ---: | ---: |
| `SPIdevice_Install` | `ACPI\FTE3600` | 42 | 41 |
| `23H2_SPIdevice_Install` | `ACPI\FTE3600` | 42 | 41 |
| `USBdevice_Install` | `USB\VID_2808&PID_9338` | 44 | 43 |
| `23H2_USBdevice_Install` | `USB\VID_2808&PID_9338` | 44 | 43 |

Counts are per installation path and device instance. `Exclusive=1` appears at both lines 234 and
243 with identical root, subpath, name, type, and value, so it does not increase the distinct-value
count. The local AddReg definitions are identical for the two Windows target versions; USB adds only
two local declarations compared with SPI. The 44 source definitions produce 172 references across
the four paths.

Here, “identical” refers only to this INF's explicit AddReg declarations. System dependency
sections, service installation, WDF policies, existing registry state, and runtime writes can still
produce different installed states. The same HKR-relative path refers to each device's own instance
on SPI and USB, not to one absolute key.

<a id="路径类型与数值规则"></a>

## Paths, types, and numeric interpretation

- `A` = `HKLM\System\CurrentControlSet\Control\focalFp`. This expands `%ServiceRoot%`, defined at
  line 271. Despite the name ServiceRoot, the path is under **Control**, not Services. These 17
  values are shared machine-level configuration.
- `H` = the installed device's hardware key. Every HKR entry in this sample is referenced through
  `.NT.hw`; the specific device-instance path is unknown, so no absolute path is invented.
- `D` =
  `HKLM\System\CurrentControlSet\Services\WbioSrvc\Databases\{91CF558A-2540-4C3D-9A85-4AD392FDE4DA}`.
  This is an explicitly declared global database-configuration path.
- `0x00010001` / `0x10001` / `%REG_DWORD%` all mean DWORD; omitted flags mean a string; `0x00010008`
  means append to a multistring without adding duplicates. Decimal literals are interpreted as
  decimal and `0x`-prefixed literals as hexadecimal. Table types are those declared in the INF; this
  table does not prove how special device properties are ultimately stored.

HKR scope and type rules follow the
[Microsoft AddReg documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addreg-directive).
Original numeric spellings, original flags, and normalized values are all retained in the JSON.

<a id="厂商参数四条路径共用"></a>

## Vendor parameters shared by all four paths

Source section: `FTFP_AlgInfo.AddReg`. All values are DWORDs under path `A`. These are only the key
names and declared defaults; no algorithm formula, unit, or hardware register is inferred from a
name.

| Line | Name | Decimal value |
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

The vendor spelling `ValidErea*` is preserved. `Center` / `*Edge` must not be treated as pixel-crop
dimensions, and matching thresholds must not be copied directly into libfprint's independent
matcher. The engine cross-check below identifies the former as enrollment sample
requirements; several algorithm thresholds still lack a complete downstream interpretation.

## Engine-adapter cross-check: 2026-10-05

Sample: `ftWbioEngineAdapter.dll`, 1,287,336 bytes, SHA-256
`2e70ec5599e5269fb9f7999a111d67a5ccd644e4a30b45f139d718f43e809004`.
Addresses in this section are RVAs in that engine DLL, not in the UMDF driver.
This check evaluates specific claims in an external package-analysis report;
it does not reproduce the proprietary matcher or establish its accuracy.

| Claim | Verified data flow and limit |
| --- | --- |
| `Center` and edge values are pixel crop dimensions | Incorrect. `B950` is `EngineAdapterQueryExtendedInfo`; `BABE–BB5F` writes them into output offsets `10/14/18/1c/20`. These are the center/top/bottom/left/right enrollment sample requirements, not image coordinates. The INF defaults total 18 regional samples. |
| `4740` reads all 17 keys | Incorrect. Function `4740–48f2` reads seven: AlgMaxTemplates, EnrollMaxTemplates, EnrollScore, VerifyLevel, UpdateLevel, QualityScore and ValidEreaScore. Other values need their own readers traced. |
| VerifyLevel=15 is a demonstrated independent FAR threshold | Not established. The stores at `4843` and `4870` for VerifyLevel and UpdateLevel target the same global `1504ec`. Later diagnostics at `73f8/7410` distinguish separate verify/update structure fields. The registry reader alone neither establishes the operative verify setting nor provides a measured FAR mapping. |
| ValidEreaScore=75 proves 75% sensor coverage | Only the normalization is established: `48b5–48cd` divides by 100; `73bd` labels it `valid_area_scale`, and `d98e→24150` supplies it to a setting operation. The denominator and region used by the actual acceptance decision were not established by this check. |
| EnrollScore=100 rejects every image with quality below 100 | The value reaches `1504d8`, labelled `enroll_score_threshold` at `735a`. Equating this with an individual image-quality score requires the downstream decision rule, not just the name. |
| NonFingerDetect=0 proves that liveness detection is disabled | The cited `91539` is inside the string `focal_SetNonFingerDetectVer`, not a parameter address. `d9f9→d9fb→241a0` supplies detection version zero; `d923/d92c` separately configure enrollment and verification non-finger detection. This does not prove that all non-finger detection, or liveness detection, is disabled. |

The sample-count interpretation is corroborated by Microsoft's
[WINBIO_EXTENDED_ENGINE_INFO definition](https://learn.microsoft.com/en-us/windows/win32/secbiomet/winbio-extended-engine-info),
which specifies the required good enrollment samples for each region.

Mayflower source paths, function names and diagnostic strings are present
(for example `1775b` and `1bba5`). Such strings identify implementation leads;
they do not constitute embedded source text or a complete reconstruction of
the BRISK/MFS matching and quality decisions. None of these values is imported
into the independent Linux matcher on the strength of a name alone.

<a id="设备配置四条路径共用"></a>

## Device configuration shared by all four paths

Source section: `Biometric_Device_AddReg`.

| Line | Path | Name | Type | Declared value |
| --- | --- | --- | --- | --- |
| 232 | H | DeviceCharacteristics | DWORD | `0x0100` (256) |
| 233 | H | Security | SZ | `D:P(A;;GA;;;BA)(A;;GA;;;SY)` |
| 234 | H | Exclusive | DWORD | 1 |
| 235 | H | SystemWakeEnabled | DWORD | 1 |
| 236 | H | DeviceIdleEnabled | DWORD | 1 |
| 237 | H | UserSetDeviceIdleEnabled | DWORD | 1 |
| 238 | H | DefaultIdleState | DWORD | 1 |
| 239 | H | DefaultIdleTimeout | DWORD | 5000 |
| 240 | H\WDF | WdfDirectedPowerTransitionEnable | DWORD | 1 |

These are Windows device-installation and power-policy settings, not an SPI register table. They do
not establish Linux reset delays or GPIO polarity. Semicolons inside the `Security` string must be
retained, not mistaken for INF comment delimiters.

<a id="winbio-插件配置四条路径共用"></a>

## WinBio plug-in configuration shared by all four paths

Source section: `DriverPlugInAddReg`.

| Line | Path | Name | Type | Declared value |
| --- | --- | --- | --- | --- |
| 243 | H | Exclusive | DWORD | 1 (duplicates line 234) |
| 244 | H\WinBio\Configurations | DefaultConfiguration | SZ | `"0"` |
| 245 | H\WinBio\Configurations\0 | SensorMode | DWORD | 1 |
| 246 | H\WinBio\Configurations\0 | SystemSensor | DWORD | 1 |
| 247 | H\WinBio\Configurations\0 | SensorAdapterBinary | SZ | `ftWbioSensorAdapter.DLL` |
| 248 | H\WinBio\Configurations\0 | EngineAdapterBinary | SZ | `ftWbioEngineAdapter.DLL` |
| 249 | H\WinBio\Configurations\0 | StorageAdapterBinary | SZ | `ftWbioStorageAdapter.DLL` |
| 250 | H\WinBio\Configurations\0 | DatabaseId | SZ | `91CF558A-2540-4C3D-9A85-4AD392FDE4DA` |

`DefaultConfiguration` is the string `"0"`, not DWORD 0. The GUID is a configuration identifier, not
a chip ID.

<a id="winbio-数据库配置四条路径共用"></a>

## WinBio database configuration shared by all four paths

Source section: `DatabaseAddReg`; every path is `D`.

| Line | Name | Type | Declared value |
| --- | --- | --- | --- |
| 253 | BiometricType | DWORD | `0x00000008` (8) |
| 254 | Attributes | DWORD | `0x00000001` (1) |
| 255 | Format | SZ | `CDAE92F1-5B32-4a91-94A8-56ACA204B3B9` |
| 256 | InitialSize | DWORD | `0x00000020` (32) |
| 257 | AutoCreate | DWORD | 1 |
| 258 | AutoName | DWORD | 1 |
| 259 | FilePath | SZ | Empty string |
| 260 | ConnectionString | SZ | Empty string |

An empty string is an explicit declared value, not missing data.

<a id="usb-独有的两项"></a>

## The two USB-only values

Source section: `usb_device_include`, referenced only by `.NT.hw` on the two USB installation paths
(lines 46 and 71).

| Line | Path | Name | Type / operation | Declared value |
| --- | --- | --- | --- | --- |
| 93 | H | LowerFilters | MULTI_SZ, append if absent | `WinUsb` |
| 94 | H | WinUsbPowerPolicyOwnershipDisabled | DWORD, set | 1 |

Line 93 does not replace the entire filter list with a single `WinUsb` entry. The final list depends
on existing contents and other installation operations. These values describe Windows USB-stack
differences, not differences in sensor-chip initialization.

<a id="addreg-之外的安装差异及外部依赖"></a>

## Installation differences and external dependencies beyond AddReg

All repeated Include/Needs directives in each section are retained. The following table lists
dependencies as sets per section; adjacent source lines are not assumed to form one-to-one pairs.
The external files are absent from this CAB, and system INFs from the analysis computer were not
substituted for those of the sample's target Windows version.

| Installation path / section suffix | Include files | Needs sections |
| --- | --- | --- |
| SPI / `.NT`, `.NT.hw`, `.NT.Services` | None | None |
| USB / `.NT` | WINUSB.INF | WINUSB.NT |
| 23H2_SPI / `.NT` | WUDFRD.INF | WUDFRD.NT |
| 23H2_SPI / `.NT.hw` | WUDFRD.INF | WUDFRD.NT.HW |
| 23H2_SPI / `.NT.Services` | WUDFRD.INF | WUDFRD.NT.Services |
| 23H2_USB / `.NT` | WINUSB.INF, WUDFRD.INF | WINUSB.NT, WUDFRD.NT |
| 23H2_USB / `.NT.hw` | WUDFRD.INF, WINUSB.INF | WUDFRD.NT.HW, WINUSB.NT.HW |
| 23H2_USB / `.NT.Services` | WUDFRD.INF, WINUSB.INF | WUDFRD.NT.Services, WINUSB.NT.Services |

The ordinary USB `.NT.hw` / `.NT.Services` sections have no local Include/Needs directives. Source
lines and the complete dependency list are in the JSON. Under the
[Microsoft DDInstall documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-ddinstall-section),
Needs names system-INF sections that must also be processed during installation. Without those
files, the dependency operations cannot be fully expanded.

- Ordinary SPI explicitly adds service `WUDFRd`; ordinary USB explicitly adds `WUDFRd`, `WinUsb`,
  and `focalFpSrvcDeamon`. The two 23H2 paths obtain framework services through system INFs, and
  23H2 USB additionally declares `focalFpSrvcDeamon`. An EXE appearing in a copy list does not prove
  that the SPI installation installs that service.
- Both USB paths specify `UmdfDispatcher=WinUsb`. Both SPI paths and 23H2 USB specify
  `UmdfDirectHardwareAccess`, `UmdfFileObjectPolicy`, and `UmdfImpersonationLevel`; ordinary USB
  does not explicitly set those three values. An omitted declaration does not imply the opposite
  runtime value.
- All four paths specify `UmdfLibraryVersion` `2.15.0`. The UMDF service binary path changes from
  `%12%\UMDF\ftWbioUmdfDriverV2.dll` in ordinary installations to `%13%\ftWbioUmdfDriverV2.dll` in
  23H2 installations. The USB auxiliary-service EXE path changes correspondingly. DIRID placeholders
  are retained; no absolute system path is assumed.

Identical AddReg values between Windows target versions do not erase these differences. This report
does not simulate registry writes caused by service installation or WDF directives.

<a id="验证结果与下一步"></a>

## Validation results and subsequent work

All five AddReg definition sections were checked: 17 vendor parameters, 9 device settings, 8 plug-in
settings, 8 database settings, and 2 USB additions. Extraction preserves repeated AddReg references
and checks string expansion, hexadecimal conversion, empty strings, multistring append operations,
and the security descriptor containing semicolons. The four paths contain 172 references. Local
value sets are identical between Windows target versions; the USB–SPI difference is exactly the two
values listed above. No hardware tests or driver-code changes were part of this step.

**This INF contains no identified computer-model-specific registry values, chip-selection keys, GPIO
routing, or firmware/initialization-table selection keys.** Such differences may still exist in
other packages or runtime logic.

The next stage of the original research plan was static binary analysis: record file hashes and PE
metadata, locate readers for these 17 settings and chip-detection branches, verify the known FT9361
firmware's offset, length, and hash, and trace initialization-sequence selection. A string match
alone does not prove that a setting is used, and a candidate binary region is not automatically
confirmed firmware.

Subsequent work confirmed the main hardware-adaptation mechanisms; see
[Windows runtime adaptation](windows-runtime-adaptation.md). That work covers resource binding, chip
detection, and firmware selection. The complete read-side data flow of the 17 vendor parameters has
not been reconstructed individually.
