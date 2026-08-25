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
| Fedora 42 and newer | Native libgpiod 2.x; source build supported | Not yet hardware-verified |
| Ubuntu 26.04 and newer | Native libgpiod 2.x; source build supported | Not yet hardware-verified |
| Ubuntu 22.04 / 24.04 | Official `libgpiod-dev` is 1.x | Requires a 2.x backport; unsupported as packaged |

The driver itself is not Omarchy-specific. Desktop authentication wiring is
distribution-specific and remains outside the portable hardware core.
