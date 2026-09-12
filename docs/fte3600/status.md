# FTE3600 / FT9361 status

## Hardware profiles

| Model | System vendor | Product | ACPI HID | SPI Bus | Reset GPIO | Finger IRQ GPIO | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| One-Netbook A1 | `ONE-NETBOOK TECHNOLOGY CO., LTD.` | `A1` | `FTE3600` | Mode 0, 1 MHz, CS 0 (`\_SB.PCI0.SPI1`) | Offset `0x55` on `\_SB_.PCI0.GPI0` | Offset `0x56` on `\_SB_.PCI0.GPI0` | Verified on hardware |
| Medion Akoya E3224 | `MEDION` | `E3224` | `FTE3600` | Mode 0, 1 MHz, CS 0 (`\_SB.PCI0.SPI1`) | Offset `0x27` on `\_SB_.GPO1`; not claimed, polarity unverified | Offset `0x00` on `\_SB_.GPO2` | Profile added; testing in progress (hardware reset disabled) |

The driver refuses an unknown DMI/GPIO profile. Other computers that expose
the same ACPI HID are not automatically supported without a verified profile.

The Medion profile additionally requires `product_version=FT` and
`board_name=YS13G`; missing or different values are rejected. Its reset GPIO
is never requested or driven while polarity remains unverified. A failed
software reset therefore ends initialization without hardware recovery.

Unit tests cover exact DMI selection, rejection of incomplete or mismatched
identities, the separate controller mappings, reset policy and A1 polarity,
ACPI path comparison, and generated udev modalias rules. These tests do not
simulate GPIO requests or replace Medion capture/enrollment/verification tests.

## Functional status

- Sensor discovery, initialization, IRQ-driven capture, cancellation, cleanup,
  and close work on the verified machine.
- A bounded DMI-gated hardware-reset recovery handles the observed non-idle
  bootloader state without uploading firmware.
- Eight-stage clean-room enrollment and versioned template persistence work.
- The opt-in personal verification policy has completed genuine and impostor
  smoke tests and an Omarchy lock-screen unlock.
- Identify, match-on-chip storage, firmware update, and sensor flash/OTP writes
  are not implemented.
- Population FAR/FRR calibration is not complete.

## Distribution matrix

| Distribution | Dependency status | Hardware status |
| --- | --- | --- |
| Arch Linux / Omarchy | Native libgpiod 2.x; package recipe included | Verified on one One-Netbook A1 |
| Fedora 43 and newer | Native libgpiod 2.x; source build supported | Not yet hardware-verified |
| Ubuntu 26.04 and newer | Native libgpiod 2.x; source build supported | Not yet hardware-verified |
| Ubuntu 22.04 / 24.04 | Official `libgpiod-dev` is 1.x | Requires a 2.x backport; unsupported as packaged |

The driver itself is not Omarchy-specific. Desktop authentication wiring is
distribution-specific and remains outside the portable hardware core.
