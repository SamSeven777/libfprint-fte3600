# FTE3600 hardware and validation status

A configured hardware profile is not evidence of successful operation. The
status below distinguishes implementation, maintainer reports and missing
validation; it is not a general authentication-safety certification.

| Platform | Evidence and scope | Reset route | IRQ route |
| --- | --- | --- | --- |
| One-Netbook A1 | Maintainer reports discovery, capture, enrollment, verification and cold-boot recovery on A1. Independent replication and a complete power/cancellation matrix remain needed. | `\_SB_.PCI0.GPI0`, 85 (`0x55`), active-low | Same controller, 86 (`0x56`), active-high |
| GPD Pocket 3, Jasper Lake | Experimental profile in main/upstream; no public enrollment/verification success closure yet. Requires controller HID `INT34C8`. | `\_SB_.GPI0`, 211, active-low | Same controller, 56, active-high |
| GPD Pocket 3, Tiger Lake | Experimental profile in main/upstream; no public enrollment/verification success closure yet. Requires controller HID `INT3455`. | `\_SB_.GPI0`, 179, active-low | Same controller, 24, active-high |
| Medion E3224 | Previous implementation never worked. Independent legacy bring-up; ordinary FT9361 driver rejects this platform before I/O. | Reported GPO1 pin 39; role/polarity not established. ctfdavis candidate does not request it. | Reported GPO2 pin 0, active-high; not yet used by new backend |

DMI names are `ONE-NETBOOK TECHNOLOGY CO., LTD. / A1`,
`GPD / Pocket 3` or `GPD / GPD Pocket 3`, and `MEDION / E3224`,
respectively. Do not infer support for a similar product name. Main/upstream
do not include the Medion profile; the Medion branch does not thereby inherit
main's GPD support.

GPD routing requires an exact supported controller HID; absent or unknown HID
must be rejected before configuring GPIO. A1 has a separately defined stable
route. These checks constrain configuration; they do not establish electrical
safety or successful operation of an experimental board.

## Medion evidence and next comparison

See the [rebuild evidence record](medion-rebuild.md). The Mint success report
names ctfdavis's module. Do not substitute the later unverified 4 MHz attachment
or infer Medion support from A1 tests. Old mock Medion capture coverage modeled
an A1 response, not a Medion response, and has been replaced by a no-I/O gate test.

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
The next bounded comparison and its interpretation limits are in the
[legacy-protocol test plan](../medion-legacy-id-test.md).

## Implemented functions and test limits

The A1 implementation supports 64 × 80 capture, eight-stage enrollment and
host-side verification when explicitly enabled. The intended FT9361 recovery
uses a 10,396-byte external image and a 10,403-byte continuous SPI transaction;
image transactions need 5,128 bytes. A buffer setting of 32,768 bytes accommodates
both. Expected application idle is `a5 5a`; `00 00` only means the expected
response was not obtained and is not a diagnosis by itself.

Unit tests and mock lifecycle tests do not establish successful cold boot,
suspend/resume, GPIO polarity, population accuracy or complete memory erasure.
A CI definition is not an executed result; retain logs tied to the exact
commit, branch and build options. Medion diagnostic/power tests are not
interchangeable with main's production-driver lifecycle tests.

## Authentication evidence

BRISK personal policy version 3 uses at least five mutual matches/inliers and
spatial/residual gates. Historical maintainer reports describe zero observed
acceptances in 342,720 offline non-matching comparisons. The repository does not
currently provide a complete independently reproducible protocol, independent
evaluation split and deployment-level report for that result. Do not present it
as measured population FAR=0 or as a latency/FRR guarantee.

Multi-person, multi-session FAR/FRR for the actual eight-subtemplate decision,
including retries and failed captures/enrollments, remains unmeasured. This is
an evidence gap, not merely a missing laboratory certificate. Default
authentication is disabled; opt-in use remains experimental with a working
password fallback. Do not enable experimental biometric authentication for
system-wide sudo or root access.
