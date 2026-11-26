# Fast-open validation

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

## FW9369 test branch: identity reuse with fresh calibration

The [older Pocket 3 report](https://github.com/SamSeven777/libfprint-fte3600/issues/2#issuecomment-6006608845)
contains a patch against `549767a`. That backend retained identity, glue path,
generation and CS polarity across close, then ran normal initialization. Its
C1 deep sleep made the next direct ID read return zero; the contributor fixed
that by resetting before initialization. Repeated opens measured 1.38–1.39 s,
with verification and S3 resume reported working. A
[later report](https://github.com/SamSeven777/libfprint-fte3600/issues/2#issuecomment-6212129012)
measured approximately 3.9 s from lock-screen authentication start to the PAM
prompt on `6efa376`, compared with 1.4–2 s on `1270b39`.

Current main uses separate Linux runtime/session objects and reconstructs its
calibration state on every open. Close frees those objects and releases the
board resources; it does not implement the old backend's C1 shutdown command.
The old SSM patch therefore cannot be applied directly. This branch ports its
identity-reuse and reset-before-ID ordering, retaining full fresh calibration:

- A successful FW9369 open records only the SPI/GPIO/UIO pair, generation,
  ACPI mode, CS capability and working CS polarity in the owning FpDevice.
- A healthy close releases every hardware handle. A subsequent normal open
  with the same pair and generation restores the confirmed CS polarity,
  performs the existing H10/L20/H reset plus 10 ms settling, and reads SRAM
  `0x1a8b` twice. Both replies must be `0x9362`.
- Confirmed identity skips the generic byte/word/legacy family scanner.
  The complete existing FW9369 startup, SPI negotiation, FDT calibration,
  image baseline and automatic Down arm still run with new host state.
- A missing/stale hint or different live ID uses full discovery. Transport,
  cancellation and stale-generation errors terminate the open. An alternate
  CS retry uses a new cold session at the opposite current polarity. Calibration failure never triggers a
  speculative fast retry.
- PR #13's bounded calibration recovery is included. Failed actions,
  cancellation and failed close invalidate the hint. Recovery reopens remain
  cold and retain the remove-finger / recalibrate / touch-again sequence.

This differs from the [13 ms experiment in #12](https://github.com/SamSeven777/libfprint-fte3600/issues/12):
it does not keep a calibrated runtime open across close, or retain calibration
buffers. That experiment was withdrawn after higher idle package power and
cancellation limitations.

### Pocket 3 hardware feedback

@iwansugiarto [tested `453dc03` against `18147ef`](https://github.com/SamSeven777/libfprint-fte3600/pull/17#issuecomment-7183847268)
on a GPD Pocket 3 (G1621-02, BIOS 2.04), Arch/Omarchy, kernel
`7.2.15-4-omarchy`, fprintd 1.94.5. Benchmark opens were 3749/3712/3716 ms
on the baseline and 3715/591/592 ms on the candidate. In the installed
candidate, repeated claims took 0.61–0.71 s; a new process still opened cold
in 3.75–3.86 s.

Enrollment completed 8/8, including release-timeout recovery retaining 5/8
and 6/8 stages. A newly enrolled template matched 5/5, and lock-screen
authentication worked. Cancellation invalidated the hint as expected.
The older template matched 3/7 before re-enrollment; the cause is unresolved.
The calibration-failure branch of #13 and idle power remain untested.

Verification pending across suspend produced fprintd Release/Claim errors
and an open-device cleanup warning. This was not compared with the baseline.
The existing framework cancels active operations when suspend is unsupported
and waits for driver completion. It forbids public close until resume;
`fp_device_is_open()` remains true until the client closes the public device,
even after the driver has released its hardware runtime.

The public API regression covers verification blocked in capture or release,
hardware cleanup before suspend returns, the expected busy close during
suspend, and resume/close/reopen/verification in the same process. It passes
with the existing cancellation fallback. This does not reproduce fprintd's
D-Bus client ordering or establish that the reported suspend issue is fixed.
For a retest, include timestamped fprintd/debug and client logs covering
suspend, completion of verification cancellation, Release, resume and Claim;
compare the baseline with the candidate. Do not infer working suspend from
a successful cold open in a new daemon.

The reported LTO probe-test failure was reproduced, along with a benchmark
test linker failure on GCC 13. Both embedded CLI tests now bind their calls
directly to test mocks, avoiding LTO's bypass of linker `--wrap`. Production
runtime code and LTO remain unchanged.

### Hardware test

Use `feature/fw9369-fast-open`. Its merge commit `18147ef` combines main
`96997d7` with PR #13 (`4decc0c`) and is the comparison baseline before the new
optimization. Build the baseline and branch head with the same options from
[the installation guide](install.md), recording each full commit. Keep the
existing kernel/resource setup and matcher settings for both runs.

With other sensor clients and fprintd stopped, run from each built checkout:

```sh
git rev-parse HEAD
uname -r
sudo ./scripts/fte3600-tool.sh bench 3
```

Restore fprintd after benchmarking. The first open in each new benchmark
process has no identity hint and takes the ordinary path; cycles two and three
exercise reuse. After reboot and suspend/resume, repeat the benchmark. A new
benchmark process always starts without a hint. To test generation invalidation
within one FpDevice, keep the same fprintd process running across suspend and
perform verification before and after it.

For enrollment, verification and PAM testing, install the candidate library
using the normal installation steps and restart fprintd so it loads that build.
Report first/repeated open times, repeated positive verification, enrollment,
lock-screen/PAM behavior, and suspend/resume. Also retest #12's early-touch
recovery and cancellation followed by another attempt. Confirm that accepted
enrollment stages survive a recovery retry. Compare idle power after closing
with the baseline if power measurements are available.

The production-linked synthetic tests replay the existing full calibration
transcript after the new reset and two ID reads, then capture an image. They
also cover first/second ID mismatch, generation/pair/mode invalidation, cold
recovery, I/O failure, cancellation, stale reset and CS restoration. Public API
tests cover healthy-close hint reuse, invalidation after failure/cancellation,
and PR #13 enrollment recovery. A cached-CS regression reproduced failure
before switching the retry from the ACPI-derived polarity to the opposite
current polarity. Hardware results belong in the test PR and #16.
