# FW9369 release-time baseline maintenance

The working tree based on `395425c` adds normal post-capture baseline
maintenance to address a confirmed gap relative to Windows. It is a candidate
fix for issue #2, not confirmation that baseline drift caused the hardware
failure. No physical GPD Pocket 3 is attached to the test environment.

## Evidence and scope

The baseline flow and RVAs refer to the same Windows DLL documented in
[Wire Protocols](protocols.md#baseline-maintenance-after-release). The changed
backend confirms UP with three manual samples, stages an updated FDT baseline,
and performs a background image scan bracketed by manual checks. It retains
the existing DOWN/UP thresholds and does not add resets, sleep cycles, delays,
or repeated initialization to successful capture/release cycles.

Linux commits both baselines only after the complete check succeeds. This is
an explicit adaptation to asynchronous cancellation and I/O failures. It uses
the known four-channel default; it does not claim every vendor calibration,
bad-pixel, wrong-baseline, or lifecycle branch has been reproduced.

INVALID diagnostics capture the latched raw FDT channels and saved baseline
before recovery changes the operating mode. These aggregate readings and DAC
values are sufficient to distinguish some baseline excursions in a future
report without requesting fingerprint images or stored templates.

## Regression coverage

The backend fixture adds 22 cases to the existing 103:

- Eight capture/release cycles in each DB/SMIC process variant, with increasing
  FDT and image background levels and repeated enrollment stage numbers to
  represent rejected placements. Capture remains usable beyond the original
  image background range; no C6 initialization, GPIO reset or sleep is added.
- False/noisy UP and a full or partial finger returning before or after the
  background scan. The same UP threshold must hold on all four channels.
- SPI failure and cancellation at each of the five manual samples, checking
  that neither baseline is partially replaced and temporary image data is
  cleared during cleanup.
- Isolated image spikes and decreasing background values do not contaminate
  the normal upward baseline update.
- Diagnostic read failure/cancellation after INVALID cannot leave the previous
  calibration marked usable.

Existing recovery tests additionally verify the new diagnostic read occurs
before INVALID is acknowledged. Protocol framing, transport generation checks,
cleanup, prearmed release latches, and authentication lifecycle checks remain
part of validation.

## Local results

All 125 FW9369 backend cases passed in each of these WSL Ubuntu builds, with
warnings treated as errors:

| Build | Configuration | Validation |
| --- | --- | --- |
| `build-issue2-lto` | Personal authentication enabled, IPA disabled, LTO enabled | Backend, protocol, SPI transfer, transport lifecycle, authentication lifecycle: 5 groups passed |
| `build-fte3600-ci-false` | Personal authentication and IPA disabled | Backend, protocol, transport lifecycle passed; final backend rerun passed |
| `build-audit-main-sanitize` | Personal authentication and IPA enabled, ASan/UBSan | Backend, protocol, authentication lifecycle passed; final backend rerun passed |

Sanitizer runs used `ASAN_OPTIONS=detect_leaks=0` and
`UBSAN_OPTIONS=halt_on_error=1`. Logs are in each build's `meson-logs` directory.
`git diff --check` passed. These tests validate synthetic device behavior and
failure handling, not the physical source of issue #2's INVALID events.

## Hardware acceptance

Repeat enrollment for more than four captures, including rejected placements,
and repeat verification/open/close. If INVALID recurs, collect the lines
`INVALID latched detector`, `release recheck rejected`, and
`release baseline committed`, along with the existing recovery messages.

Persistent INVALID recovery still uses the prior bounded initialization path.
If a finger remains present during that calibration it can still fail; normal
release maintenance does not establish that an arbitrary recovery starts with
an uncovered sensor. The tester's negative reset experiments are not evidence
for adding another reset or a longer delay.
