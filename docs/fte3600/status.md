# FTE3600 hardware and validation status

The current implementation uses ACPI resources and runtime/ROM chip discovery;
there is no DMI admission table. The current transport has compile and mock-test
evidence, but no physical-device validation in this change.

The transport migrated from the custom SPI bridge; its details are described in
[ACPI glue / stock spidev](acpi-spidev.md). ABI 2 supports both GpioInt and
ordinary ACPI IRQ/Interrupt resources, with a reset GPIO and a separate UIO
interrupt device. FW9369 final shutdown now masks/acknowledges known events
and sends C1; FT9368 has the confirmed bounded wake retry. These changes have
software-test evidence, not a new hardware result. The
[coverage record](windows-lifecycle-coverage.md) distinguishes the implemented
fixes from remaining lifecycle differences.

## Latest GPD result, checked on 2026-10-04

The tester's [resource correction](https://github.com/SamSeven777/libfprint-fte3600/issues/2#issuecomment-5987576100)
and [capture result](https://github.com/SamSeven777/libfprint-fte3600/issues/2#issuecomment-5987625692)
supersede the earlier assumption that this board uses the GpioInt branch of
its ACPI templates. Its active resource is ordinary `Interrupt(Edge, ActiveLow)`.
With a local patch adding that IRQ resource to main `1ce4c74`, unmodified
userspace identified `9362` using physical active-high CS, initialized,
captured and completed two close/reopen runs. IRQ polarity needed no override.
This is evidence for that patched revision on that board, not for the current
ACPI glue / stock-spidev transport.

Images were discarded, so image quality, enrollment, verification and system
suspend/resume remain untested in that report. Closed-device IRQ counts still
rose at roughly 10 per second; no accompanying sensor-event registers identify
the cause. Do not report either a proven sleep fix or an IRQ-polarity fault.

## Historical platform profiles

The following table records **historical observations from the previous
spidev/GPIO-profile implementation**, not current routing rules or evidence
that the new bridge works on those devices.

| Platform | Evidence and scope | Reset route | IRQ route |
| --- | --- | --- | --- |
| One-Netbook A1 | Maintainer reports discovery, capture, enrollment, verification and cold-boot recovery on A1. Independent replication and a complete power/cancellation matrix remain needed. | `\_SB_.PCI0.GPI0`, 85 (`0x55`), active-low | Same controller, 86 (`0x56`), active-high |
| GPD Pocket 3, Jasper Lake | Experimental profile in main/upstream; no public enrollment/verification success closure yet. Requires controller HID `INT34C8`. | `\_SB_.GPI0`, 211, active-low | Same controller, 56, active-high |
| GPD Pocket 3, Tiger Lake | Experimental profile in main/upstream; no public enrollment/verification success closure yet. Requires controller HID `INT3455`. | `\_SB_.GPI0`, 179, active-low | Same controller, 24, active-high |
| Medion E3224 | Separate experimental `medion-e3224` branch. Current implementation has not produced a successful identity/capture result on the reported machine. | `\_SB_.GPO1`, 39 (`0x27`); active-low is the current hypothesis, not a completed board-level validation | `\_SB_.GPO2`, 0; reported active-high IRQ |

Those model strings and routes are retained as historical evidence only.
The current driver obtains each controller/pin from ACPI. A shared ACPI ID does
not establish chip identity, and successful operation still needs hardware tests.

## Earlier issue evidence checked on 2026-10-04

The [earlier GPD Pocket 3 report](https://github.com/SamSeven777/libfprint-fte3600/issues/2#issuecomment-5981476115)
provides a positive identity for one **i7-1195G7 / G1621-02** board: SRAM query
`04 fb 9a 8b 00 01` returns big-endian `93 62` with active-high SPI chip select.
Both an eight-byte full-duplex transfer and write-then-read under one chip
select work. Active-low returns zero, although this board's ACPI says
`PolarityLow`. The initial reset/IRQ interpretation was INT34C5 lines 14/323,
not the older Tiger Lake profile's 179/24; the IRQ interpretation was corrected
by the newer ordinary-Interrupt report above. These observations apply to the reported board,
not automatically to every Pocket 3 variant.

**The reported GPD protocol and CS requirements are implemented; the newer
patched-main test above provides limited physical capture evidence.** Discovery sends the factory's 12-byte ID query,
tries both physical CS polarities if necessary, and requires two matching
`9362` responses. The FW9369 backend performs AFE/FDT and image calibration,
reads 16-bit samples, and independently constructs an 8-bit image. It requires
an uncovered sensor during opening to establish its baseline. On this branch,
CS negotiation uses stock spidev and requires the matched ABI2 reset/UIO glue;
old custom bridge interfaces are not accepted by this transport. Reset GPIO
active-low and SPI CS active-high are separate electrical signals.

**Medion E3224 remains unconfirmed.** Separate reset/IRQ controllers are handled
by the bridge, but [the last sensor-response test](https://github.com/SamSeven777/libfprint-fte3600/issues/1#issuecomment-5928567443)
still returned all zeroes before and after software reset with the SPI parent
held in D0. The [newer baseline-tool report](https://github.com/SamSeven777/libfprint-fte3600/issues/1#issuecomment-5979436524)
failed its isolated library ABI check before touching hardware; it is not a
test of the new clean-room bridge and supplies no new chip identity. The old
Mint stack's reported success remains a useful reference. Do not extrapolate
the GPD chip identity or required CS polarity to Medion without evidence.

## Medion evidence and next comparison

The Medion report identifies separate GPO1/GPO2 controllers; the reset-controller
log identifies `INT3453`, not `INT3452`. Do not substitute reset 40 / IRQ 39.
The module's exact sensor IC has not been confirmed by a valid device response.
Neither the shared ACPI ID nor all-zero responses proves FT9361, FT9362 or a
missing power rail.

The same reported machine worked with an older Mint software stack. Preserve
that known-good comparison as the starting point. Compare initialization,
firmware, transport, GPIO and power-management behavior with that stack before
requesting another experiment. Do not ask the reporter to repeat an unchanged
recovery sequence that already failed. See the [hardware discussion](https://github.com/SamSeven777/libfprint-fte3600/issues/1).

## Implemented functions and test limits

The current catalog contains eight chip profiles, six Windows protocol families
and four Linux backend modules. Each has a native-image BRISK adapter for
eight-sample enrollment and verification in the experimental opt-in build.
The default build exposes capture only. Sharing a protocol or image size does
not make templates or firmware interchangeable.

| Chip | Native image | Initialization and recovery boundary |
| --- | --- | --- |
| FT9338 | 88 × 88 | Running-application capture; RAM recovery needs current-open runtime identity plus matching boot-B OTP. First unidentified cold boot is unsupported. |
| FT9348 | 96 × 96 | A8 runtime or matching ROM/SPI-OTP identity; its own external RAM firmware. |
| FT9361 | 64 × 80 | A8 runtime or matching ROM/SPI-OTP identity; its own external RAM firmware. |
| FT9536 | 64 × 128 | Running application, positive boot-A identity or current-open runtime plus boot-B OTP; its own RAM firmware and complete readback. |
| FT9365 | 64 × 80 | Positive silicon identity and host AFE/DAC configuration; no application firmware upload. |
| FT9368 | 64 × 80 | Healthy identified application; persistent update is separate and explicit. Blank/unresponsive recovery remains unsupported. |
| FW9369 / raw ID 9362 | 64 × 80 | Positive silicon identity, host FDT/image calibration; uncover the sensor while opening. No application firmware upload. |
| FT9769 / raw IDs 9391, 9392 | 40 × 196 | Positive identity and variant check, host AFE/DAC configuration; extra raw rows are drained and excluded from the image. |

The [firmware installer](install.md#2-install-the-firmware-for-the-identified-chip)
can validate and install all six catalogued payloads for FT9338, FT9348, FT9361,
FT9536 and the FT9368 application/PRAM pair. It does not authorize device
recovery or enable persistent updates. Installing FT9338 firmware cannot
replace the missing first-cold-identity evidence, and installing the FT9368
pair cannot establish a blank-chip recovery route. No vendor payload is bundled.

The bridge bounds transactions by the SPI controller limit and a 32,768-byte
ceiling. Each backend checks its actual largest transaction before starting;
the FT9365/9769 FIFO is deliberately chunked, while legacy image/RAM-readback
transactions remain continuous. See [transport limits](dynamic-discovery.md#electrical-and-protocol-limits).
Legacy application idle is `a5 5a`; `00 00` means that the expected response was
not obtained and is not a diagnosis by itself.

ACPI resource discovery removes the computer-model whitelist, but it does not
make missing or contradictory firmware descriptions usable. One SPI resource,
one single-pin reset GpioIo and one single-pin edge GpioInt are required.
Separate GPIO controllers are supported; ambiguous resources, unsupported
trigger modes, reset mapping conflicts and transfer limits are explicit
failures. The bridge cannot infer a missing power rail, inverter or undocumented
board reset polarity. A shared `FTE3600` ACPI ID is not a chip identity.

Unit tests and mock lifecycle tests do not establish successful cold boot,
suspend/resume, GPIO polarity, population accuracy or complete memory erasure.
Synthetic installer tests check metadata consistency, input validation and
atomic file replacement in temporary directories; they do not download vendor
packages, write system firmware paths or exercise hardware programming.
A CI definition is not an executed result; retain logs tied to the exact
commit, branch and build options. Medion diagnostic/power tests are not
interchangeable with main's production-driver lifecycle tests.

## Authentication evidence

New enrollments on all eight sensor profiles use BRISK diagnostic policy 7 /
optional personal policy 8, with native image parameters and rotation-invariant
spatial-shape evidence. Verification accepts a passing comparison against any
of eight individual samples or their canonically reconstructed mosaic. Each
comparison retains the five mutual matches/inliers and residual gates.
Existing wire-v1 FT9361 templates retain policy 5/6 through a compatibility
path; they do not define the other profiles' geometry. Older extractor/policy
versions remain unsupported. See [family authentication](family-authentication.md).

Historical maintainer reports describe zero observed
acceptances in 342,720 offline non-matching comparisons. The repository does not
currently provide a complete independently reproducible protocol, independent
evaluation split and deployment-level report for that result. Do not present it
as measured population FAR=0 or as a latency/FRR guarantee.

Those historical pair comparisons do not evaluate the current mosaic decision.
Multi-person, multi-session FAR/FRR for the actual gallery-plus-mosaic decision,
including retries and failed captures/enrollments, remains unmeasured. This is
an evidence gap, not merely a missing laboratory certificate. Default
authentication is disabled; opt-in use remains experimental with a working
password fallback. Do not enable experimental biometric authentication for
system-wide sudo or root access.
