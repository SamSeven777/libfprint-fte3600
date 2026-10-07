# FTE3600 support for libfprint

**Medion experiment:** this branch adds a [standalone spidev diagnostic](docs/fte3600/medion-spidev.md)
for Tuxman2's E3224 wiring. It tests identification, initialization and capture
with the distribution's SPI/GPIO interfaces, without installing a custom kernel
module or system libfprint. Use that guide for this experiment; the installation
instructions below describe the regular ACPI glue-based driver.

This downstream libfprint fork implements experimental Linux support for eight
FocalTech sensor profiles: FT9338, FT9348, FT9361, FT9536, FT9365, FT9368,
FW9369 (silicon ID 9362), and FT9769 (IDs 9391/9392).

The current `main` uses the distribution's **stock spidev** for SPI transfers
and a small **ACPI reset/IRQ glue module** for board resources. Sensor identity
is detected at runtime; there is no laptop-model whitelist. The glue is not
included in the upstream kernel and still requires a trusted module signature
where Secure Boot requires one.

Capture backends exist for all eight profiles. On 2026-10-05, the maintainer
reported current-main hardware testing on One-Netbook A1, and a GPD Pocket 3
report demonstrated capture and close/reopen on unmodified main `7ea5cfc`.
These reports do not establish all-device compatibility, image quality or a
complete authentication/power test matrix. Read the
[support and hardware status](docs/fte3600/status.md) for their scope before testing.

<a id="build-and-use"></a>

## Getting started

1. Follow the [installation guide](docs/fte3600/install.md) for dependencies,
   chip-specific firmware, build options and rollback.
2. Follow [ACPI glue and spidev integration](docs/fte3600/architecture.md) to
   migrate an older bridge installation and configure the current transport.
3. Use the [troubleshooting guide](docs/fte3600/troubleshooting.md) to separate
   resource, permission, identification and capture failures.

Select `-Ddrivers=fte3600` or `-Ddrivers=all,fte3600` when building. Authentication
is disabled by default; `-Dfte3600_personal_auth=true` enables experimental
BRISK enrollment and verification. Keep a working password fallback. See
[authentication and template compatibility](docs/fte3600/family-authentication.md)
and [security](SECURITY.md) for the validation limits.

The board-specific [Medion E3224 diagnostic](https://github.com/SamSeven777/libfprint-fte3600/blob/medion-spidev/docs/fte3600/medion-spidev.md)
lives on the separate `medion-spidev` branch. Its standalone testing procedure
is distinct from installing this driver.

## Documentation and development

The **[documentation index](docs/fte3600/README.md)** groups user guides,
architecture, protocols, Windows research and dated validation records.

- [Architecture](docs/fte3600/architecture.md): driver, transport, protocol,
  algorithm and build boundaries.
- [Matcher cores](docs/fte3600/matcher-architectures.md): reusable BRISK and
  2D-IPA interfaces, separate from sensor protocols.
- [Contributing](CONTRIBUTING.md): development checks and documentation policy.
- [Upstream preparation](docs/fte3600/upstream-preparation.md): proposed kernel
  and libfprint review boundaries and outstanding evidence.

The host implementation is independently written under LGPL-2.1-or-later and
does not execute a vendor matching library. Proprietary sensor firmware is
external and is not bundled. [Implementation provenance](docs/fte3600/clean-room.md)
records the use of Windows binary/transport analysis and hardware experiments.
