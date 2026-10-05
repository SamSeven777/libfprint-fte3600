# FTE3600 Hardware Support and Validation Status

[Documentation Index](README.md) · [Installation Guide](install.md) · [Wire Protocols](protocols.md)

This page tracks implementation status, driver capabilities, and reported device validation results across the FocalTech FTE3600 sensor family.

---

## 1. Chip Implementation Matrix

| Sensor Profile | Geometry | Protocol Support | Firmware Requirement | Matcher Support | Hardware Status |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **FT9338** | 88 × 88 | Supported (Legacy A8) | `ft9338.bin` | BRISK | Implemented & Unit Tested |
| **FT9348** | 96 × 96 | Supported (A8 Family) | `ft9348.bin` | BRISK | Implemented & Unit Tested |
| **FT9361** | 64 × 80 | Supported (A8 Family) | `ft9361.bin` | BRISK / 2D-IPA | **Verified on Hardware** (One-Netbook A1) |
| **FT9536** | 64 × 128 | Supported (Legacy A8) | `ft9536.bin` | BRISK | Implemented & Unit Tested |
| **FT9365** | 64 × 80 | Supported (FT9365) | None (ROM mode) | BRISK | Implemented & Unit Tested |
| **FT9368** | 64 × 80 | Supported (FT9368) | App + Pramboot | BRISK | Implemented & Unit Tested |
| **FW9369** (ID 9362) | 64 × 80 | Supported (Special C6) | None (ROM mode) | BRISK / optional 2D-IPA | **BRISK verified on hardware** (GPD Pocket 3); IPA/fusion awaits hardware validation |
| **FT9769** (ID 9391/2) | 40 × 196 | Supported (FT9769) | None (ROM mode) | BRISK (Adaptive) | Implemented & Unit Tested |

The ordinary authentication build remains BRISK-only. Grand Synergy v3 is an
explicit `-Dfte3600_ipa_auth=true` opt-in for FT9361 and FW9369, with
`-Dfte3600_personal_auth=true` also required. It selects each sensor's template
policy and preserves sensor isolation. Existing BRISK enrollments remain usable
in BRISK or dual mode; re-enrollment is needed to add IPA features. FW9369 IPA
support has synthetic test coverage, not measured accuracy or hardware
validation. See [optional matcher installation](install.md#optional-grand-synergy-v3-matching).

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
