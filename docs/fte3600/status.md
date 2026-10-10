# FTE3600 Hardware Support and Validation Status

[Documentation Index](README.md) · [Installation Guide](install.md) · [Wire Protocols](protocols.md)

This page tracks implementation status, driver capabilities, and reported device validation results across the FocalTech FTE3600 sensor family.

---

## 1. Chip Implementation Matrix

| Sensor Profile | Geometry | Protocol Support | Firmware Requirement | Matcher Support | Hardware Status |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **FT9338** | 88 × 88 | Supported (B38 / Boot-A recovery) | `ft9338.bin` | BRISK | Implemented & Unit Tested |
| **FT9348** | 96 × 96 | Supported (A8 Family) | `ft9348.bin` | BRISK | Implemented & Unit Tested |
| **FT9361** | 64 × 80 | Supported (A8 Family) | `ft9361.bin` | BRISK / 2D-IPA | **Verified on Hardware** (One-Netbook A1) |
| **FT9536** | 64 × 128 | Supported (B38 / Boot-A recovery) | `ft9536.bin` | BRISK | Implemented & Unit Tested |
| **FT9365** | 64 × 80 | Supported (FT9365) | None (ROM mode) | BRISK | Implemented & Unit Tested |
| **FT9368** | 64 × 80 | Supported (FT9368) | App + Pramboot | BRISK | Implemented & Unit Tested |
| **FW9369** (ID 9362) | 64 × 80 | Supported (Special C6) | None (ROM mode) | BRISK / optional 2D-IPA | BRISK and optional IPA tested on GPD Pocket 3; see revision-specific results below |
| **FT9769** (ID 9391/2) | 40 × 196 | Supported (FT9769) | None (ROM mode) | BRISK (Adaptive) | Implemented & Unit Tested |

The ordinary authentication build remains BRISK-only. Grand Synergy v3 is an
explicit `-Dfte3600_ipa_auth=true` opt-in for FT9361 and FW9369, with
`-Dfte3600_personal_auth=true` also required. It selects each sensor's template
policy and preserves sensor isolation. Existing BRISK enrollments remain usable
in BRISK or dual mode; re-enrollment is needed to add IPA features. FW9369 IPA
support has synthetic coverage and a small hardware test report; population
accuracy remains unmeasured. See [optional matcher installation](install.md#optional-grand-synergy-v3-matching).

---

## 2. Tested Device Reports

### GPD Pocket 3 (FW9369 / ID 0x9362)
- **Environment**: Linux Kernel 7.2+, Arch / Omarchy.
- **Hardware Profile**: ACPI `Interrupt(Edge, ActiveLow)`, SPI mode `0x4`.
- **Validation**:
  - The complete factory path identifies raw ID `0x9362`; C6 readback did not
    acknowledge on the reported unit, but the vendor flow treats that result as
    non-gating and the backend validates the identity again during initialization.
  - Background baseline calibration and IRQ finger event capture confirmed.
  - Multi-cycle close/reopen lifecycle verified.
  - Unchanged kernel generations use fast open; suspend/resume forces a complete
    factory rediscovery before the backend is cached again.

The [2026-10-06 report on `549767a`](https://github.com/SamSeven777/libfprint-fte3600/issues/2#issuecomment-6006608845)
found that an unpatched second open in one fprintd process failed after C1 sleep.
With the reporter's factory-reset fix, warm opens took 1.38–1.39 s and verification
after suspend/resume succeeded. This tree integrates that wake sequence with
regressions for retained sleep, reset failure, cancellation, and changed identity;
the integrated revision still needs hardware retesting. Full factory discovery,
including the C6 retries, is unchanged.

In the same report, BRISK and IPA agreed on all seven captures with individual
engine results logged (five matching and two nonmatching fingers). The reported
dual-mode totals were 6/8 genuine attempts accepted and 0/3 other-finger attempts
accepted. These counts do not establish FAR/FRR or an improvement over BRISK.

The [2026-10-09 follow-up on `395425c`](https://github.com/SamSeven777/libfprint-fte3600/issues/2#issuecomment-6078245824)
reports only 2-4 successful captures per session, counting matcher placement
rejections, followed by `IDLE | INVALID` on each touch. The reporter explicitly
retracted the earlier hands-off/spontaneous-event interpretation. Reinitializing
with or without a factory GPIO reset did not restore detection within the
session; a new session after a pause temporarily did. The cause is unconfirmed.

The subsequent working-tree change adds the missing normal release-time
baseline maintenance described in [Wire Protocols](protocols.md#baseline-maintenance-after-release),
plus latched FDT diagnostics before INVALID recovery. Synthetic drift and
failure tests cover the new path; a new GPD Pocket 3 test is still required.
This must not be described as a hardware-verified fix for the reported fault.

### One-Netbook A1 (FT9361)
- **Environment**: Linux Kernel 6.x / 7.x.
- **Hardware Profile**: A8 protocol with external firmware `ft9361.bin`.
- **Validation**:
  - Successful image capture and host-side authentication verified.

---

## 3. Test Suite & Quality Assurance

Run the FTE3600 regression checks with warnings treated as errors:

```bash
./scripts/check-fte3600.sh
```

### 2026-10-06: issue #2 fixes

Local validation used the working tree based on `549767a`, with the FW9369 C1
wake, udev import ordering, and LTO test isolation fixes. GCC 13.3.0 and Meson
1.3.2 ran in WSL Ubuntu with `-Ddrivers=fte3600` and `-Dwerror=true`:

| Personal authentication | IPA authentication | Production LTO | Checks executed | Result |
| :---: | :---: | :---: | :--- | :--- |
| Disabled | Disabled | Disabled | Required script | 28/28 targets passed |
| Enabled | Enabled | Disabled | Required script | 28/28 targets passed |
| Enabled | Disabled | Enabled | Arch PKGBUILD's check list, release build | 25/25 targets passed |

The LTO run tests the package's selected suites on Ubuntu; it is not a native
Arch `makepkg` run. Production objects retain LTO; linker-wrapped test objects
and their private/driver archives use separate non-LTO builds. Both installed
udev rule forms are checked for consistency and import ordering; `udevadm
verify` 255 also accepted the standalone rule. The five new FW9369 cases cover
DB/SMIC fast reopen after C1, reset failure, cancellation, and a changed ID.

Logs are in `meson-logs/testlog.txt` under `build-fte3600-ci-false`,
`build-fte3600-ci-true`, and `build-issue2-lto`. These are synthetic regressions;
no local GPD hardware test was performed.

### 2026-10-05: optional FW9369 matcher

Local validation on 2026-10-05 used the working tree based on `3a36d68`, with
the FW9369 optional matcher changes, GCC 13.3.0 and Meson 1.3.2 in WSL Ubuntu.
All configurations used `-Ddrivers=fte3600` and `-Dwerror=true`.

| Personal authentication | IPA authentication | Checks executed | Result |
| :---: | :---: | :--- | :--- |
| Disabled | Disabled | Required script: 28 test targets | 28 passed |
| Enabled | Enabled | Required script: 28 test targets | 28 passed |
| Enabled | Disabled | 6 matcher/template/lifecycle targets; lifecycle rerun after adding runtime opt-in rejection cases | All passed |
| Enabled | Enabled | Template, family-template and authentication lifecycle under ASan + UBSan | 3 passed |

Local logs are in `meson-logs/testlog.txt` under `build-fte3600-ci-false`,
`build-fte3600-ci-true`, `build-fte3600-brisk-merge` (latest lifecycle run), and
`build-fte3600-acpi-spidev-sanitize`. Counts refer to test targets, not individual
test cases. The new matcher fixtures are synthetic; these results do not
measure hardware reliability, biometric accuracy or FAR/FRR.
