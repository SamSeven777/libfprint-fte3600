# Protocol evidence for FTE3600 multi-chip support

[Documentation index](README.md)

<a id="fte3600-多芯片实现所需的协议证据"></a>

Verification date: 2026-10-04. This document records wire-protocol facts suitable for independent
implementation and their limits. It contains no vendor code, disassembly, firmware bytes, or
initialization-data tables. Evidence comes from static control-flow analysis of the x64 Windows
package version 2.0.3.102; the sample SHA-256 is
`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`. All RVAs are relative to this
sample's image base, not file offsets. Package provenance, cross-version checks, and firmware hashes
are in the [hardware inventory](windows-hardware-inventory.json). **Static protocol evidence does
not replace hardware validation or establish uniform behavior across every OEM version.**

<a id="可以共用的-ft9348--ft9361-a8-协议"></a>

## Shared FT9348 / FT9361 A8 protocol

The chips share the same 19 non-destructor operation entries. That is only a research lead.
Constructor parameters, actual MCU configuration, SPI image reads, reset, and common work-mode
control flow were checked separately. Those findings support a parameterized A8 backend;
method-table equality alone would not justify enabling capture.

| Parameter | FT9348 | FT9361 | Evidence RVA |
| --- | --- | --- | --- |
| Runtime `14/15` response | `60 60` | `40 50` | `0x27290`; constructor parameters below |
| Image width × height | 96 × 96 | 64 × 80 | `0x35834`, `0x35914`; field orientation confirmed by `0x2D4A0` |
| Expected FW version | `30` | `30` | Same constructor parameters; read register `1A` |
| Expected AGC version | `31` | `31` | Same constructors; read register `3C` |
| Work-mode register | `76` | `76` | Shared constructor `0x392B8`, mode access `0x285CC` |
| Object wait parameter after reset | 160 ms | 160 ms | `0x35834`, `0x35914`; used at `0x39C50` |
| Firmware size | 10312 bytes | 10396 bytes | Respective constructors and download entry `0x39730` |
| Complete image SPI transaction | 9224 bytes | 5128 bytes | `0x2E990` → `0x2F020` |
| Image-data offset in complete SPI response | 8 bytes | 8 bytes | Same call chain |

Geometry fields are not stored in width-then-height order. Image-size use and output at `0x2D4A0`
establish that `+9` is width and `+8` is height; the low byte of a little-endian integer must not
simply be treated as width. FW/AGC values above describe the analyzed firmware and do not authorize
unknown versions automatically.

<a id="寄存器与帧格式"></a>

### Registers and frame format

All example values in this section are hexadecimal bytes, except lengths and times explicitly
expressed in decimal. SPI command mapping is at `0x22C78`; register wrappers are at `0x2F1B0` and
`0x2FC30`.

- Runtime one-byte register read: `10 EF reg 00`, then clock out the required response bytes.
- Runtime one-byte register write: `11 EE reg value 00`.
- MCU idle check: read two consecutive bytes from `20`; only `A5 5A` indicates idle. Evidence:
  `0x2853C`. Other responses mean busy or a protocol anomaly, not a specific chip identity.
- Finger status: read one byte from `1D`; `01` or `A0` permits capture. Evidence: `0x2841C`,
  conditional on the MCU being idle.

Normal SPI image reads use this exact format:

1. Let `N = width × height`, using overflow-safe host integer arithmetic.
2. Send `04 FB 34 00 total_hi total_lo`, where `total = N + 8`, followed by zero padding within the
   same transaction. The entire transaction is also `N + 8` bytes.
3. The first 6 response bytes form the transaction header. Skip a further 2 bytes and take exactly N
   bytes from complete-response offset 8, bitwise-inverting each byte.
4. Neither the header nor the 2 additional non-pixel bytes enter image processing.

The FT9348 request header is therefore **`04 FB 34 00 24 08`**; FT9361 uses **`04 FB 34 00 14 08`**.
`0x2E990` passes `N + 2` to the lower-level reader, and `0x2F020` adds 6 and writes that total into
header bytes 4 and 5. This rules out an FT9348 length of `24 02`. The normal reader at `0x2E990`
does not first send a resume preread. `0x26F70` is a separate preread path and must not be added
unconditionally to normal capture.

<a id="mcu-配置"></a>

### MCU configuration

Shared FT9348/FT9361 entry `0x399F0` performs this protocol:

1. Read register `30`. If it is already `BB`, this entry does not rewrite configuration.
2. Otherwise write `01 = 01`, `41 = 0F`, and `30 = BB` in order, waiting 2 ms after each write.
3. Read back `30`. Linux should require `BB` and fail otherwise.
4. Write `22 = 00`, wait 2 ms; write `23 = 0E`, wait 2 ms.
5. Linux should also verify MCU idle before reporting readiness.

Windows logs a failed configuration-marker readback and continues. Tightening that failure handling
is permissible; continuing capture is not a protocol requirement. These individually verified
register operations are configuration facts, not a copied vendor initialization table.

<a id="采集重置与重新布防"></a>

### Capture, reset, and rearming

FT9348 and FT9361 enter the same common control flow for the following operations, with parameters
supplied by the object:

| State or operation | Wire behavior | Evidence RVA |
| --- | --- | --- |
| Normal arming | After MCU idle, write work mode `01`, `1F = 01`, `1E = 01`; wait 10 ms and read `1D` | `0x287D4`, `0x28A14` |
| Quick recapture | After idle, write work mode `02`, `54 = 01`, then check MCU status again | Same entries |
| Stop normal capture | For known mode 1 or unknown mode: `1E = 00`, `1F = 00`, then wait 10 ms on SPI | `0x28668` |
| Exit modes 2/3/4 | The common flow omits those two stop-register writes and enters the next mode after reset | `0x28668` |
| Software reset | Send single-byte command `70` in two separate transactions, 5 ms apart on SPI | `0x28C54`, `0x22C78`, `0x2FC30` |
| After finger trigger | Confirm MCU idle, then `1D` = `01/A0`, then read the complete image frame | IRQ `0x2DC20` → `0x2841C` → `0x2E990`; also `0x2D140`, `0x2F660` |
| Choice after image read | FT9348 and FT9361 both rearm in normal mode 1; FT9338 takes a different branch | `0x2E990`, `0x2D140` |

Mode 3 is a low-power path: after writing work mode `03`, it also writes `00 = 20`. This is separate
from mode 1/2 used for normal capture. Linux need not introduce it merely to reproduce Windows power
policy.

Interrupt resources come from ACPI. Static host code does not establish one fixed rising/falling
edge for every board; resource handling must preserve the described polarity. Linux must prevent
stale IRQs from crossing operations and, after cancellation, timeout, or read failure, reset the
device or prove it has returned to a usable state.

Hardware reset has a separate timing contract: Windows GPIO helper `0x2F5E0` writes raw buffer `01`,
waits 10 ms, writes `00` for 20 ms, then writes `01`. `0x39C50` calls this pulse twice, with 10 ms
between calls, waits the object's 160 ms after the second pulse, then performs software reset. Raw
values, physical levels, and Linux logical assertion values must be distinguished; the full write
chain and resource evidence are in the [GPIO polarity analysis](gpio-polarity.md). These are waits
in the sample, not oscilloscope-confirmed minimum pulse widths. Different pulse widths in existing
FT9361 Linux hardware reports cannot automatically establish minimum timing guarantees for FT9348.

<a id="冷启动与固件选择"></a>

### Cold start and firmware selection

A8 ROM-family classification and OTP sources are documented in
[dynamic discovery](dynamic-discovery.md) and [runtime adaptation](windows-runtime-adaptation.md).
This review reconfirmed:

- Family responses `2B50`, `95A8`, and `23DD` enter the A8 OTP branch at `0x278F4`.
- For SPI, the low 4 OTP bits `1/2/3` identify FT9348; `4/E/F` identify FT9361. USB uses a separate
  shift and must not be treated identically to SPI.
- Both A8 chips share download entry `0x39730`. After entering download state, `0x2FD80` writes the
  respective complete firmware to address 0, waits 2 ms, then resets into the application. The
  `0x2FAB0` write frame is `05 FA 00 00 size_hi size_lo`, firmware contents, and one trailing zero
  byte. Header length is firmware size; complete transaction length is size plus 7. FT9348 uses
  header `05 FA 00 00 28 48`, total 10319 bytes; FT9361 uses `05 FA 00 00 28 9C`, total 10403 bytes.
- File size, hash, and chip identity must match exactly. Sharing a protocol does not authorize
  substituting another chip's firmware.
- After application startup, recheck geometry, FW, AGC, and MCU status. A successful download
  transaction alone does not permit capture.

Download-entry GPIO timing is supported by a direct call chain, not an analogy with application
reset: `0x39730` → SPI download entry `0x2EAD0` → GPIO reopen `0x30ABC` → GPIO pulse `0x2F5E0` →
`55 AA` → firmware write `0x2FD80` / `0x2FAB0`. Both A8 objects use this entry.

| Download stage | GPIO / SPI behavior | Explicit wait |
| --- | --- | --- |
| Release before download | GPIO raw `01`, controller target high | 10 ms |
| Reset before download | GPIO raw `00`, controller target low | 20 ms |
| Release and synchronize | GPIO raw `01`, then exactly two bytes `55 AA` | No additional Sleep observed between them |
| Upload | Send the complete `05 FA` firmware frame defined above | 2 ms after a successful write |
| Start application | A8 double hardware reset: each pulse is high for 10 ms, low for 20 ms, then released; another 10 ms between calls | 160 ms after final release |
| Application software reset | Single byte `70`, 5 ms, then another single byte `70` | 2 ms after the second command |
| Verification | Query MCU idle and reread runtime parameters | Bounded checks; successful upload alone is insufficient |

Download entry has **no** 160 ms application-reset wait. The two subflows must not be confused. The
controller target stays high for 20 ms between the two low pulses: 10 ms from the outer gap plus the
second helper's own initial 10 ms. These are explicit program waits; scheduling and bus latency add
time. They are not stated electrical minimum or maximum limits.

**Independent Linux consistency rule:** if an observed runtime identity conflicts with later ROM/OTP
identity, reject that open without loading either firmware. Only a response such as `0000/FFFF`,
carrying no valid runtime identity, may acquire an initial identity from a reliable ROM result.
Runtime geometry is an application response and cannot replace pre-download ROM identity. This is
stricter than the sample's permissive fallback, not a claim that Windows already enforces the same
rule.

<a id="ft9338--ft9536-的已确认差异"></a>

## Confirmed FT9338 / FT9536 differences

| Parameter | FT9338 | FT9536 | Evidence RVA |
| --- | --- | --- | --- |
| Width × height | 88 × 88 | 64 × 128 | `0x3574C`, `0x35C44`; field orientation `0x2D4A0` |
| Runtime `14/15` | `58 58` | `40 80` | Runtime dispatch |
| FW / AGC | `40 / 10` | `23 / 13` | Constructor parameters |
| Work-mode register | `76` | `47` | Same constructors; access at `0x285CC` |
| Object wait after reset | 80 ms | 180 ms | Same constructors; used at `0x36BD0` |
| Associated firmware size | 14184 | 11934 | Same constructors |

This group shares MCU configuration `0x36A30`, which differs explicitly from A8: it unconditionally
writes `01 = 01`, `41 = 0F`, and `30 = BB`, waiting 1 ms after each, then reads `30`. It has none of
A8's `22/23` configuration steps. Its download entry `0x365C0` additionally configures
download-state registers and verifies firmware readback; the corresponding A8 entry does not perform
that readback. Normal SPI images still use the geometry-dependent framing at `0x2E990`, but FT9338
selects mode 2 after reading an image while other legacy types select mode 1. A common frame format
alone is therefore insufficient to enable the full A8 lifecycle.

At the time of this initial analysis, this group's complete boot-A/boot-B download, update, and
failure-recovery paths had not been specified. The following minimal runtime capability was
deliberately separated from cold start instead of borrowing A8's lifecycle. Subsequent cold-recovery
work is documented in [legacy38 recovery](legacy38-recovery.md); the historical warm-only scope
below is not the current overall support limit.

<a id="ft9338--ft9536-最小运行时采集协议"></a>

### Minimal FT9338 / FT9536 runtime capture protocol

This capability **requires the specified application firmware to be running already**; it does not
by itself provide complete cold-start support. The facts below come from actual call chains, not
merely a shared method table.

`0x2C754` first returns to idle through `0x28668`, then reads versions through `0x2C8BC`. Matching
versions lead directly to this group's `0x36A30` configuration and initial arming in mode 1. Version
checks actually compare registers `1A` and `3C` with object parameters; only the failure branch
enters firmware handling. A minimal Linux runtime capability may explicitly reject that branch
without implementing unknown cold-start actions.

| Step | FT9338 | FT9536 | Control-flow evidence |
| --- | --- | --- | --- |
| Runtime identity read | `14/15 = 58 58` | `14/15 = 40 80` | `0x27290` |
| Firmware version read | `1A = 40`, `3C = 10` | `1A = 23`, `3C = 13` | `0x2C8BC` and respective constructor parameters |
| Return to idle | Two single-byte `70` commands, 5 ms apart on SPI; stop controls according to current mode | Same, with a different mode address | `0x28668` → `0x28C54` / `0x285CC` |
| Mode register | `76` | **`47`** | `0x3574C`, `0x35C44`, `0x285CC` |
| Stop mode 1 or unknown mode | `1E = 00`, `1F = 00`, wait 10 ms on SPI | Same | `0x28668` |
| Stop current mode 2/3/4 | Skip the `1E/1F` writes after software reset | Same | `0x28668` |
| MCU initialization | Unconditionally write `01 = 01`, wait 1 ms; `41 = 0F`, wait 1 ms; `30 = BB`, wait 1 ms; read back `30` | Same | `0x36A30` |
| First normal arming | Mode `01`, `1F = 01`, `1E = 01`; read `1D` after 10 ms | Same, writing mode at `47` | `0x2C754` → `0x287D4` |
| Data ready after IRQ | MCU `20/21 = A5 5A`, finger `1D = 01` or `A0` | Same | `0x2DC20` → `0x2841C` |
| Image SRAM address | `3400` | `3400` | `0x2E990` |
| Width × height, pixel bytes | 88 × 88, 7744 | 64 × 128, 8192 | Constructors, `0x2D4A0`, `0x2E990` |
| Complete image-request header | `04 FB 34 00 1E 48` | `04 FB 34 00 20 08` | `0x2E990` → `0x2F020` |
| Complete transaction length | 7752 bytes | 8200 bytes | Same call chain |
| Pixel extraction | Take N bytes at response offset 8 and invert each byte | Same | `0x2E990` |
| Mode after successful image read | **Mode 2** | **Mode 1** | The final branch of `0x2E990` selects mode 2 only for internal type 1 |

FT9338's post-read mode 2 sequence first confirms MCU idle, writes work-mode register `76 = 02` and
`54 = 01`, then queries the MCU, expecting it to leave idle for an active state. FT9536 chooses
normal mode 1, writing `47 = 01`, `1F = 01`, and `1E = 01`; it does not write mode to `76`. Both
paths are evidenced at `0x287D4`. The retry selection for an invalid finger at `0x2D140` uses the
same type distinction. No actual FT9536 call site here confirms mode 2 optimization; a common
function accepting argument 2 does not establish verified FT9536 quick mode.

A single-shot Linux capture may return directly to idle after completion, omitting Windows'
automatic rearming for continuous capture. The next capture begins from verified mode 1. An IRQ
without a valid finger may likewise terminate the current arm and retry from mode 1. This avoids
introducing unproven FT9536 quick mode; FT9338 mode 2 remains an optional optimization.

Interrupt dispatch `0x2DC20` handles special types 4, 9, 11, and 12 first. FT9338 internal type 1
and FT9536 type 6 both take the legacy branch: call `0x2841C`, then, on success, reach `0x2E990`
through SPI method offset `A0`. Thus this group's `0x3400` 8-bit image format has direct
IRQ-to-image-read call evidence.

<a id="本组取消失败和重开边界"></a>

### Cancellation, failure, and reopen limits for this group

The Windows common return-to-idle entry is used before firmware-version checks and supports both
mode registers. Linux can independently express it as a deadline-bounded state machine: stop
accepting old IRQs after cancellation, timeout, or transfer failure, send the confirmed software
reset and required stop commands, then read MCU idle. Only verified idle permits session reuse. GPIO
release should leave reset deasserted.

Exact software timing: single byte `70` → wait 5 ms → single byte `70` → read mode immediately. If
stop controls are needed, write `1E/1F = 00` and wait 10 ms. This entry has no final 2 ms
requirement; that delay belongs to the A8 hardware-reset wrapper. Persistent transfer failure or
failure to reach idle ends the session and requires reopen. Cleanup must not silently load A8
firmware or claim recovery without evidence.

The original minimal runtime design could explicitly disable hardware reset, ROM discovery, and
firmware upload, reporting an error and releasing resources on version mismatch or MCU-recovery
failure. That was a limited implementation option, not a permanent restriction on the later
[legacy38 recovery implementation](legacy38-recovery.md). The following hardware-reset facts
distinguish protocols; they do not require every warm capture to reset the hardware.

When hardware reset is needed, this group's `0x36BD0` calls the GPIO pulse helper twice, 10 ms
apart, then waits **80 ms / 180 ms** respectively. Unlike A8's `0x39C50`, **this entry has no final
A8 software reset and 2 ms wait**. Runtime identity and versions must be rechecked afterward. If the
application is absent, the minimal runtime capability must report unsupported cold start rather than
assume firmware survived; any implemented recovery requires its own confirmed identity and protocol.

Regression requirements for this limited capability include initial mode 1; FT9338 mode 2 if
enabled; FT9536 mode address `47`; both frame lengths and pixel boundaries; no image read while MCU
is busy or finger status is invalid; short-frame/capacity rejection; cancellation and error cleanup;
firmware/AGC mismatch; and identity changes after reset. These are implementation requirements, not
physical FT9338/FT9536 capture validation.

<a id="特殊芯片探测不是统一的无副作用只读操作"></a>

## Special-chip probing is not uniformly read-only or side-effect-free

### FT9368

`0x26D9C` sends `FF 00 00 00` through dedicated transport entry `0x2E200`, then waits 5 ms. It
requests a 32-byte block with `91 80 00 20 00 00 00`, a 39-byte total transaction, with block data
at complete-response offset 7. Block offsets 19 and 20 form the big-endian chip ID, expected to be
`9368`; their complete-response offsets are 26 and 27. The same block supplies version,
manufacturer, and geometry information, but log names alone cannot establish the complete capture
format. The initial command changes protocol state and has not been proven safe for every
unidentified legacy chip. The subsequent dedicated implementation is described in
[FT9368 protocol](ft9368-protocol.md).

<a id="ft9369--ft9365--ft9769-相关入口"></a>

### Entries related to FT9369 / FT9365 / FT9769

The main probe at `0x10A74` goes through `0xFD94` to write internal register `C6 = 01`, wait, and
read it back, then reads chip ID at internal address `1A8B` through `0x10D70`. Internal-register
wrappers `0x1830C/0x18368` use `08 F7` / `09 F6`, unlike runtime `10 EF` / `11 EE`. Reads of 16-bit
internal addresses also carry an address flag and use different response framing. This is not a
universal read-only substitute for legacy `14/15` queries.

The subsequent `ft93xx` path at `0x19978` additionally includes reset, SPI-mode setup, an ID
acceptance table, validation, and variant classification: for example, `9391` may become `9395`
according to `1816`. Its existence does not prove that capture, GPIO timing, and recovery are fully
supported for every such ID. Permitted writes before identification and recovery after errors need a
separate backend specification; trying every initialization sequence on unknown devices cannot fill
an evidence gap. Subsequent specifications are in [special probing](special-probe.md),
[FW9369 protocol](fw9369-protocol.md), and [ft93xx protocol](ft93xx-protocol.md).

<a id="本轮-linux-实现和测试边界"></a>

## Linux implementation and test scope of this analysis

This report provides static evidence for shared A8 initialization and capture on FT9348/FT9361, with
chip-specific firmware selection. Implementation tests must at least cover both frame lengths and
pixel boundaries, capacity rejection, version mismatches, ROM/runtime conflicts, incorrect firmware
rejection, reset after cancellation/timeout, close/reopen, and interrupt cleanup. Mathematical
synthetic images can validate memory and state-machine behavior; real fingerprints must not become
repository fixtures.

FT9348 has different geometry. The validity of historical FT9361 templates and matching thresholds
does not transfer automatically; reusing capture code is not evidence of authentication quality on
another chip. Current parameterized matching is documented separately in
[family authentication](family-authentication.md). The new-chip paths described by this original
analysis lacked physical-device validation; current implementation and hardware evidence are tracked
in the [support matrix](status.md#implemented-functions-and-test-limits).
