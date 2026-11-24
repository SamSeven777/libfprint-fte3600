# Legacy fast-open validation

## FT9361 hardware measurements, 2026-11-21

On the One-Netbook A1, upstream `579ac579` took 3.54–3.92 seconds to open
the reader. The previously installed `395425c` build took 0.36–0.37 seconds.
These are public libfprint synchronous device-open timings, measured over
three open/close cycles per build on the same machine.

The new Linux/libfprint warm-open path sends the existing legacy ping's two
idle commands, then reads MCU readiness, geometry and both firmware-version
bytes. An exact, live geometry and firmware/AGC version match qualifies. It still loads
the verified firmware for recovery and runs the ordinary initializer and
acquisition setup. Unknown or incompatible replies use full cold discovery;
I/O errors, stale device generations and cancellation terminate the action.
Explicit recovery of an existing failed session retains cold initialization.
The libfprint adapter also uses cold startup when reopening after an action
failure. No hardware handles or asserted readiness are retained across close.

Three measured opens with this path took 81.25, 73.91 and 73.68 milliseconds.
This measures opening a running sensor application. It does not establish
cold-boot speed, suspend/resume correctness, other-profile hardware support,
or end-to-end fingerprint verification latency.

Regression checks cover successful warm startup followed by image capture,
each failed readiness/geometry/version predicate falling back to the existing
cold transcript, non-legacy profiles retaining cold startup, mandatory
and correctly bound firmware, fatal I/O, stale generation, cancellation and
explicit cold recovery. Existing Linux board/runtime, authentication,
open/close and probe suites were also run.

Implementation inputs: the current public device-session, legacy ping,
geometry, version, boot initializer and Linux board/runtime sources and their
synthetic tests. Public `395425c` code was consulted to compare lifecycle
behavior; the new path composes current implementation components. No vendor
executables, firmware contents, disassembly or captured fingerprints were read.
The existing firmware was supplied unchanged to the verified loader.

## Shared legacy profiles

Fast open also qualifies FT9338 (88×88), FT9348 (96×96) and FT9536 (64×128).
They share FT9361's readiness and application registers and ordinary legacy
initializer. Qualification uses each profile's existing geometry and expected
firmware/AGC pair: FT9338 `40/10`, FT9348 and FT9361 `30/31`, FT9536 `23/13`
(hexadecimal). Geometry distinguishes FT9348 from FT9361. Every open reads
these values afresh. Verified, model-bound firmware remains required.

Synthetic session tests cover successful startup and capture, mismatches in
both readiness bytes and every geometry/version field, missing or wrongly
bound firmware, fatal I/O and explicit cold recovery on all four profiles.
Linux runtime tests cover the verified loader, cancellation, stale device
generations and resource cleanup for each profile. FT9368, FW9369, FT9365 and
FT9769 continue to use full initialization.

FT9338, FT9348 and FT9536 have no fast-open hardware measurements yet. To
validate one, run `sudo ./scripts/fte3600-tool.sh bench 3` with fprintd stopped,
then restore fprintd and test enrollment and verification. Repeat after a
reboot and suspend/resume. Report the commit, chip/profile, first and repeated
open times, and results in [#16](https://github.com/SamSeven777/libfprint-fte3600/issues/16).
A running application can qualify after a reboot or resume; otherwise the
same open falls back to full initialization. The FT9361 timings above do not
predict timings on another chip.
