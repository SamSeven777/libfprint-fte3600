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
| Firmware transaction | 10,403-byte upload with chip select held throughout |
| Reset GPIO | controller offset `0x55`, active low |
| Finger IRQ GPIO | controller offset `0x56`, rising edge |
| Idle MCU status | `a5 5a` |

The driver refuses an unknown DMI/GPIO profile. Other computers that expose
the same ACPI HID are not automatically supported.

## Functional status

- Sensor discovery, initialization, IRQ-driven capture, cancellation, cleanup,
  and close work on the verified machine.
- A bounded DMI-gated hardware reset is followed, if needed, by one FT9361
  firmware upload and restart. The corrected standalone sequence recovered
  `00 00` to `a5 5a`; three real-driver open/close cycles passed. The owner
  subsequently confirmed successful initialization and fingerprint recognition
  after a cold boot directly into Linux. The normal journal does not establish
  whether that boot exercised the upload branch.
- An exact-A1 systemd helper prevents runtime suspend of both levels of the
  affected Intel LPSS/pxa2xx SPI controller. Delayed repeated probes remain
  stable with both policies set to `on`; allowing either level to suspend
  reproduced invalid all-zero or `0x95` reads.
- Eight-stage clean-room enrollment and versioned template persistence work.
- The opt-in personal verification policy has completed genuine and impostor
  smoke tests and an Omarchy lock-screen unlock.
- Identify, match-on-chip storage, arbitrary firmware updates, and sensor
  flash/OTP writes are not implemented. Recovery accepts only the pinned
  FT9361 firmware supplied locally by the owner.
- Synthetic matcher benchmarks are implemented in
  `scripts/calibrate-matcher.c` and `scripts/run-calibration.sh`. The recorded
  run across 100 generated identities reported 0 accepts in 30,000 impostor
  comparisons and 330 rejects in 2,800 genuine comparisons (11.79%). These
  generated patterns do not establish population FAR/FRR or authentication
  security on real fingerprints.

## Synthetic matcher benchmarks

The FT9361 sensor has an active area of only 64×80 pixels. An offline generator
uses mathematical patterns to exercise matcher behavior without storing
personal fingerprints or templates in the repository. Its modeled variations
include:

- Non-rigid elastic skin deformation ($\pm 10\%$)
- Multi-condition touch variations: dry skin, sweaty/conductive skin, angular tilt ($\pm 22^\circ$), and off-center placement ($\pm 11\text{px}$)
- Sensor contrast and Gaussian thermal noise variations

### Benchmark results (`scripts/run-calibration.sh`)

| Metric | Baseline | Calibrated Policy |
| --- | --- | --- |
| Synthetic Identities | 60 | 100 |
| Total Impressions | 360 | 800 |
| Genuine Pairs Evaluated | 900 | 2,800 |
| Impostor Pairs Evaluated | 7,080 | 30,000 |
| Synthetic impostor accept rate | 0 / 7,080 | 0 / 30,000 |
| Synthetic genuine reject rate | 30.89% (278 / 900) | 11.79% (330 / 2,800) |
| Min Inliers Gate | 9 | 7 |
| Min Mutual Matches | 9 | 7 |
| Competing Model Cluster Separation | $3.0^\circ$ / $2.4\text{px}$ | $8.0^\circ$ / $4.8\text{px}$ |
| Minimum Spatial Anisotropy | 0.08 | 0.05 |

The recorded run observed no accepted synthetic impostor comparisons; this
does not prove a zero false-accept rate or a security margin for real users.
The reported improvements for heavy shear (72% $\to$ 85% pass) and off-center
placement (87% $\to$ 95% pass) describe generated benchmark conditions only.
Independent, multi-person, multi-session calibration remains outstanding;
the experimental authentication restrictions in [SECURITY.md](../../SECURITY.md)
still apply.

## Distribution matrix

| Distribution | Dependency status | Hardware status |
| --- | --- | --- |
| Arch Linux / Omarchy | Native libgpiod 2.x; package recipe included | Verified on one One-Netbook A1 |
| Fedora 43 and newer | Native libgpiod 2.x; source build supported | Not yet hardware-verified |
| Ubuntu 26.04 and newer | Native libgpiod 2.x; source build supported | Not yet hardware-verified |
| Ubuntu 22.04 / 24.04 | Official `libgpiod-dev` is 1.x | Requires a 2.x backport; unsupported as packaged |

The driver itself is not Omarchy-specific. Desktop authentication wiring is
distribution-specific and remains outside the portable hardware core.
