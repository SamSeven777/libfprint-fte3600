# FTE3600 / FT9361 status

## Hardware

| Field | Verified One-Netbook A1 |
| --- | --- |
| Exact DMI vendor / product | `ONE-NETBOOK TECHNOLOGY CO., LTD. / A1` |
| ACPI / sensor | `FTE3600` / FT9361, 64 × 80 pixels |
| SPI | Mode 0, 8 bits, 1 MHz, chip select 0 |
| Reset / IRQ | `\_SB_.PCI0.GPI0` offsets `0x55` (active low) / `0x56` (rising edge) |
| Image / recovery transaction | 5,128 / 10,403 bytes, each with continuous chip select |
| Application / AGC / idle MCU | `0x30` / `0x31` / `a5 5a` |

Unknown DMI/GPIO profiles are rejected; a shared ACPI identifier does not
establish compatibility.

The experimental Medion profile requires all four exact fields:
`sys_vendor=MEDION`, `product_name=E3224`, `product_version=FT`,
`board_name=YS13G`. IRQ is offset `0x00` on `\_SB_.GPO2`;
the reset resource is offset `0x27` on `\_SB_.GPO1`.
Reset polarity remains unverified: the line is not claimed or driven,
hardware-reset and firmware recovery are disabled, and failed software reset
ends initialization. Profile unit tests do not replace end-to-end Medion
hardware validation.

## What has been verified

- On one A1: discovery, capture, cancellation/cleanup, eight-stage enrollment,
  template persistence, genuine/impostor smoke tests, and Omarchy unlock.
- The corrected standalone recovery changed `00 00` to `a5 5a`; repeated
  driver open/close checks passed. On 2026-09-12 the owner confirmed
  initialization and fingerprint authentication after a direct Linux cold
  boot. The journal does not show whether that boot invoked firmware upload.
- The A1 power helper was active both during earlier `00 00` failures and
  during the successful cold boot. It is retained as a precaution, not a
  demonstrated requirement with the corrected driver; see
  [troubleshooting](troubleshooting.md).

A1 recovery permits one size- and SHA256-pinned, owner-supplied firmware
upload per open; [installation](install.md#firmware-for-cold-boot-recovery)
documents the required file and SPI buffer. Identify, match-on-chip storage,
arbitrary firmware updates, and flash/OTP writes are not implemented.

## Authentication boundary

Minimum inlier and mutual-match gates are both **7**. These are experimental
settings, not proven optimal or independently calibrated on a real-user
population. Multi-person, multi-session FAR/FRR validation remains outstanding;
synthetic tests cannot establish authentication security. Default builds
expose capture only. The [security restrictions](../../SECURITY.md) still
apply to opt-in verification.

Hardware validation is limited to Arch Linux / Omarchy on the A1. CI builds
Fedora 43 and Ubuntu 26.04, without hardware validation on either distribution.
libgpiod 2.x is required; Ubuntu 22.04/24.04 need a backport.
