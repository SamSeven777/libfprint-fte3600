<a id="windows-对照复查--2026-10-04"></a>

# Windows comparison recheck — 2026-10-04

[Documentation index](README.md)

**Historical audit of the pre-ACPI-glue implementation.** This record covers
the local working tree after `a59b2e3`, including CS-session cleanup changes.
It establishes only the branches checked below, not a complete port of the
Windows lifecycle. Later ordinary-IRQ, sleep/background-event and FT9368 wake
findings are recorded in [lifecycle coverage](windows-lifecycle-coverage.md).
The counts below belong to that earlier tree and do not validate all current
stock-spidev/ACPI-glue resource layouts.

The Windows baseline was the locally retained AMD64
`ftWbioUmdfDriverV2.dll` from package 2.0.3.102, with recomputed SHA-256
`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`.
Its actual PE FileVersion/ProductVersion is 1.0.0.3188; package and file
versions are distinct. The audit compared original disassembly with Linux
state machines without executing the vendor DLL or accessing hardware.
All Windows addresses below are RVAs. Only protocol facts are recorded in
the repository, not vendor code or firmware.

<a id="发现并修复的问题"></a>

## Issues found and fixed

<a id="1-ft9338ft9536-下载失败仍进入应用启动"></a>

### 1. Failed FT9338/FT9536 download still entered application startup

Previously, `fte3600-legacy-recovery.c` put the double hardware reset,
application-start wait and MCU/geometry checks in the state machine's cleanup
section. Upload, readback or full-comparison failure, and cancellation, could
all enter that sequence. Preserving the error prevented a successful result
but did not prevent physical startup pulses.

In Windows `0x365c0`, readback failure returns at `0x36854–0x368aa`;
full-comparison failure returns at `0x368da–0x36916`. Only successful
validation reaches `0x3691b` and then invokes startup method `0x36bd0`
at `0x3693b–0x36948`.

The fix separated startup from error cleanup. Only a complete, matching
readback allows the double pulse and application checks. Failure merely
attempts to release reset and invalidates the session. The parent
`fte3600_init_complete` then closes resources without sending another
`70`, reading mode or checking MCU state. An active GPIO pulse completes
before responding to cancellation, and cleanup errors do not replace the
original error.

This changes actions after RAM-recovery failure, not firmware identity gates.
Releasing reset alone is not proof that the chip returned to idle. See
[legacy38 recovery](legacy38-recovery.md).

<a id="2-专用身份确认失败被当成未知设备继续探测"></a>

### 2. Conflicting special-family identity was treated as an unknown device

The Windows factory path at `0x2411c–0x24185` wakes/configures C6, then selects
the special family from observed `9362/9365/9391/9392`. Linux independently
requires repeated confirmation to reduce misidentification.

Previously, special cold probing cleaned up and returned an empty result when
two IDs differed, or when the `9391` subtype register `1816` had invalid CRC
or inconsistent repeated values. The parent could try another polarity or
legacy ROM probing. This discarded positive identity-conflict evidence and
differed from the direct-read path's handling.

After the fix, inconsistent IDs return a protocol error; invalid subtype CRC
retains the checksum error. Required GPIO cleanup completes, then discovery
ends. The parent restores CS without continuing to ROM fallback. Stable unknown
responses can remain a miss; stable known-but-unsupported models retain their
diagnostic evidence.

This repairs Linux's independent confirmation policy; it does not claim
Windows used the same double-read rule. See [special probing](special-probe.md).

<a id="正常路径核对"></a>

## Normal-path checks

| Item | Windows evidence | Finding within the checked scope |
| --- | --- | --- |
| Reset output | `0x2f5e0`, `0x31414 → 0x3106c` | Raw pin values `1 → 10 ms → 0 → 20 ms → 1` are passed unchanged to GPIO writes. Linux logical `0/1/0` through an active-low descriptor requests the same H/L/H waveform. This is not a measured waveform. |
| SPI transfer | `0x30cbc–0x30e08` | Windows constructs equal-length TX/RX full-duplex requests. The old Linux bridge's simultaneous TX/RX in one `spi_transfer` matched this; no evidence required extra clocks. |
| Legacy wake | `0x28c54`, `0x28344`, `0x2422a` | Two separate one-byte `70` commands, 5 ms apart, up to six rounds, then 350 ms after MCU idle. Linux retained the validated A1 trailing 2 ms/repeated-geometry fast path before slow fallback. |
| FT9348/FT9361 A8 upload | `0x39730`, `0x39c50` | `55 aa`, complete upload, 2 ms, double hardware reset, 160 ms and double `70` follow the checked order. Upload failure cannot reach startup pulses. Windows also lacks legacy38-style complete RAM readback here. |
| FT9338/FT9536 upload | `0x365c0`, `0x36bd0` | Four-byte preparation, 20 ms, complete upload, 2 ms, complete readback and 80/180 ms startup waits match. This audit corrected the error branch. |
| Four legacy image formats | `0x2e990 → 0x2f020` | `3400`, frame length `N+8`, pixel offset 8 and byte inversion match. FT9536 uses mode register `47`; the other three use `76`. |
| FW9369 / raw ID `9362` | `0x10d70`, `0x186b0`, `0x17000`, `0x18208`, `0x180f4` | Identity/process selection, manual scan and FIFO framing match; a complete frame has a six-byte header and 10,240 pixel bytes. |
| FT93xx / `9365,9391,9392` | `0x19978`, `0x1aa7c`, `0x1ebe0`, `0x21ed0`, `0x21f94` | FD/FE, C6, ADC windows/integration, at most 1,790 payload bytes per packet, four extra rows, pixel masking and 9392 reordering match. `9391 + 1816=0fff` must not select FT9769. |
| FT9368 | `0x2444e`, `0x384f0`, `0x38670`, `0x25328`, `0x266b4` | The checked INFO/cleanup paths, seven-byte header plus 5,120 pixel bytes, and explicit-update PRAM readback/flash-packet verification order match. Later wake-path gaps are documented separately. |

“Match” is limited to the listed messages and control flow. It does not validate
all register meanings, OEM differences, calibration algorithms or cold-start
branches. FT9769's main factory responses are `9391/9392`; class names, raw
chip values and computer model names are not interchangeable.

<a id="保留的独立设计与验证边界"></a>

## Independent decisions and evidence limits

- **CS:** Windows `0x2eb20 → 0x303bc` opens Resource Hub using the ACPI
  connection ID. The checked chain did not show dual-CS correction. Linux's
  polarity trials and restoration were independent decisions. Restoring a
  session's original value did not establish electrical correctness or
  shared-bus isolation. Current stock-spidev restoration has different process
  lifetime limits; see [the current transport](acpi-spidev.md).
- **ACPI:** Windows selects the first matching resource; Linux requires
  unambiguous roles. Different GPIO controllers or interleaved reset/IRQ
  resources do not change GpioInt's index semantics.
- **C6 retries:** Windows helper `0xfd94` allows at most 31 attempts. Linux
  cold probing allowed four, each with a 4 ms wait. This shorter bound was a
  policy difference; slow-device evidence was needed before calling it a
  demonstrated hardware failure.
- **First completely blank FT9338 boot:** Linux does not adopt Windows'
  `FE!=02` or OTP=`FF` default classification. No reliable identity means
  no automatic firmware choice. The Medion explicit-candidate experiment is
  separate.
- **Software reset after failed A8 upload:** Startup double hardware pulses
  are unreachable after failure, but the parent can still issue legacy `70`
  cleanup. There was no evidence that this starts unverified RAM in A8 download
  state; it must not be conflated with the demonstrated legacy38 defect.
- **Calibration and standby:** FW9369's uncovered-baseline checks and FT93xx's
  exposure/texture decisions are independent host algorithms, not a complete
  implementation of Windows FDT/OTP calibration. FT93xx keeps verified awake
  idle instead of the final `A5` at `0x1a804`; equivalence across hardware
  remains unmeasured.
- **FT9368 cold recovery:** Explicit flash update requires valid application
  identity first and does not cover every Windows no-application ROM recovery
  branch. This is distinct from legacy RAM recovery.

The audit also corrected stale descriptions of FT9361-only support and
unconditional `_DSD` reset-polarity precedence. There were eight sensor profiles
across four backend modules, not eight separate backend implementations.
Conflicting reset properties were rejected before driving the output.

<a id="验证记录"></a>

## Validation at that checkpoint

A new identity-change regression reproduced “error expected, empty success
returned” before the fix and passed afterward. Tests also checked that the
parent sent no further ROM query, restored CS and retained the first error.
Legacy38 tests covered failures in all eight pre-start SPI transactions for
both chips, a mismatch in the last readback byte, cancellation during
upload/readback/startup, and GPIO-cleanup failure.

Ubuntu 24.04/WSL, GCC 13.3, warnings treated as errors:

| Configuration | Passing suites | Special probe | Legacy38 recovery | Complete lifecycle |
| --- | ---: | ---: | ---: | ---: |
| Authentication and IPA off | 26 | 21 | 39 | 120 |
| Authentication and IPA on | 26 | 21 | 39 | 123 |
| ASan + UBSan, three affected suites | 3 | 21 | 39 | 123 |

All selected suites passed. Each complete normal matrix skipped one optional
case requiring external FT9361 firmware; the three affected suites had no
skips. The preceding CS fix's 49 policy tests and Linux 6.8 `W=1` module build
remained applicable to that old bridge; this audit made no further kernel
change.

No new A1, Medion or GPD hardware evidence was obtained, so these results did
not establish validation of any complete computer.
