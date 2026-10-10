# FT9338 / FT9361 Vendor-Behavior Fixes

Date: 2026-10-10.

## Scope and evidence

This change addresses the seven findings from the FT9338 / FT9361 comparison
against `ftWbioUmdfDriverV2.dll` 2.0.3.102, SHA-256
`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`.
The starting revisions were main `1270b3964d0c79e446ac5ff6d73dc33484c481b7`
and medion-spidev `9133588c273478db2487841810df41fe81d2fe21`.
Addresses below are hexadecimal RVAs in that specific DLL. Firmware data and
vendor implementation code are not added to the repository.

| Finding | Implemented behavior | Vendor evidence |
| --- | --- | --- |
| Wrong continuation mode | FT9338 mode 2, FT9361 mode 1, for both non-finger events and continued enrollment | `2D18F-2D1A2`, `2EA77-2EA83` |
| Version mismatch aborts open | Failed FW/AGC checks enter matching firmware recovery; retryable download/startup failures restart the complete download, at most five attempts | `2C754`, `2C8BC`, `2C4A4` |
| Startup read errors bypass polling | Transient failed reads and busy replies use the same 20-read budget and 2 ms waits, including the last failed read; discovery wake retains its own six-attempt budget | `3695C-36981`, `3987A-398A6`, `28344` |
| Extra cold-configuration prerequisites | Configuration immediately follows startup, without stale geometry/version checks or a fast-open geometry delay; FT9338 omits the preliminary marker read | `36A30`, `399F0` |
| Fatal non-BB configuration marker | Log the successfully read value and continue, including the remaining A8 `22/23` writes; actual configuration transaction errors still fail | `36B6E-36BC2`, `39B83-39BD7` |
| Extra idle rearm stop sequence | Already-idle rearm goes directly to mode selection; the stop sequence remains in busy return-idle | `287D4`, `285CC`, `28668` |
| Successful OTP cleanup reset | Disable OTP and return without the additional child/parent success reset pulses; error cleanup remains | `27650`, `278F4` |
| FT9338 wake ordering | Paired `70` return-idle precedes the decision to recover a known FT9338 application | `2C754`, `28668` |

The configuration finding occupies two table rows. The shared fixes are in
both branches. Medion's `--test-ft9338` and explicit `--boot ft9338` commands
also gain the bounded complete-download retry. Factory/OTP selection happens
before that loop, and configuration/runtime diagnostic failures are outside
it. No incomplete or unverified FT9338 payload is started as error cleanup.

## Regression coverage

The lifecycle suite adds 28 independent synthetic wire/fault cases, covering
both chips: continuation mode, FW and AGC mismatch recovery, non-BB markers,
transient I/O and timeout failures, 100 failed polls across five downloads,
a retryable upload failure, five exhausted upload attempts, generation loss,
cancellation, warm paired-70 wake, nonfatal final status unavailability, and
firmware-version read failure. Poll-delay assertions cover the twentieth
failure; successful startup must reach configuration without an intervening
scheduled delay or old metadata query.

Existing enrollment tests additionally assert the chip-specific mode for every
continued frame. OTP fixtures reject successful cleanup reset pulses. Medion
fixtures check exact normal command traces, the five-attempt bound, recovery
from each injected startup transfer fault, and cancellation/error cleanup.
All payloads and images used here are generated test data.

| Tree / configuration | Result |
| --- | --- |
| main, release + LTO, BRISK-only, warnings as errors; full Meson suite | 30 passed, 33 skipped, 0 failed |
| medion-spidev, BRISK + IPA, warnings as errors; full Meson suite | 37 passed, 33 skipped, 0 failed |
| main, personal authentication disabled; lifecycle, auth lifecycle, B38 recovery, FW9369 backend | 4 passed |
| medion-spidev, personal authentication disabled; lifecycle, auth lifecycle, B38 recovery, both FT9338 startup engines, identify I/O | 6 passed |
| main, AddressSanitizer + UndefinedBehaviorSanitizer, BRISK + IPA; lifecycle and B38 recovery | 2 passed, no sanitizer errors |

The full-suite skips are disabled non-FTE3600 driver fixtures and the udev-hwdb
check in this build environment. They are not passing hardware tests.

Local build directories used: `build-issue2-lto`, `build-medion-sync-true`,
`build-fte3600-ci-false`, `build-medion-sync`, and `build-audit-main-sanitize`.
Their Meson test logs contain the detailed results.

## Remaining integration boundaries

- Main still requires positive current-session ROM identity before a firmware
  write. It does not adopt Windows' broader Boot-A/B38 default classifications,
  nor turn OTP `00` or family `1534` alone into a unique FT9338 identity.
  Medion's explicit FT9338 experiment has a separate selection contract.
- Generation validation, cancellation, safe reset release on failure, IRQ/event
  handling and terminal-action idle cleanup remain Linux lifecycle behavior.
  The shared backend retains a best-effort final MCU read: transient failure
  leaves idle unverified, and arming performs its own status check.
- Medion startup diagnostics still verify runtime geometry and versions after
  configuration. These assertions make the experiment's result reviewable;
  they are not presented as vendor initialization prerequisites.
- The existing tight release-to-`55 AA` transfer preparation, firmware payloads,
  reset polarity, per-chip startup waits and image framing are unchanged.
  Separate GPIO/SPI system calls do not provide physical atomicity.

These results validate software control flow and mocked wire behavior against
one inspected Windows build. This patch has not been run on a physical A1 or
E3224. Earlier successful hardware runs do not constitute validation of these
new changes, and the project does not claim complete Windows equivalence.

## Follow-up: completion before IRQ wait installation

The post-push audit of main `be663cf` and medion-spidev `b077888` reproduced
an additional race: a mode-2 acquisition could complete before the post-arm
MCU read. The old arm state treated idle as failure, drained the fresh IRQ,
and retriggered capture. Three such completions produced a timeout with no
image read. The old fixture only delivered IRQs after wait installation.

The first uncommitted fix retained completions but a further DLL comparison
found three remaining differences. The revised implementation addresses them:

| Condition | Revised behavior | Vendor evidence |
| --- | --- | --- |
| MCU already idle after arm | Write mode zero, retaining the queued IRQ without another capture trigger | `287D4`, `289CE-289DB`, `285CC` |
| Next enrollment frame has no IRQ | Deliver the already-read preceding frame before waiting for the next IRQ | `2E990-2EACC` rearms but does not await the next IRQ before completing the image request |
| FT9361 warm initialization | Use mode-dependent ReturnIdle before the version decision; retain a separate A8 cold-start tail | `2C7F9`, `28668-287D3`, `3987A-398A6` |

The pending completion stays in the kernel IRQ queue while the previous image
is processed. The next capture validates the session, consumes the fresh IRQ,
checks MCU/finger status and reads the image. If an idle post-arm outcome has
no IRQ, a one-second host timeout starts only when its image is requested.
It does not consume the matcher processing budget or gate the previous frame.
The capture owns this timer and destroys it on IRQ, cleanup and teardown.
Reset clears pending arm state, and a new arm drains stale events. A latched
positive finger status alone still cannot produce an image.

Warm initialization of both chips uses `70`, 5 ms, `70`, then reads the work
mode. Modes 1/unknown stop `1E` then `1F` and wait 10 ms; modes 2/3/4 skip
these writes. A8's post-upload hardware startup still uses its paired `70`,
2 ms settle and bounded MCU polling, without the warm-entry stop sequence.
The firmware payloads and dedicated Medion startup engines are unchanged.

Twenty-seven lifecycle cases are added relative to the pushed revisions:
fifteen IRQ/enrollment cases and twelve warm-entry traces. The latter assert
actual command bytes and requested timing for both chips in modes 0/1/2/3/4/FF.
The IRQ cases assert mode-zero writes before image reads, one trigger per
acquisition, cancellation/generation/timeout cleanup, and session reuse.
For both chips, the missing-next-IRQ test requires one reported enrollment
stage before timing out. Another test withholds each next IRQ until the
previous frame's progress callback, detecting any dependency on the next
event before reporting the preceding image. Existing cold-start and upload
tests continue to exercise the separate firmware/startup paths.

These tests use generated data and simulated I/O. Hardware validation remains
pending; no claim of complete Windows equivalence follows from these fixes.

Revised follow-up validation:

| Configuration | Result |
| --- | --- |
| main, BRISK-only release/LTO, warnings as errors; full suite | 30 groups passed, 33 skipped, 0 failed; lifecycle 214 cases passed |
| medion-spidev, BRISK+IPA, warnings as errors; full suite | 37 groups passed, 33 skipped, 0 failed; lifecycle 227 cases passed |
| main, BRISK+IPA, AddressSanitizer and UndefinedBehaviorSanitizer | Lifecycle passed, no sanitizer errors |
| main, personal authentication disabled | Lifecycle passed |

The skips have the same reasons as above. The final test-only adjustment
allows cancellation or generation loss to interrupt a mode-zero write before
completion; successful image reads still require that write. Both branches'
IRQ/enrollment regression groups were rebuilt and rerun after that adjustment.
