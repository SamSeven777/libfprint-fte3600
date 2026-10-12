# FW9369 release and recovery lifecycle

The previous independent runtime reused startup baselines between FW9369
captures and did not implement this sensor's release-time maintenance. This
revision adds the missing lifecycle for raw silicon ID `0x9362`, including
its libfprint enrollment retries and consecutive actions on an open device.
The BRISK matcher and print format are unchanged.

## Behavior

A captured frame arms UP detection before delivery to libfprint. The next
capture confirms release with three accepted observations, updates the active
FDT thresholds and CRCs, updates the image baseline, checks renewed contact
and applies the reviewed recalibration conditions. Rejected or noisy release
observations remain in the release wait. A successful explicit release wait
is idempotent when capture follows it. Queued UP/DOWN events are retained
across matching and action boundaries.

Event handling includes communication checks, exact IRQ acknowledgement,
combined-status priority, the second status read and late-UP handling.
Detected reset/ESD, INVALID state, repeated confirmation failures and persistent
identity failures enter bounded recovery. SPI recovery performs the physical
HIGH 10 ms / LOW 20 ms / HIGH reset pulse, waits 5 ms and executes chip startup
and calibration. It does not rediscover the factory, upload firmware or reopen
the transport. Recovery alone does not establish that the finger was released.

Cancellation, timeout and I/O failures stop further acquisition and require
close/reopen. Closing after a final sample discards pending state; the next
open starts cold. Final sample delivery does not wait for a physical finger
lift, but a subsequent acquisition must pass the release lifecycle.

## Software validation

The reviewed corpus contains 98 synthetic original-execution cases, including
four supplementary DATA/IDLE stages. The component replay compares complete
SPI transactions, waits, payload hashes, retained state and outputs for 90
applicable cases. One late-UP/renewed-contact result intentionally follows the
Linux guard while preserving the observed traffic and state. Four internal
spread cases and four outer SPI-worker recovery cases are outside component
replay; independent native math and production-runtime tests cover those paths.

Existing startup (219) and adaptive image (237) fixture suites pass. New native
release tests cover classifier boundaries, integer wrap, CRCs, bad-baseline
conditions, complete event ordering and transport failures. The public runtime
test links actual production objects and covers eight touches, changing retained
baselines, queued interrupts, idempotent/automatic release, late UP, bounded
recovery, timeout, cancellation, I/O faults and close/reopen. Normal and
ASan/UBSan checks pass. A separate public libfprint API test covers eight accepted
enrollment samples with a duplicate retry, then retained-runtime verify/capture,
error retirement and cancellation using the unchanged matcher.

The native master integration passes the committed `scripts/check-fte3600.sh`:
12 production suites, the separate synthetic-backend public API suite, and
9 kernel transport-policy groups. Native builds treat compiler warnings as errors.

## A1 automatic regression

The same independent source built and passed package checks against stable
libfprint 1.94.10 on A1, x86_64 NixOS 26.05, on 2026-10-12. A 25 ms timed
open-cancellation request returned cancellation with the device closed after
28.388 ms. Three subsequent opens and closes passed; open times were
3497.180, 3486.370 and 3493.813 ms. The test requested no fingerprint capture,
enrollment, verification or stored-print change. A1 is FT9361, so this is a
shared-path regression check, not physical FW9369 validation. The master
integration has separate native software validation.

## Evidence and limits

Implementation and tests were authored in fresh clean contexts from the
reviewed behavioral specification `fw9369-release-v1`, with existing independently
written startup and adaptive image components. The research handoff used the
Windows 2.0.3.102 artifact with SHA-256
`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`.
This change includes no vendor executable, firmware, disassembly, proprietary
matching algorithm or personal biometric input.

The finite synthetic observations distinguish original behavior from Linux
policies. Linux propagates transport and readiness errors, guards undefined
image neighborhoods, bounds recovery to three attempts per public runtime capture or release call and checks
cancellation/deadlines. It also refuses to report release when maintenance
finds renewed contact, including the original's late-UP override path.
An explicit release call followed by a capture call has separate budgets;
a direct capture shares one budget across automatic release and capture.
These differences are deliberate; complete Windows equivalence is not claimed.

Earlier GPD Pocket 3 results apply to the previous SamSeven777 implementation,
not this source revision. A1 is FT9361 and cannot validate the FW9369 event
lifecycle physically. Medion E3224's unknown identity/OTP/readback failure is
outside this repair. Physical FW9369 capture/enrollment, release latency,
suspend and changing environmental conditions still need device testing.

Source identity and hashes are recorded in
[`independent-source.json`](independent-source.json).
