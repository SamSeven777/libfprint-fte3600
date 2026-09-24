# Clean-Room Implementation & Upstream Architectural Rationale

This document outlines the legal provenance, reverse-engineering methodology, and architectural rationale for the `fte3600` driver in `libfprint`. It is intended to accompany upstream code review and downstream security audits.

---

## Clean-Room Provenance & Legal Basis

The `fte3600` driver and its integrated host-side biometric matcher are an independent, clean-room implementation released under the **GNU Lesser General Public License version 2.1 or later (LGPL-2.1-or-later)**:

- **Zero Proprietary Code Execution**: The driver executes entirely in native Linux user space (`libfprint` / `fprintd`). It does not run vendor binaries, execute Windows DLL/ELF binaries, or invoke proprietary dynamic libraries.
- **Protocol Discovery**: Register framing, SPI command sequences, and the volatile SRAM firmware injection protocol were deduced solely via non-invasive bus transport analysis (USB/SPI capture of public Windows driver exchanges) and black-box hardware validation on physical devices.
- **No Proprietary Formats**: The driver does not read, write, or convert vendor biometric templates or vendor descriptor formats. All feature extraction, spatial filtering, and template serialization are implemented from published mathematical algorithms.
- **Excluded Vendor Artifacts**: The repository explicitly excludes proprietary Windows binaries, decompiler listings, and private fingerprint image/template databases.

---

## Architectural Rationale: Why `FpDevice` Instead of `FpImageDevice`?

`libfprint` traditionally separates drivers into two primary classes:
1. `FpImageDevice`: Designed for imaging sensors where raw frames are passed to libfprint's built-in NBIS (Bozorth3) minutiae matcher.
2. `FpDevice`: Designed for "match-on-chip" smart sensors that perform biometric enrollment and verification in hardware firmware.

The FocalTech FT9361 sensor is a **raw image sensor**, but the `fte3600` driver deliberately derives from `FpDevice`:

### 1. The Minutiae Starvation Problem on 64 × 80 Sensors
The FT9361 has an exceptionally small active sensing area of **64 × 80 pixels** (approximately 3.2 mm × 4.0 mm). Standard forensic fingerprint algorithms like NBIS (NIST Biometric Image Software):
- Require a typical contact area of at least 256 × 256 or 500 DPI over a large finger surface.
- Rely on detecting a minimum of 12 to 20 reliable ridge minutiae (ridge endings and bifurcations).
- On a 64 × 80 surface, NBIS detects an average of **fewer than 4 reliable minutiae**, leading to complete failure of the Bozorth3 matching pipeline (FRR near 100%).

### 2. Host-Side Geometric Consensus Matcher
To make the FT9361 functional on Linux, the driver implements a custom host-side matcher based on dense scale-space keypoints and geometric consensus:
- **Feature Extraction**: Gaussian/Difference-of-Gaussians (DoG) scale-space feature extraction capable of locating 20–40 stable keypoints even in small contact areas.
- **Descriptors**: Orientation-normalized binary descriptors (BRISK) evaluated over deterministic concentric sampling patterns.
- **Verification Consensus**: Rigid/affine RANSAC geometric consensus with strict spatial span, bounding box, and residual error bounds (Policy Version 3).

Because `FpImageDevice` in `libfprint` does not support custom host matchers or versioned keypoint template serialization, deriving from `FpDevice` allows the driver to manage the full capture-to-match lifecycle directly, fulfilling libfprint's public D-Bus API without altering the core library matcher architecture.

---

## Algorithmic Foundation & Literature References

All biometric processing algorithms in `fte3600` are based on established, published academic literature:

1. **Scale-Space Extrema Detection (DoG)**:
   - Lowe, D. G. (2004). *Distinctive Image Features from Scale-Invariant Keypoints*. International Journal of Computer Vision, 60(2), 91–110. [DOI: 10.1023/B:VISI.0000029664.99615.94](https://doi.org/10.1023/B:VISI.0000029664.99615.94)
2. **Binary Robust Invariant Scalable Keypoints (BRISK)**:
   - Leutenegger, S., Chli, M., & Siegwart, R. Y. (2011). *BRISK: Binary Robust Invariant Scalable Keypoints*. IEEE International Conference on Computer Vision (ICCV), 2548–2555. [DOI: 10.1109/ICCV.2011.6126542](https://doi.org/10.1109/ICCV.2011.6126542)
3. **Random Sample Consensus (RANSAC)**:
   - Fischler, M. A., & Bolles, R. C. (1981). *Random Sample Consensus: A Paradigm for Model Fitting with Applications to Image Analysis and Automated Cartography*. Communications of the ACM, 24(6), 381–395. [DOI: 10.1145/358669.358692](https://doi.org/10.1145/358669.358692)

---

## Firmware Distribution & Integrity Model

- **Volatile SRAM Injection**: The FT9361 sensor lacks non-volatile flash memory for its MCU runtime code. When main power is removed (cold boot), the sensor reverts to a raw bootloader state (`00 00` idle code). The driver restores functionality by uploading a 10,396-byte runtime microcode block over SPI.
- **Zero-Binary Repository Policy**: To respect third-party copyright, the microcode binary is not distributed in this Git repository.
- **Automated Upstream Extraction**: The included script [`./scripts/install-firmware.sh`](../../scripts/install-firmware.sh) downloads the officially signed vendor driver CAB directly from the Microsoft Update Catalog, extracts the payload at a verified byte offset, validates its SHA256 checksum, and installs it to `/usr/lib/firmware/fte3600/ft9361.bin`.
- **Integrity Guarantee**:
  - Size: Exactly `10,396` bytes.
  - Expected SHA256: `027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f`.
  - Cold-boot recovery checks size and hash before initiating any SPI transfer.

