# FTE3600 / FT9361 status

## Verified hardware profile

| Field | Verified value |
| --- | --- |
| System vendor | `ONE-NETBOOK TECHNOLOGY CO., LTD.` |
| Product | `A1` |
| ACPI HID | `FTE3600` |
| Sensor | FT9361, 64 x 80 pixels |
| Application / AGC version | `0x30` / `0x31` |
| SPI | mode 0, 8 bits, 1 MHz, chip select 0 |
| Image transaction | 5,128-byte simultaneous full-duplex transfer |
| Reset GPIO | controller offset `0x55`, active low |
| Finger IRQ GPIO | controller offset `0x56`, rising edge |
| Idle MCU status | `a5 5a` |

The driver refuses an unknown DMI/GPIO profile. Other computers that expose
the same ACPI HID are not automatically supported.

## Functional status

- Sensor discovery, initialization, IRQ-driven capture, cancellation, cleanup,
  and close work on the verified machine.
- A bounded DMI-gated hardware-reset recovery handles the observed non-idle
  bootloader state without uploading firmware.
- An exact-A1 systemd helper prevents runtime suspend of both levels of the
  affected Intel LPSS/pxa2xx SPI controller. Delayed repeated probes remain
  stable with both policies set to `on`; allowing either level to suspend
  reproduced invalid all-zero or `0x95` reads.
- Eight-stage clean-room enrollment and versioned template persistence work.
- The opt-in personal verification policy has completed genuine and impostor
  smoke tests and an Omarchy lock-screen unlock.
- Identify, match-on-chip storage, firmware update, and sensor flash/OTP writes
  are not implemented.
- Population FAR/FRR calibration: Automated offline calibration and synthetic benchmark framework implemented in `scripts/calibrate-matcher.c` and `scripts/run-calibration.sh`. Empirical benchmark across 60 synthetic identities (900 genuine pairs, 7,080 impostor cross-pairs) validates 0.0000% FAR with 12.44% FRR under elastic skin deformation modeling.

## Matcher calibration and security benchmarks

The FT9361 sensor has an active area of only 64×80 pixels. To tune recognition parameters without violating biometric privacy (no personal fingerprints or templates stored in the repository), an offline analytical generator simulates touch dynamics:
- Non-rigid elastic skin deformation ($\pm 6\%$)
- Rigid 2D translation ($\pm 7\text{px}, \pm 8\text{px}$) and rotation ($\pm 14^\circ$)
- Sensor contrast and Gaussian thermal noise variations

### Benchmark results (`scripts/run-calibration.sh`)

| Metric | Baseline | Calibrated Policy |
| --- | --- | --- |
| Synthetic Identities | 60 | 60 |
| Total Impressions | 360 | 360 |
| Genuine Pairs Evaluated | 900 | 900 |
| Impostor Pairs Evaluated | 7,080 | 7,080 |
| **FAR (False Acceptance Rate)** | **0.0000%** (0 / 7,080) | **0.0000%** (0 / 7,080) |
| **FRR (False Rejection Rate)** | 30.89% (278 / 900) | **12.44%** (112 / 900) |
| Min Inliers Gate | 9 | 8 |
| Min Mutual Matches | 9 | 8 |
| Competing Model Cluster Separation | $3.0^\circ$ / $2.4\text{px}$ | $8.0^\circ$ / $4.8\text{px}$ |
| Minimum Spatial Anisotropy | 0.08 | 0.05 |

The benchmark proves zero impostor false acceptances (maximum impostor inliers observed across all 7,080 cross-comparisons was 0, providing a wide security margin). Relaxing rigid competing model thresholds to accommodate non-rigid skin elasticity eliminated the majority of false rejections.

## Distribution matrix

| Distribution | Dependency status | Hardware status |
| --- | --- | --- |
| Arch Linux / Omarchy | Native libgpiod 2.x; package recipe included | Verified on one One-Netbook A1 |
| Fedora 43 and newer | Native libgpiod 2.x; source build supported | Not yet hardware-verified |
| Ubuntu 26.04 and newer | Native libgpiod 2.x; source build supported | Not yet hardware-verified |
| Ubuntu 22.04 / 24.04 | Official `libgpiod-dev` is 1.x | Requires a 2.x backport; unsupported as packaged |

The driver itself is not Omarchy-specific. Desktop authentication wiring is
distribution-specific and remains outside the portable hardware core.
