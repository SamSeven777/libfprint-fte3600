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
| **FW9369** (ID 9362) | 64 × 80 | Supported (Special C6) | None (ROM mode) | BRISK | **Verified on Hardware** (GPD Pocket 3) |
| **FT9769** (ID 9391/2) | 40 × 196 | Supported (FT9769) | None (ROM mode) | BRISK (Adaptive) | Implemented & Unit Tested |

---

## 2. Tested Device Reports

### GPD Pocket 3 (FW9369 / ID 0x9362)
- **Environment**: Linux Kernel 7.2+, Arch / Omarchy.
- **Hardware Profile**: ACPI `Interrupt(Edge, ActiveLow)`, SPI mode `0x4`.
- **Validation**:
  - Dynamic discovery and C6 negotiation successfully identified raw ID `0x9362`.
  - Background baseline calibration and IRQ finger event capture confirmed.
  - Multi-cycle close/reopen lifecycle verified.

### One-Netbook A1 (FT9361)
- **Environment**: Linux Kernel 6.x / 7.x.
- **Hardware Profile**: A8 protocol with external firmware `ft9361.bin`.
- **Validation**:
  - Successful image capture and host-side authentication verified.

---

## 3. Test Suite & Quality Assurance

Every release is verified using the automated test suite with `-Dwerror=true`:
```bash
ninja -C build-fte3600 test
```
- **Unit & Protocol Tests**: 30 test fixtures covering protocol state machines, frame parsing, unaligned memory safety, popcount math, and lifecycle transitions.
- **CI Status**: 30 Passed, 0 Failed.
