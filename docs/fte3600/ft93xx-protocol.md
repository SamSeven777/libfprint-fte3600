# FT9365 / FT9769 protocol notes

This is a technical description independently derived from the supplied
`ftWbioUmdfDriverV2.dll`. Addresses below are RVAs in that baseline, not Linux
symbols. No vendor implementation, firmware image or calibration database is
included. The Linux host algorithm is independently written and is not a claim
of equivalence to the Windows fingerprint-processing algorithm.

## Identity and geometry

| Object | Positive silicon ID | Visible image | FIFO image | Evidence |
| --- | --- | --- | --- | --- |
| FT9365 | `9365` | 64 × 80 | 64 × 80 × 2 bytes | Constructor `359f4`, area selection `18a20` |
| FT9769 | `9391` or `9392` | 40 × 196 | 40 × 200 × 2 bytes | Constructor `35d24`, area selection `18a20` |

The constructors store height at object offset 8 and width at offset 9. Four
extra rows accompany FT9769 data and are not part of the public image. The
Windows host processes the first width × height samples (`1f044`).

The shared ID reader (`1a44c`, `19978`) reads register `1a8b`. For `9391`, the
reference reads `1816` and changes its internal ID to `9395` when that register
is `0fff`. Linux rejects this combination before changing the scan window.
Known lower-layer IDs `9363`, `9349`, `9372`, and `9395`–`9398` are not aliases
for these two capture profiles. No default profile is selected on an unknown
ID, malformed reply or failed transfer.

The shared Windows factory path (`24123/241ae → 10a74`) wakes the chip, writes
`c6=01`, waits 4 ms and verifies its readback **before** selecting a family from
register `1a8b`. Its ID read uses a 12-byte transaction with header count one.
Linux first attempts non-configuring ID reads, then uses the independently
implemented [shared negotiation fallback](special-probe.md) when those fail.
Each selected CS polarity requires a repeated matching ID, plus repeated
variant validation for `9391`. C6 is state-changing; its reset default and
individual bit meanings are not established. A failed fallback resets the
hardware before another discovery protocol is attempted.

After the factory has selected FT93xx, its initialization path
(`2465e → 196b8 → 19978 → 1aa7c`) configures the chip's internal I/O pad mode:
SFR `fd=0a`, then `fe=7f`, a 1 ms wait and readback of `fe`. Its log names this
the 1.8 V mode. This is a chip register operation, not an ACPI regulator or
GPIO command. Linux applies it only after positive ID selection; unknown-family
negotiation never writes these voltage controls. The backend revalidates
`c6=01` (`195d0`) and identity before image configuration.

## Wire encoding

All transfers described here keep chip select asserted for the complete
command and response. Multi-byte register values, addresses and counts are
big-endian.

| Operation | Bytes transmitted | Response |
| --- | --- | --- |
| Read SFR | `08 f7 address 00 00` | Byte 4 |
| Write SFR | `09 f6 address value` | None |
| Read register | `04 fb (address_hi OR 80) address_lo 00 00` followed by 4 dummy bytes | BE value at bytes 6–7, trailer at 8–9 |
| Write register | `05 fa (address_hi OR 80) address_lo 00 00 value_hi value_lo` | None |
| Read image FIFO | `06 f9 9a 05 count_hi count_lo` followed by payload and 2 dummy bytes | Payload starts at byte 6 |

For FIFO reads, count is **payload bytes / 2 − 1**. The reference limits each
transaction to 1790 payload bytes and repeatedly reads the same FIFO port
`1a05` (`21ed0`, called by `21f94`). A 10240-byte or 16000-byte frame therefore
does not require a single frame-sized SPI transaction. Each individual FIFO
transaction has eight bytes of overhead; these are stripped before appending
its payload. A partial transfer is an error, never a partially valid image.

Register checksums (`21d8c`, `21df8`) use CRC-16/CCITT-FALSE: polynomial
`1021`, initial value `ffff`, no reflection, no final XOR. The checksum covers
the returned data bytes, excluding the command header. The trailer is BE.
The reference explicitly accepts a zero trailer. Linux preserves that
sentinel behavior and validates every nonzero register checksum. The image
FIFO reader does not validate its two trailer bytes; their meaning is not
established, and Linux does not invent a FIFO checksum requirement.

## State and image scan

Three-byte commands have a byte followed by its complement and `00`:

| Operation | Command | Observation |
| --- | --- | --- |
| Wake register interface | `5a a5 00` | Wait at least 1 ms |
| Enter idle | `c0 3f 00` | SFR `80` must become `50` |
| Release interface | `a5 5a 00` | Follows idle verification |
| Select full image scan | `c4 3b 00` | SFR `80` must become `54` |

The reference also has a navigation scan command and several FDT modes;
these are not needed by the independent full-image acquisition loop.

The image sequence at `1f2d4` is: configure image mode, select image scan,
check its SFR state, set bit 0 of scan-window register `1800`, and wait for
image-ready bit `0020` in `1a82`. Register `1a84` acknowledges interrupt bits.
Other status bits include reset `0200`, ESD `0400`, and open/short `0800`.
Linux rejects those faults and has bounded mode, status and idle waits.

Returning idle (`1a804`) wakes the interface, requests idle, checks SFR `80`,
and releases the interface. Linux leaves the interface in the verified awake
idle state instead of claiming a post-release state it has not read back.
Linux also closes the protected AFE
register window (`9a=00`) and clears pending interrupts. Cleanup ignores
action cancellation, attempts the remaining bounded cleanup steps after an
I/O error, and keeps the original error. A failed cleanup cannot establish
`idle_verified` and cannot authorize another action on that session.

## Cold image initialization

These devices run the host-driven register/FIFO protocol; this path does not
upload application firmware. Hardware configuration facts were derived from
`1ca30`, `1ebe0`, `1f734`, `1a9e0`, `10f94`, `1a8b8`, `11098`, and `1abf0`.
Values are expressed as separately meaningful fields rather than an opaque
copied initialization table:

* FT9365 uses a 7-bit SDAC, initially 72, image gain 7, window `4ffe` and
  extension `0000`. FT9769 uses an 8-bit SDAC, initially 124, image gain 5,
  window `c7fe` and extension `7fff`.
* Register `1801` combines the model's SDAC width, gain and image polarity
  fields. Register `1807` selects 128 integrations and sample field 15.
* Four MHz AFE timing uses `1806=023b`, `180a=0001`, `180b=0104`.
  Register `1812` retains its undocumented high bit, sets timing field
  `[13:7]=2`, `[6:0]=0`, and disables automatic baseline subtraction at bit
  14. An unverified startup image is never used as an empty-sensor base.
* Shared cold configuration sets reset timing `1808` to `0807` for FT9365
  or `084b` for FT9769, clears `1815`, and sets `180d=1000`.
* The protected analog window is opened with SFR `9a=5a`. FT9365 modifies
  `c0[12:0]=0444`, `c1[5:0]=21`, and `c2[7:6]=3`. FT9769 configures channels
  3, 9, 15 and 20: register `c0+2*channel` gets `[12:0]=0103`, and
  `c1+2*channel` gets `[12:0]=0000`. Other bits are preserved. The window is
  closed with `9a=00`, including cleanup after a failed operation.
* Image mode is `1804=27ca`, `1811=01fe`. Interrupt mask `1a83` enables
  data-ready and AFE events (`0060`) while preserving unrelated fields.
  FT9365 I/O drive strength is `1a8e[2:0]=2`.
* Timing SFRs use `8e=244` (100 ms scaled by 10000/4096), `8d=97` (5 ms
  scaled by 10000/512), `8f=ff`; watchdog programming stops `90`, writes
  `91:92=07d0`, then starts `90=1`. Register `1a06=0001` selects ordinary
  image acquisition.

Linux reads back configuration registers and rejects mismatches. Writes
that are strobes or protected-window controls are not mistaken for readable
configuration latches. No undocumented bits are overwritten by partial-field
settings. OTP serial-number collection is unnecessary for this image path;
the mapped models' scan geometry and configuration are selected by positive
silicon identity, not by host model names.

## Samples and independent host processing

Samples are BE words with signal in mask `0ffc` (`1f044`, `1f104`). For raw
silicon ID `9392`, reverse every group of four visible samples (`1f6e4`).
ID `9391` uses its ordinary order. The additional FT9769 rows are drained
but excluded from the image. They do not alter row stride.

The reference calibrates SDAC using a 10-bit histogram median around 868;
raising SDAC increases this median (`1d6d4`, `1e2e8`). Linux uses its own
bounded binary search over the confirmed DAC range and accepts a finite
operating band around that value. It retains only the resulting scalar DAC,
not the startup pixels. A finger already present at open cannot silently
become a stored subtraction baseline.

Linux full-image qualification currently checks percentile range and local
horizontal gradients before returning an 8-bit image normalized between its
1st and 99th percentiles. Flat frames are discarded, wiped, and polled at a
bounded rate with cancellation. This is a capture-quality heuristic, not
proof of physical finger presence, liveness, or authentication accuracy.
Unlike the Windows FDT implementation, it does not establish or retain eight
per-electrode empty-sensor baselines. Real devices are required to validate
fixed-pattern rejection, gain, polarity, empty-sensor behavior and acceptable
capture latency.

Between enrollment frames, the release observer requires three consecutive
raw frames with both percentile range below 128 and total horizontal gradient
below eight times the pixel count. Any textured frame resets that streak.
Release observations remain 100 ms apart. Ordinary empty-image capture polls
back off from 100 to 200, 400 and at most 500 ms. These are Linux host policies,
not a calibrated contact detector, sensor sleep command or temperature model.
Every scan and an entry-time cancellation perform verified idle cleanup.

FT9365 and FT9769 now have sensor-specific BRISK adapters for the explicitly
enabled experimental personal-authentication build. Their native 64 × 80 and
40 × 196 images are not resized to FT9361 geometry. New templates carry their
own model and processing revision; the modern policy evaluates spatial shape
using rotation-invariant principal variances and each sensor's long/short-side
ratio. Default builds
remain capture-only. Protocol and synthetic tests validate implementation
behavior, not population accuracy, liveness or authentication suitability.
See [family authentication](family-authentication.md) for the opt-in policy,
template isolation and remaining evaluation limits.

## Validation scope

Protocol tests use known CRC check vectors, exhaustive FIFO size boundaries,
synthetic pixel ramps, FT9392 group ordering, cropped rows, unsupported IDs,
and malformed lengths. Backend tests simulate the hardware boundary while
running the real SSMs and transfer ownership. These establish host safety and
sequencing, not electrical timing on a physical device or biometric quality.
The family matcher and public authentication lifecycle tests additionally use
generated mathematical images to exercise both chip geometries, enrollment,
verification, mismatched templates and failure completion. No real fingerprint
corpus or measured multi-person FAR/FRR supports these profiles yet.
