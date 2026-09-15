# FTE3600 / FT9361 Linux Driver

[![FTE3600 CI](https://github.com/SamSeven777/libfprint-fte3600/actions/workflows/fte3600-ci.yml/badge.svg)](https://github.com/SamSeven777/libfprint-fte3600/actions/workflows/fte3600-ci.yml)

Open-source `libfprint` driver for the FocalTech FT9361 SPI capacitive fingerprint sensor:
supports discovery, image capture, eight-stage enrollment, native cold-boot recovery,
and opt-in host-side verification.

> [!NOTE]
> Default builds expose image capture only. Host-side authentication requires
> `-Dfte3600_personal_auth=true`. See [SECURITY.md](SECURITY.md) for policy details.

## Supported hardware

The driver targets the **FocalTech FT9361** SPI sensor (`ACPI\FTE3600`, 64 × 80 pixels).
Platform status:
- **One-Netbook A1** (`ONE-NETBOOK TECHNOLOGY CO., LTD. / A1`): Verified on real hardware (discovery, capture, enrollment, verification, and native cold-boot recovery).
- **Medion Akoya E3224** (`MEDION / E3224`): Experimental profile in development (routing identified; real-hardware verification in progress on `medion-e3224` branch).

Because GPIO routing and pin polarities vary by motherboard, unknown hardware profiles
fail closed during device probe to prevent invalid GPIO assertions. See [hardware status](docs/fte3600/status.md)
for platform details or to contribute a new profile.

## Quick start

```sh
git clone --branch main https://github.com/SamSeven777/libfprint-fte3600.git
cd libfprint-fte3600

# Download and install runtime firmware for cold-boot recovery
./scripts/install-firmware.sh
```

- [Installation guide](docs/fte3600/install.md): dependencies, firmware setup, build/test, packaging, and safe enrollment.
- [Hardware status](docs/fte3600/status.md): verified specifications and calibration metrics.
- [Troubleshooting](docs/fte3600/troubleshooting.md): diagnostics, SPI buffer configuration, and Fedora SELinux setup.
- [Clean-room implementation](docs/fte3600/clean-room.md): architecture, algorithm references, and provenance.
- [Contributing](CONTRIBUTING.md) · [Release history](CHANGELOG.md).

## License and provenance

Based on upstream [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint)
(commit [`c4654fdc85c25afdd9115bec2f95a44145ae3b94`](https://gitlab.freedesktop.org/libfprint/libfprint/-/commit/c4654fdc85c25afdd9115bec2f95a44145ae3b94),
version `1.94.100`). New FTE3600 code is licensed under `LGPL-2.1-or-later`; see [COPYING](COPYING).

The driver and BRISK matcher are clean-room implementations developed without vendor source code
or proprietary libraries. No proprietary firmware binary is distributed in this repository.

Thanks to libfprint/fprintd contributors and [Omarchy](https://omarchy.org/) for the
integration environment. [OpenAI Codex](https://openai.com/codex/) and
[Google Antigravity](https://deepmind.google/) substantially assisted implementation,
testing, review, and documentation. These acknowledgements imply no endorsement or official support.
