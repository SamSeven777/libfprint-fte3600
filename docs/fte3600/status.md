# FTE3600 / FT9361 Hardware & Feature Status

## Hardware Profiles

| Parameter | One-Netbook A1 | GPD Pocket 3 |
| :--- | :--- | :--- |
| **Status** | **Fully Verified** | **Supported** |
| **DMI sys_vendor** | `ONE-NETBOOK TECHNOLOGY CO., LTD.` | `GPD` |
| **DMI product_name** | `A1` | `Pocket 3` / `G1621-02` |
| **ACPI Device ID** | `FTE3600` | `FTE3600` |
| **Sensor IC** | FocalTech FT9361 (64 × 80 px) | FocalTech FT9361 (64 × 80 px) |
| **SPI Configuration** | Mode 0, 8-bit, 1 MHz | Mode 0, 8-bit, 1 MHz |
| **Reset GPIO** | `\_SB_.PCI0.GPI0` offset `0x55` (ActiveLow) | `\_SB_.GPI0` offset 211 / 179 (ActiveLow) |
| **IRQ GPIO** | `\_SB_.PCI0.GPI0` offset `0x56` (ActiveHigh) | `\_SB_.GPI0` offset 56 / 24 (ActiveHigh) |
| **SPI Buffers** | 5,128 B (image) / 10,403 B (fw) | 5,128 B (image) / 10,403 B (fw) |
| **Ready MCU Status** | `a5 5a` | `a5 5a` |

Unknown hardware profiles fail closed during device probe to prevent invalid GPIO assertions.

## Verified Capabilities (One-Netbook A1)

- **Device lifecycle**: Discovery, probe filtering, opening, cancellation, close, and clean release.
- **Image acquisition**: Single-frame 64 × 80 image capture over full-duplex SPI.
- **Enrollment**: Standard 8-stage enrollment with quality filtering, duplicate rejection, and cross-stage spatial dispersion.
- **Template persistence**: Versioned binary template serialization (Policy Version 3).
- **Cold-boot recovery**: Automatic RAM firmware injection on cold boot when sensor MCU status is `00 00`.
- **Power management**: Seamless operation across kernel runtime-PM and suspend/resume cycles.

## Authentication Calibration & Limitations (Policy Version 3)

The host-side BRISK matcher operates with empirical geometric consensus gates (Policy Version 3)
evaluated against 342,720 offline test comparisons (synthetic spatial perturbations and FVC2002 DB3_B datasets):

- **Observed False Acceptances**: 0 false acceptances across the 342,720 evaluated pairs under controlled test conditions.
- **Minimum inliers & mutual matches**: `5`.
- **Geometric consistency**: Rigid/affine RANSAC with strict error bounds (`median_error < 1.25 px`, `rms_error < 1.40 px`, `inlier_ratio >= 0.20`).
- **Spatial distribution**: Bounding box span (`x_span >= 6.0`, `y_span >= 8.0`), spatial cell coverage (`>= 2`), and descriptor variance bounds.

> [!WARNING]
> Multi-person, multi-session population FAR/FRR metrics across the full 8-subtemplate authentication pipeline
> (where any subtemplate matching authorizes access) remain unmeasured.
> Default builds expose image capture only. Opt-in authentication (`-Dfte3600_personal_auth=true`) is for
> experimental personal use and must not be used for high-assurance security domains.
