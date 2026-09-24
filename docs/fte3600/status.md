# FTE3600 / FT9361 Hardware & Feature Status

This document details supported hardware profiles, verified driver capabilities, and biometric calibration benchmarks for the FocalTech FT9361 / FTE3600 driver in `libfprint-fte3600`.

---

## Hardware Support Matrix

| Parameter | One-Netbook A1 | GPD Pocket 3 (Jasper Lake) | GPD Pocket 3 (Tiger Lake) | Medion Akoya E3224 |
| :--- | :--- | :--- | :--- | :--- |
| **Branch** | `main` / `upstream-submission` | `main` / `upstream-submission` | `main` / `upstream-submission` | `medion-e3224` |
| **Verification** | **Fully Verified (Hardware)** | **Supported** | **Supported** | *In Progress (Pending HW test)* |
| **DMI `sys_vendor`** | `ONE-NETBOOK TECHNOLOGY CO., LTD.` | `GPD` | `GPD` | `MEDION` |
| **DMI `product_name`** | `A1` | `Pocket 3` / `G1621-02` | `Pocket 3` / `G1621-02` | `Akoya E3224` / `E3224` |
| **ACPI Device ID** | `FTE3600` | `FTE3600` | `FTE3600` | `FTE3600` |
| **Sensor IC** | FocalTech FT9361 (64 × 80 px) | FocalTech FT9361 (64 × 80 px) | FocalTech FT9361 (64 × 80 px) | FocalTech FT9361 (64 × 80 px) |
| **SPI Configuration**| Mode 0, 8-bit, 1 MHz | Mode 0, 8-bit, 1 MHz | Mode 0, 8-bit, 1 MHz | Mode 0, 8-bit, 1 MHz |
| **GPIO Controller** | `INT3453:00` (`\_SB_.PCI0.GPI0`) | `INT34C8:00` (`\_SB_.GPI0`) | `INT3455:00` (`\_SB_.GPI0`) | `INT3452:00` |
| **Reset GPIO Pin** | Offset 85 (`0x55`, ActiveLow) | Offset 211 (ActiveLow) | Offset 179 (ActiveLow) | Offset 40 (ActiveLow) |
| **IRQ GPIO Pin** | Offset 86 (`0x56`, ActiveHigh) | Offset 56 (ActiveHigh) | Offset 24 (ActiveHigh) | Offset 39 (ActiveHigh) |
| **SPI Buffers** | 5,128 B (image) / 10,403 B (fw)| 5,128 B (image) / 10,403 B (fw)| 5,128 B (image) / 10,403 B (fw)| 5,128 B (image) / 10,403 B (fw)|
| **Ready MCU Status** | `a5 5a` | `a5 5a` | `a5 5a` | `a5 5a` |

> [!NOTE]
> The driver enforces a strict **fail-closed** policy during ACPI/DMI device probe. If a machine presents ACPI ID `FTE3600` but does not match a verified DMI vendor/product pair, probe aborts immediately (`-ENODEV`) to prevent driving incorrect GPIO lines.

---

## Verified Driver Capabilities

Tested on reference hardware (**One-Netbook A1**, Linux kernel 6.x, `libgpiod` 2.x):

- **Device Lifecycle & Power Management**:
  - Full probe filtering, device open, sensor reset, and clean resource deallocation.
  - Asynchronous transfer cancellation support for enrollment, verification, and image capture.
  - Safe operation across kernel runtime power management and system suspend/resume cycles.
- **Image Acquisition**:
  - Direct full-duplex SPI streaming of single-frame 64 × 80 pixel raw capacitive images (5,128 bytes wire transfer).
  - Background noise floor cancellation and contrast normalization.
- **Cold-Boot SRAM Recovery**:
  - Automatic detection of uninitialized/cold-boot sensor state (`00 00` idle status).
  - Volatile injection of verified 10,396-byte microcode payload directly into sensor SRAM over SPI.
- **Biometric Processing Pipeline (Policy Version 3)**:
  - 8-stage interactive enrollment with real-time image quality evaluation.
  - Adaptive rejection of low-contrast presses, duplicate touches, and zero-displacement samples.
  - Serialization into versioned binary biometric templates (`/var/lib/fprint/`).

---

## Biometric Matcher Calibration (Policy Version 3)

The FT9361 sensor has an active area of only 64 × 80 pixels (approximately 3.2 mm × 4.0 mm). Standard minutiae-based algorithms (such as NBIS / Bozorth3) typically fail due to insufficient minutiae count.

The host matcher employs a custom implementation of BRISK keypoint detection, orientation-normalized binary descriptors, and rigid/affine RANSAC geometric consensus gating.

### Calibration Parameters

| Metric / Parameter | Value | Rationale |
| :--- | :--- | :--- |
| **Minimum Mutual Matches** | `>= 5` | Rejects candidate pairs lacking sufficient structural feature overlap. |
| **Minimum RANSAC Inliers** | `>= 5` | Ensures geometric consistency across keypoint spatial arrangements. |
| **Inlier Ratio** | `>= 0.20` (20%) | Eliminates false consensus from sparse random keypoint clustering. |
| **Geometric Residuals** | `median < 1.25 px`, `rms < 1.40 px` | Enforces rigid transformation bounds between query and template. |
| **Spatial Span** | `x_span >= 6.0 px`, `y_span >= 8.0 px` | Requires inliers to span a physical area rather than a single cluster. |
| **Spatial Cell Coverage** | `>= 2` distinct grid cells | Prevents repetitive texture artifacts from passing consensus. |

### Evaluation Metrics

Policy Version 3 was evaluated against an offline test matrix of **342,720 pairwise comparisons** (incorporating synthetic affine/spatial perturbations, sensor noise profiles, and cross-session FVC2002 DB3_B datasets):

- **Observed False Accepts**: **0** across all 342,720 non-matching sample evaluations under controlled benchmark conditions.
- **Observed False Rejections**: Stable within operational interactive retry budgets when proper multi-angle enrollment is performed.

> [!WARNING]
> Multi-person, multi-session population FAR/FRR metrics across the full 8-subtemplate authentication pipeline have not been independently certified by a biometric evaluation laboratory. Biometric verification is intended strictly for personal desktop experiments with a password fallback.

---

## Related Documentation

- [Installation Guide](install.md) — Detailed build, dependency, and configuration instructions.
- [Clean-Room Provenance & Upstream Rationale](clean-room.md) — Architectural rationale for `FpDevice` derivation and clean-room provenance.
- [Troubleshooting](troubleshooting.md) — Diagnostic steps for common initialization or permission issues.

