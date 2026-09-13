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

## What has been verified

- On one A1: discovery, capture, cancellation/cleanup, eight-stage enrollment,
  template persistence, genuine/impostor smoke tests, and Omarchy unlock.
- The corrected standalone firmware recovery changed `00 00` to `a5 5a`.
- On that A1, 54 corrected-driver open/close checks passed without image
  capture or firmware upload, including 10 actual suspend/resume cycles with
  both runtime-PM controls set to `auto` and a 30-second idle. The owner then
  confirmed fingerprint authentication after a direct Linux cold boot without
  the power helper. The helper has been removed; this result does not establish
  power-management behavior on other hardware.

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
