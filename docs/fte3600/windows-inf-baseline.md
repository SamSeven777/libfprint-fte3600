# Windows INF analysis: the first baseline sample

[Documentation index](README.md)

<a id="windows-inf-分析第一份基线样本"></a>

Analysis date: 2026-10-04. Scope: the single driver package referenced by
`scripts/install-firmware.sh`. This report is not an inventory of every FTE3600 driver version or
computer model. This step examined the INF only; it did not execute or install the Windows driver or
modify libfprint.

<a id="来源与可复核标识"></a>

## Source and reproducible identifiers

- [Original Microsoft-hosted CAB download](https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2026/08/e684f740-91ac-4458-9097-09850eaedf9d_4f80a6cb0c9e453d4619667af7c92eadeb165e0f.cab)
- CAB SHA-256: `2c3380810f40ca2152fdce3f0f237441521f65fee80cbe802a2def4ca01d6cb5`
- INF: `ftWbioUmdfDriverV2.inf`
- INF SHA-256: `b64c8ebe2ec65f47996fa8f27e8f68f9c91076b7cb915839aafde51f6240c1dd`
- INF line 11 declares driver date `07/05/2025` and version `2.0.3.102`. This is the date declared
  by the INF, not the download date; the year and month in the download URL do not determine the
  driver version date either.
- Provider: `FocalTech Electronics(ShenZhen)Co.,Ltd` (lines 9 and 264).
- Class: `Biometric`; declared catalog file: `ftWbioUmdfDriverV2.cat` (lines 7–10). This step did
  not verify the catalog signature or member signatures.
- The original local sample is stored under workspace `work/windows-driver-baseline/`, outside the
  source repository. This report contains only analytical findings and locations.

The CAB contains 7 files: one INF, one CAT, `ftWbioUmdfDriverV2.dll`, `ftWbioEngineAdapter.dll`,
`ftWbioSensorAdapter.dll`, `ftWbioStorageAdapter.dll`, and `focalFpSrvcDeamon.exe`. There is no
separately named firmware file; firmware embedded in a binary is still possible.

### Later daemon cross-check: 2026-10-05

The package's `focalFpSrvcDeamon.exe` is 296,968 bytes, SHA-256
`ab44e8c505e9dd6d627ec2df0730700ebbcb2c284124e0cce1d82a9214e34033`.
This later binary check supplements the original INF-only scope.

RVA `2290` enumerates biometric device nodes; `2392/23a8` filters for
`VID_2808` or `VEN_FTE`. `23e0` queries device-node status, and the error-code
mask selects 10, 31, 37 and 43. `2413/242d` call helper `2130` to disable and
then enable the selected node through `DIF_PROPERTYCHANGE`. This confirms a
device-error recovery service rather than merely a copied executable.

A separate branch logs "reboot usbhub" at `249a`, searches for `VID_0000`
at `2578`, and calls the same device-node disable/enable helper at
`25b2/25cd`. The log does not prove a reset of the entire USB hub or a physical
sensor reset pulse. This does not establish a need for an equivalent service
on the Linux ACPI/SPI path.

<a id="完整设备映射仅限此-inf"></a>

## Complete device mapping in this INF

Line 15 of `[Manufacturer]` references two target-decorated versions of `Standard`. The following
are all active entries, excluding comments. Under the
[Microsoft INF Models syntax](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-models-section),
the first ID after the install section is the hardware ID; any later IDs are compatible IDs. None of
these entries has an additional compatible ID.

| INF line | Models section | Hardware ID | Install-section base name | Main install section |
| --- | --- | --- | --- | --- |
| 18 | `Standard.NTamd64` | `ACPI\FTE3600` | `SPIdevice_Install` | `SPIdevice_Install.NT` |
| 19 | `Standard.NTamd64` | `USB\VID_2808&PID_9338` | `USBdevice_Install` | `USBdevice_Install.NT` |
| 22 | `Standard.NTamd64.10.0...22631` | `ACPI\FTE3600` | `23H2_SPIdevice_Install` | `23H2_SPIdevice_Install.NT` |
| 23 | `Standard.NTamd64.10.0...22631` | `USB\VID_2808&PID_9338` | `23H2_USBdevice_Install` | `23H2_USBdevice_Install.NT` |

There are 4 mappings and 2 unique hardware IDs. Both Models sections target AMD64; the second has
the `10.0...22631` decoration. `23H2_` is the vendor's install-section naming convention, not
another hardware model. All device display names resolve to `FocalTech Fingerprint reader` (lines
268–269).

`ACPI\FTE3600` uses the SPI installation path; the USB ID uses WinUSB. These declarations describe
this package's binding scope. They do not identify the USB device as FT9361 or prove that all
devices sharing the ACPI ID use the same chip, pin layout, or firmware.

<a id="为第二步建立的注册表追踪入口"></a>

## Registry references for the next analysis step

This section records references without assigning hardware meanings to registry values that had not
yet been analyzed at this stage.

| Installation path | Main-section AddReg | `.NT.hw` AddReg |
| --- | --- | --- |
| SPI, both target versions | `FTFP_AlgInfo.AddReg` | `Biometric_Device_AddReg`, `DriverPlugInAddReg`, `DatabaseAddReg` |
| USB, both target versions | `FTFP_AlgInfo.AddReg` | The same three sections, plus `usb_device_include` |

Locations: USB lines 37–90; SPI lines 97–139; registry definitions at lines 92–94, 209–228, and
231–260. Repeated `AddReg=` directives in one section must all be retained. A conventional
single-value INI dictionary would incorrectly overwrite earlier entries.

This INF has no Models/AddReg branches for A1, GPD Pocket 3, Medion E3224, or other computer models.
It also contains no directly extractable GPIO pin mapping, SPI initialization-register sequence, or
firmware-selection table. The explicit configuration branches observed here concern transport and
Windows target version. INF analysis alone cannot establish whether model differences come from
other OEM INFs, ACPI resources, runtime chip detection, or DLL logic.

The second step's local registry extraction is complete; see the
[registry differences report](windows-registry-baseline.md) and
[structured inventory](windows-registry-baseline.json). They record roots, subpaths, names, type
flags, original values, expanded strings, source lines, applicable installation paths, and `Include`
/ `Needs` dependencies. External system INFs have not been expanded. This INF alone cannot establish
the complete registry state after installation.

<a id="与现有-linux-实现的关系"></a>

## Relationship to the Linux implementation

At the time of this initial INF analysis, the device table in `libfprint/drivers/fte3600.h` matched
`FTE3600` and the image parameters were FT9361's 64 × 80. Those were properties of the early Linux
implementation, not evidence that this INF identified that chip. The implementation has since
expanded; see [current status](status.md) and the
[architecture](architecture.md).

The research sequence was registry comparison, followed by locating firmware and initialization data
with their offsets, lengths, hashes, selection conditions, and evidence, then extending libfprint
from confirmed technical specifications. The subsequent reports are linked above and in
[runtime adaptation](windows-runtime-adaptation.md). Both technical specifications and independent
implementations must retain provenance. Extracting firmware does not make it open source or grant
redistribution rights. Vendor binaries remain outside the source repository under the boundaries in
[clean-room provenance](clean-room.md).
