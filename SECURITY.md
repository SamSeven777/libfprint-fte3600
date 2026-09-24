# Security & Privacy Policy

This policy outlines the security architecture, hardware gating boundaries, cryptographic integrity guarantees, and vulnerability reporting procedures for `libfprint-fte3600`.

---

## 1. Authentication Security Model

### Dual-Policy Build System
- **Default Build (Capture-Only)**: Standard builds configure `-Dfte3600_personal_auth=false`. In this mode, the driver exposes raw image capture only and disables host-side biometric verification interfaces.
- **Opt-in Authentication**: Host-side biometric verification requires explicitly setting `-Dfte3600_personal_auth=true`.

### Calibration Bounds (Policy Version 3)
Authentication uses geometric consensus gating over BRISK descriptors and rigid/affine RANSAC:
- Minimum of 5 mutual descriptor matches and 5 geometric inliers.
- Strict inlier ratio requirement (`>= 0.20`).
- Error residual bounds (`median_error < 1.25 px`, `rms_error < 1.40 px`).
- Spatial span (`x_span >= 6.0 px`, `y_span >= 8.0 px`) and spatial cell diversity (`>= 2` cells).

### Security Scope & Limitations
In offline evaluation across **342,720 pairwise comparisons** (incorporating synthetic affine perturbations, noise models, and FVC2002 DB3_B datasets), zero false acceptances were observed under controlled test conditions.
However, population-level FAR/FRR metrics across the full 8-subtemplate interactive pipeline have not been independently certified by a formal biometric laboratory. Biometric authentication is intended strictly for personal experiments (e.g. lock screens) and must always have a working root/password fallback. Do not use as the sole factor for mission-critical or multi-user enterprise systems.

---

## 2. Hardware Safety & Fail-Closed Gating

### Fail-Closed DMI Gating
Because the driver directly manipulates Linux kernel GPIO character devices (`/dev/gpiochip*`), asserting incorrect lines could damage motherboard circuitry or interfere with critical platform peripherals.
The driver enforces strict fail-closed gating:
- ACPI and DMI tables (`sys_vendor` and `product_name`) are checked before any GPIO or SPI handle is opened.
- Only verified platforms (**One-Netbook A1** and **GPD Pocket 3** on `main` / `upstream-submission`) are allowed to proceed.
- Unrecognized systems fail probe immediately with `-ENODEV`, preventing any line configuration.

### Volatile SRAM Firmware Injection
- The driver interacts with the sensor MCU strictly through volatile SRAM injection during cold boot.
- The microcode payload is pinned to exactly `10,396` bytes and must match the known SHA256 checksum (`027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f`).
- The driver does not write to persistent flash, OTP (One-Time Programmable) memory, or perform permanent firmware reflashing.

---

## 3. Privacy & Biometric Data Handling

- **Log Sanitization**: Raw biometric image data and microcode transfer payloads are treated as sensitive. They are redacted and truncated in debug logs (`FP_SPI_SENSITIVE`).
- **Buffer Sanitization**: Primary frame buffers and intermediate template structures allocated by the driver are explicitly zeroed upon deallocation.
- **Template Storage**: Biometric templates are managed by `libfprint` and `fprintd`, serialized to root-owned storage at `/var/lib/fprint/` with restricted filesystem permissions (`0700`).

---

## 4. Reporting Vulnerabilities

If you discover a potential security vulnerability (e.g., memory corruption, buffer overflow, GPIO misdirection, or authentication bypass), please report it responsibly:

- **Preferred Method**: Open a report using **GitHub Private Vulnerability Reporting** via the repository's "Security" tab.
- **Diagnostics**: Please provide sanitized reproduction steps, distribution details, and kernel versions.
- **Data Protection**: **Never attach raw biometric images, core dumps containing biometric state, or serialized user templates to bug reports or public discussion threads.**

