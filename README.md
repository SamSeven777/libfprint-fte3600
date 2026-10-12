# FTE3600 support for libfprint

Linux SPI driver for FocalTech FTE3600 sensors, with BRISK enrollment,
persistent prints, verification and identification through libfprint/fprintd.

[Installation](docs/fte3600/install.md) · [Contributing](CONTRIBUTING.md) ·
[Security](SECURITY.md)

Implemented profiles: FT9338, FT9348, FT9361, FT9536, FT9365, FT9368, FW9369
and FT9769. Hardware confirmation is currently limited to FT9361 on One-Netbook
A1; other models, suspend/resume and complete Windows parity remain unverified.
USB sensors are outside this driver. Authentication is experimental and has no
population accuracy calibration; retain password access.

The driver requires the ACPI reset/IRQ module in `kernel/fte3600`, distribution
spidev and separately supplied firmware where needed. Firmware is not bundled.

Host code is LGPL-2.1-or-later; kernel glue is GPL-2.0-only. Existing copyright
and dependency notices apply. The independent driver uses the attributed
SamSeven777 BRISK implementation; [source origins](docs/fte3600/independent-source.json)
and the dependency's `COPYING` / `UPSTREAM.json` identify the inputs.
