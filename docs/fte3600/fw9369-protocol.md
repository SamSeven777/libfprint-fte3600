# FW9369 protocol: hardware returning 9362

[Documentation index](README.md)

<a id="fw9369--返回-9362-的硬件协议研究"></a>

Based on `ftWbioUmdfDriverV2.dll` from the Windows package with INF version 2.0.3.102. The DLL's PE
FileVersion / ProductVersion is 1.0.0.3188, and its SHA-256 is
`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`. This document records
independently verified hardware-operation contracts, without vendor implementation code, firmware,
or initialization tables. RVAs refer to this DLL. Hardware protocol and independent Linux policy are
described separately; static evidence and simulated tests do not establish physical-device image
quality.

<a id="身份与传输"></a>

## Identity and transport

The backend constructor is at `0x35BA4`, with operation table `0x49410`. The object is named FT9369,
but detection/communication checks explicitly require internal address `1A8B` to return `9362`
(`0x10088`, `0x10CD8`). The name is not the chip ID. Image-processing parameters and row stride at
`0x15938` directly establish 64 × 80 pixels. SPI CS polarity depends on the platform connection, not
this ID; the host must preserve confirmed connection parameters.

| Operation | Complete TX | Valid RX position | Evidence |
| --- | --- | --- | --- |
| SFR read | `08 F7 reg 00 00`, 5 bytes | 1 byte at offset 4 | `0x1830C` → `0xF5D0` |
| SFR write | `09 F6 reg value`, **4 bytes** | None | `0x18368` → `0xF738` |
| Word read | `04 FB (addr_hi OR 80) addr_lo 00 01`, followed by 6 bytes of clocks; 12 bytes total | 2-byte big-endian value at offset 6 | `0x183B0` → `0xF748` |
| Word write | `05 FA (addr_hi OR 80) addr_lo 00 01 value_hi value_lo`, 8 bytes | None | `0x18550` |

Word addresses and SFR registers occupy different address spaces. `0x183B0` does not validate the
final four additional response bytes of a word read; their meaning or checking algorithm must not be
invented. SFR writes in this protocol have no trailing dummy byte, so A8's five-byte write packet
cannot be reused directly.

SPI configuration at `0xFD94` writes SFR `C6 = 01`, waits 4 ms, then reads `C6`, requiring 01.
Failure allows up to 30 retries, or 31 attempts in total. `0x23010` waits in milliseconds, not
microseconds. `0x10A74` calls this configuration, then `0x10D70` configures it again and reads
`1A8B`.

Linux first attempts repeated, consistent positive ID reads. If read-only discovery fails, the
separately bounded [special-mode probe](special-probe.md) can perform the documented C6 negotiation.
This is a deliberate state-changing fallback, not a claim that C6 is a harmless write for every
unknown device. Chip-specific initialization still requires a confirmed identity.

<a id="电源状态与命令"></a>

## Power, status, and commands

`0x186B0` translates operation numbers to short commands, usually 3 bytes. Commands are followed by
1 ms, except wake-end, which adds no such wait.

| Intent | Command / behavior | Evidence |
| --- | --- | --- |
| Return to idle | `C0 3F 00`, wait 1 ms; `C1 3E 00`, wait 1 ms | `0x10BD0` |
| Wake | `5A A5 00`, wait 1 ms; read SFR 80; if it is not 50, send `C0 3F 00`, wait 1 ms; finish with `A5 5A 00` | `0x10F0C` |
| Query device status | SFR 80 | `0x110E8` |
| Enter image mode | After image configuration, send `C4 3B 00`, wait 1 ms, query SFR 80, expect 54 | `0x17000` |
| Start one scan | In image mode, set bit 0 of word 1800, then wait 1 ms | `0x17000` |

Additional verification: the power-mode parameter 3 branch at `0xFBA0` is explicitly labeled deep
sleep. `0xFC3F → 0x11048` first performs the wake sequence above, then sends `C1 3E 00` and waits 1
ms. This directly establishes a deep-sleep use of C1, but not the state value, register
accessibility, or automatic IRQ clearing after C1. `0x10BD0` implements device idle mode; the
complete chain from every public user close to this mode has not been established. The existence of
idle/deep-sleep operations must not be described as proof that every close sends C1. D0Exit can even
conditionally enter WAIT_TOUCH; see [lifecycle coverage](windows-lifecycle-coverage.md).

Complete software initialization at `0xFED8` includes ID validation, wake, OTP-information reads,
manufacturing-process identification, interrupt clearing, parameter initialization, FDT-baseline
stabilization, and image-baseline calibration, then enters wait-for-touch. The last two steps cannot
be replaced by one fixed image-read packet.

SFR `9B >> 2` identifies the manufacturing process (`0x10D8C`). Hexadecimal 00 and 13 are accepted;
other values are reread up to 10 times. Process 13 defaults image integration to 150, and process 0
to 200 (`0x10BEC`). This is not a computer-model whitelist.

<a id="中断合同"></a>

## Interrupt contract

Word `1A82` contains event flags, word `1A83` the event mask, and writing event bits to `1A84`
clears the corresponding flags. Evidence: `0x11008`, `0x11014`, `0x10FC0`. Events form a bitmap and
must not be compared as a single enumeration value.

| Mask | Event |
| --- | --- |
| 0001 | Idle |
| 0002 | Finger down |
| 0004 | Finger up |
| 0008 | Manual detection |
| 0010 | Invalid |
| 0020 | Image data |
| 0040 | AFE |
| 0080 | Half FIFO |
| 0100 | Full FIFO |
| 0200 | Reset |
| 0400 | ESD |

`0x101EC` validates communication before reading and clearing events; a touch event enables
image-data events. Reset / ESD requires reinitialization and must not be treated as a valid
image-ready IRQ. A bounded Linux implementation should return to verified idle or close the session
after an error, rather than retry calibration indefinitely.

`0x101EC` also wakes and retries after a communication-check failure. Linux now writes C6=01 before
action-time event reads, waits 4 ms, and validates 1A8B, requiring three consecutive 9362 responses.
Blank 0000/FFFF responses trigger bounded wake recovery, with 20 ms after each wake and at most 10
rounds. Transfer errors fail immediately. A positive conflicting ID immediately invalidates the
session and calibration and prevents further chip-cleanup writes. This recovery does not replace
recalibration after ESD.

The Linux wake sequence includes C0, which stops the previous FDT operation. Once communication
recovers, existing events are read and acknowledged first. If the event is empty or irrelevant to
the current down/up wait, the implementation preserves the detection mode and baseline and resends
only C2 before continuing to wait. It does not clear events again or drain host IRQs, preserving
events that arrive during recovery. A restart failure or cancellation takes the normal bounded
cleanup path instead of entering an unarmed wait.

Windows passive ISR `0x2DDEC` handles chip events before checking whether a capture request exists.
With no request, a touch event enters WAIT_LEAVE through `0x2DEFA → 0xF8C4(2)`. Windows therefore
cannot be assumed to ignore all IRQs when no capture is requested. Linux currently handles events
only during actions and releases transport resources on close; its lifecycle differs from the
vendor's.

<a id="图像与像素格式"></a>

## Images and pixel format

A raw image contains **64 × 80 big-endian 16-bit samples**, totaling 10240 data bytes. `0x18208`
reads from FIFO word address `1A05`; `0x180F4` encodes length in words, and `0xF464` assembles the
full SPI transaction. A single-frame request is:

`06 F9 9A 05 14 00` + 10240 dummy bytes.

The complete transaction is **10246 bytes**, with response data at offset **6**. It has none of the
A8 image protocol's extra two-byte offset. `0x15938` explicitly converts each byte pair to one
big-endian sample. The read flow at `0x16898` is image-mode initialization → scan start → FIFO read
→ clear data event.

The vendor's 8-bit output is not simply the high byte of each raw sample. Required inputs include
the current no-finger baseline: a nonnegative per-pixel difference `max(baseline - sample, 0)` is
followed by edge/bad-pixel processing and normalization. This document records inputs and physical
data meaning without copying the vendor's processing functions. Linux can implement its own bounded
integer conversion. Synthetic tests must verify big-endian parsing, complete frame size, subtraction
without underflow, constant images, and output range. Without baseline and acquisition-configuration
evidence, one FIFO read cannot be declared a validated usable fingerprint image.

<a id="图像模拟前端配置"></a>

## Image analog-front-end configuration

Image-mode entry `0x168C8` and scan-clock setup `0x156A4` establish the following register contract.
These are individually verified addresses, fields, and values, not an initialization-data region
copied from the binary. Unlisted fields retain their current hardware values. Register values below
are hexadecimal unless stated otherwise.

| Word address | Field / written value | Meaning and source |
| --- | --- | --- |
| 1801 | `FC80 OR DAC`, with a 7-bit DAC | Analog bias; `0x16921` |
| 1800 | 4FFE; set bit 0 for scanning | Image scan control; `0x169EE`, `0x1705E` |
| 1804 | 27CA | Image sampling configuration; `0x16B52` |
| 1806 | bits 13:7 = 9 | 2M scan clock; `0x156DC` |
| 180A | bits 13:7 = 9, bits 6:0 = 3 | `0x15708` |
| 180B | bits 13:7 = 4, bits 6:0 = 8 | `0x15749` |
| 1807 | `(integration - 1) << 5 OR 1` | Integration is DB 200 or SMIC 150; `0x16C2E` |
| 1887 | 0002 | Default single-group channel mode; `0x16CF5` |
| 1805 | bit 4 = 0, bits 7:5 = 0 | Shared analog configuration; `0x16DC3` |
| 1811 | bits 9:0 = 01FE | `0x16E79` |

`0x16F17` enables events 0020 and 0040, then writes F4 to SFR 8E. The timer uses SFR 90=00, 91=07,
92=D0, 90=01 (2000, `0x11098`). After the first image-mode call, Windows uses host-side caching to
omit some rewrites. To avoid cache desynchronization, Linux explicitly resets known fields on every
mode switch and reads back configuration fields; trigger bits and W1C flags are excluded from this
verification.

<a id="手指检测与校准"></a>

## Finger detection and calibration

The default host configuration at `0x10BEC` selects **4-channel** FDT. The program also has an
8-channel branch, outside the current backend's implementation. Default FDT DAC is 27 and image DAC
54, both decimal. `0x1120C` describes the normal FDT DAC range as 1–125 and adjusts toward average
response 512. `0x149D4` takes the median of `raw / 4` over the image's interior, also adjusting the
image DAC toward 512. The samples' 16-bit container therefore does not imply a calibration target of
65535.

FDT-mode entry is `0x12F20`. The default 4-channel configuration has the following field contract:

| Word address | Field / written value | Evidence |
| --- | --- | --- |
| 1801 | `FC80 OR FDT_DAC` | `0x12F8A` |
| 180C | bits 10:0: 0 during DAC search; 0600 afterward | `0x1304C`–`0x13189` |
| 1881 | Period bits 15:8 = 15; count bits 4:2 = 3 | `0x132B5`–`0x13335`, 60 Hz integer configuration |
| 1800 | 07FE | `0x133D3`–`0x1343C` |
| 1804 | 27C8 | `0x134BE`–`0x134EE` |
| 1807 | 1671 | `0x13571`–`0x1358E` |
| 1808 | 0801 | `0x1360E`–`0x1365D` |
| 1887 | bits 2:0 = 5 | `0x136E0`–`0x13715` |
| 1806 / 180A / 180B | Clock fields respectively 19; 19 and 7; 9 and 17 | `0x110F0`; field widths match image mode |
| 1805 | bit 4 = 0 | `0x137D7` |
| 180D | Start with the just-read **1805** value with bit 4 cleared; set bits 9:0 = 900 | `0x13881`; 900 is decimal |
| 1888 | bits 9:2 = 0 | `0x13928` |
| 00C0 | bits 12:0 = 0444 | `0x139EC`; access after unlocking SFR 9A=5A |
| 00C1 | bits 5:3 = 4, bits 2:0 = 1 | `0x13A95` |
| 00C2 | bits 7:6 = 3 | `0x13B65`; then relock SFR 9A=00 |
| 1880 | 2D32: release-difference threshold 45, down-difference threshold 50 | `0x13E7A` |
| 1881 | bit 0 = 0, bit 1 = 1, bit 7 = 1 | `0x13F45`, manual detection |
| 1884 | bits 2:0 = 3 | `0x14010` |
| 1A8A | 00FF | `0x140C1` |

Manual sampling at `0x128EC`: finish FDT configuration and set 1881 bit 1, send `C2 3D 00`, wait 1
ms, write word 1885=0001, then poll 1A82 for 0008. The vendor waits up to 5 times, 1 ms each; Linux
fails if not ready by the limit. `0x122C4` reads four big-endian words through `0x18434`:

- DB: `04 FB 80 B8 00 04` + 8 dummy bytes, 14 bytes total.
- SMIC: use address `80 E8`; the response contains four 16-bit values starting at offset 6.

`0x11C64` takes each channel's minimum across repeated stable samples; successive changes of no more
than 5 count as stable. `0x14954` requires each of the first four values to be within 300–700.
`0x11A54` subtracts 30 from the baseline, saturating values below 30 to zero. Automatic-detection
baseline writing at `0x11770` unlocks SFR 9A=5A, sends `05 FA 80 B0 00 08` + 16 baseline bytes (use
E0 for SMIC), then writes 9A=00. Default 4-channel mode uses only the first four words; the other
four stay zero.

Automatic wait-for-down at `0x11800` restores normal FDT configuration and writes the baseline; 1881
bit 0 = 1, bit 1 = 0, bit 7 = 1. It clears events 002F and sends C2. Release detection clears bits 0
and 7. During intermediate Linux enrollment stages, release detection is armed immediately after
image capture to preserve UP arriving during asynchronous matching. Waiting for release does not
clear that event again. Only UP without simultaneous DOWN permits the next down capture. The final
stage returns to idle; if the matcher rejects it, release detection is enabled as needed. A finger
leaving before detection is enabled, and the short final-stage retry window, still require hardware
validation. Re-enabling detection on an unloaded sensor is not assumed to generate UP immediately.

<a id="linux-独立实现与验证边界"></a>

## Independent Linux implementation and validation limits

`fte3600-fw9369-protocol.{h,c}` handles bounded packet construction, big-endian parsing, and
independent image mapping. `fte3600-fw9369.c` implements asynchronous, cancellable initialization
and capture state machines. DAC adjustment uses an independent bounded binary search, range 1–125,
with target response window 450–575. It does not copy the Windows search, bad-pixel analysis, or
image-enhancement algorithms. FDT calibration requires 3 consecutive stable measurements, with at
most 10 measurements. Image-baseline calibration requires mean absolute frame difference no greater
than 20, also with a bounded attempt count. Configuration preserves unrelated fields and verifies
readable fields after writing.

Image processing uses per-pixel saturated baseline subtraction, linear mapping at percentile 99, and border extension. Low-contrast or almost empty images are rejected to avoid
enhancing noise into apparent fingerprints. This mapping is Linux policy; its thresholds need
evaluation on real images and do not claim to reproduce vendor image quality.

The unloaded baseline is established during open: **the finger must be off the sensor when opening
it**. Stability checks cannot prove that a stable object is not a finger. This is a physical
limitation of the current calibration and a required hardware-acceptance scenario. Baselines are
never written to disk and are cleared on close. Unknown manufacturing process, conflicting ID,
unstable baseline, and reset/ESD/invalid events all prevent further capture.

Cleanup after each frame, cancellation, or recoverable error uses `C0 3F 00` followed by SFR 80=50,
retaining reusable awake idle. Idle is marked verified only after a successful status read; cleanup
failure requires close/reopen. Final close uses a separate `create_shutdown()`: relock 9A, send C0
and perform bounded status polling, then read-modify-write 1A83 to mask known event bits 07FF while
preserving other bits. Independently acknowledge pending events by writing 07FF to 1A84, then send
`C1 3E 00` and wait 1 ms. Failure at one step does not prevent attempts at the remaining
finalization steps. The first error is retained and transport resources are always released;
invalidated sessions send no further chip commands.

After C1, `idle_verified` is cleared. The implementation does not guess status 51 or append unproven
status reads to a possibly sleeping chip. Successful completion establishes that these commands
completed, not measured power consumption or IRQ behavior. This final-close policy uses known vendor
commands without claiming that Windows takes the same chain on every user close. This backend sends
no hardware resets, ROM commands, firmware downloads, or persistent-storage writes.

Independent synthetic tests cover full-frame sizes, byte order, capacity/invalid-command handling,
saturated subtraction, and empty-frame rejection, plus DB/SMIC initialization, repeated capture,
cancellation, SPI errors, and cleanup errors. Release regressions additionally cover UP alone,
simultaneous UP/DOWN, persistent DOWN, UP arriving before matching, pre-arm failure, and
cancellation. Communication-recovery regressions require FDT to be running at every IRQ wait. They
cover irrelevant/empty events, newly latched events during recovery, C2 restart failure, and
cancellation, with the same event sequences without wake as controls. Simulated hardware is not
additional protocol evidence. CS polarity, actual IRQ behavior, power consumption, and final image
quality still require physical validation on GPD and other devices.
