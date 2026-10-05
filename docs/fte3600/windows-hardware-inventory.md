# FTE3600 hardware and implementation inventory

[Documentation index](README.md)

<a id="fte3600-硬件与处理逻辑计数"></a>

Verification date: 2026-10-04. The samples are three x64 packages from Microsoft Update Catalog:
**2.0.3.99, 2.0.3.100, and 2.0.3.102**. Counts cover these samples only, not every vendor chip, OEM
package, or laptop model on the market. Package sources, hashes, factory references, method
addresses, and firmware ranges are in the [reproducible inventory](windows-hardware-inventory.json).
The repository contains no vendor binaries, disassembly, or firmware contents.

<a id="数量结论"></a>

## Confirmed counts

| Object counted | Confirmed count | Meaning and limits |
| --- | ---: | --- |
| Complete INF PnP IDs | **2** | `ACPI\FTE3600` and `USB\VID_2808&PID_9338`; identical in all three versions |
| Bus-adaptation layers | **2** | SPI and USB; the ACPI ID does not distinguish individual sensors |
| Sensor backends instantiated by the version 102 factory | **8** | The factory actually references 8 constructors; this is not merely a search for 8 names |
| Version 102 operation-table groups | **6** | Compare each object's 20-slot table, omit the destructor slot, and group by exact address equality across the remaining 19 slots |
| Located response categories in the version 102 main detection path | **9** | 4 runtime-register pairs and 5 special IDs; these are not equivalent to 9 physical chips |
| Low-level `ft93xx` acceptance table | **10 IDs** | No separate main-factory mapping was found for 7 of them; their presence does not establish support |
| Associated embedded firmware ranges | **6 ranges for 5 backends** | FT9368 has separate app and pramboot ranges; all six are byte-identical across the three samples |
| Total computer models and ACPI wiring combinations | **Not determinable from these packages** | Physical-device ACPI tables have not all been collected, and the INF has no per-model inventory |

The verifiable answer is: **version 102 contains 8 sensor backends, grouped into 6 sets by shared
operation methods; the total number of computer hardware models remains unknown.** 6 is a specific count of code-structure groups, not a claim that initialization, transport, startup-state, and
recovery logic together have only 6 branches.

<a id="8-个后端如何归为-6-组"></a>

## How the 8 backends form 6 groups

| Operation group | Backend labels | Located detection entry | Shared behavior and differences |
| --- | --- | --- | --- |
| 1 | FT9338, FT9536 | Runtime `5858`, `4080`; also boot/OTP branches | All 19 operation slots are identical, but firmware and object parameters differ |
| 2 | FT9348, FT9361 | Runtime `6060`, `4050`; distinguished by A8-family OTP | All 19 operation slots are identical, but their firmware is not interchangeable |
| 3 | FT9368 | Dedicated detection/recovery entry | Separate method table; app and pramboot firmware ranges |
| 4 | FT9369 | Main-path response `0x9362` → internal type 9 | The object name is not the returned ID; this must not be described as reading `0x9369` |
| 5 | FT9365 | Response `0x9365` → internal type 12 | Shares some `ft93xx` initialization/ID-reading methods with FT9769, but not the complete method table |
| 6 | FT9769 | Response `0x9391` or `0x9392` → internal type 11 | Both responses select one backend; its final two operation slots differ from FT9365 |

The version 102 factory is at RVA `0x23690`. FT9365 and FT9769 share initialization and ID-reading
wrappers at `0x383D0` and `0x383E0`, calling low-level `0x196B8` and `0x19978`. Their final method
slots, at offsets `0x90` and `0x98`, still differ. Sharing initialization is insufficient to merge
the entire backend; conversely, identical low-level initialization need not be duplicated.

The original driver also uses the four `0x14/0x15` runtime pairs as sensor x/y dimensions. They are
response signatures, not proven immutable silicon identities. The 9 response categories are `5858`,
`6060`, `4050`, `4080`, `9368`, `9362`, `9365`, `9391`, and `9392`.

<a id="底层还出现的-10-个-id"></a>

## The 10 additional low-level IDs

The comparison loop at version 102 RVA `0x191A8` checks 10 values:

`9391`, `9392`, `9395`, `9396`, `9397`, `9398`, `9363`, `9372`, `9349`, `9365`.

This is supported by control flow, not merely log strings. The ID-reading path at `0x19978` uses
this check and can further classify `9391` as `9395` according to a register condition. Only `9365`,
`9391`, and `9392` have located direct dispatches in the main startup path. The other **7 IDs** may
serve internal variants, other entry points, or shared components; the available evidence does not
establish all their uses. They must not simply be added to the 8 class names to claim support for 15
or 16 physical chips.

<a id="版本差异已确认到什么程度"></a>

## Confirmed version differences

| Sample version | PnP IDs | Backends | Operation groups | Located firmware ranges |
| --- | ---: | ---: | ---: | ---: |
| 2.0.3.99 | 2 | 6 | 4 | 6 |
| 2.0.3.100 | 2 | 6 | 4 | 6 |
| 2.0.3.102 | 2 | 8 | 6 | 6 |

Versions 99 and 100 contain FT9338, FT9348, FT9361, FT9368, FT9369, and FT9536. Version 102 adds
factory backends FT9365 and FT9769. The **44 explicit registry-write lines are identical** in all
three INFs, as are the six located firmware ranges. Evidence for this version expansion therefore
points to added runtime implementations, rather than new per-model registry settings or replacement
FT9361 firmware.

Versions 99 and 100 have different `.text` hashes, so they cannot be treated as differing only in
version number. This analysis did not establish function-by-function equivalence or cover every OEM
repackaging and private version. Microsoft Catalog search entries include different OS,
classification, and publication records; their count does not measure computer models or independent
protocols.

<a id="固件与初始化资料"></a>

## Firmware and initialization data

The following ranges in the version 102 main DLL were located through constructor/download
associations. They are location metadata, not firmware contents. Complete SHA-256 hashes are in the
JSON.

| Object / purpose | RVA | File offset (decimal) | Size (bytes) |
| --- | --- | ---: | ---: |
| FT9338 | `0x731A0` | 465312 | 14184 |
| FT9348 | `0x76910` | 479504 | 10312 |
| FT9361 | `0x79160` | 489824 | 10396 |
| FT9368 app | `0x7BA00` | 500224 | 27120 |
| FT9368 pramboot | `0x823F0` | 527344 | 6096 |
| FT9536 | `0x83BC0` | 533440 | 11934 |

The corresponding constructor fields for FT9365, FT9369, and FT9769 do not specify this kind of
firmware range. That does not mean these chips need no initialization or prove that the driver
contains no other embedded data. Initialization may consist of host-side register operations and
parameters; not every object can be classified as loading one binary file. **The total number of
initialization tables, complete register semantics, and every failure-recovery path have not all
been established.**

<a id="原待确认清单的逐项结论"></a>

## Findings against the original open questions

This table records the scope of the original inventory. Later Linux implementation work is linked
below; the original state-machine gaps are not a current claim that those backends remain
unimplemented.

| Original question | Confirmed by this inventory | Evidence still missing at that stage |
| --- | --- | --- |
| ACPI descriptions and polarity for every computer model | Windows obtains connections from system resources; Linux can map controllers, pins, and IRQs from ACPI resources without a DMI whitelist | Each physical device's `_CRS/_DSD`, resource roles, and measured levels; GpioIo has no polarity field from which reset polarity could be inferred |
| All cold-start/recovery protocols | Runtime, boot-A, boot-B family detection, OTP, and special-chip entries were separated; a minimal independent FT9361 ROM detector existed | Complete state machines for other backends, all failure/retry conditions, and hardware traces of cold start and resume |
| Other chips' firmware/initialization tables | 6 firmware ranges for 5 backends and 6 operation groups were located | All initialization tables and parameter semantics, and complete register flows for the remaining backends |
| Version/OEM differences | INFs, factories, method tables, and the listed firmware ranges were compared across three public versions | OEM-specific packages and the full historical version set; three samples cannot exclude all OEM exceptions |
| Success rate on physical devices | No target sensor was connected for this inventory | Static analysis and simulations cannot establish compatibility across the entire family |

<a id="linux-实现如何使用这些结论"></a>

## Applying these findings in Linux

The implementation shares ACPI resource handling and a discovery state machine, then selects
protocol backends and data by chip capability. Shared methods can be reused; chip-specific firmware,
OTP classification, and parameters must remain distinct. Vendor version forks, redundant retries,
and default classifications of unknown values need not be copied.

The current implementation has removed the DMI pin whitelist. FT9361 firmware loading requires
ROM-family and OTP identity checks; unknown or unimplemented chips are rejected explicitly. **Linux
now has image-capture implementations for these 8 chips, but implementation, passing software tests,
and physical-device validation are separate levels of evidence.** FT9365/FT9769 use dedicated
exposure calibration and image qualification, FW9369 uses an unloaded baseline and FDT calibration,
and FT9368 uses its own application protocol. Cold recovery, authentication, and hardware limits are
documented in the [current support matrix](status.md#implemented-functions-and-test-limits),
[legacy38 recovery](legacy38-recovery.md), and [dynamic discovery](dynamic-discovery.md).

Source:
[Microsoft Update Catalog search](https://www.catalog.update.microsoft.com/Search.aspx?q=ACPI%5CFTE3600).
Exact download URLs and SHA-256 hashes for all three packages are retained in the accompanying JSON.
