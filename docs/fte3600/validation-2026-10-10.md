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
