# Experimental FTE3600 sensor-family Linux support

**The current transport uses stock spidev, an ACPI reset GPIO and an IRQ-only
UIO companion.** Follow the [installation and migration guide](docs/fte3600/acpi-spidev.md)
when upgrading from the earlier custom SPI bridge. This is still experimental
downstream support; the glue is not included in the upstream kernel.

This downstream libfprint fork implements runtime-selected SPI capture for
FT9338, FT9348, FT9361, FT9536, FT9365, FT9368, FW9369 (silicon ID 9362) and
FT9769 (9391/9392). It implements external RAM recovery for FT9348/FT9361,
explicit FT9368 flash updates, and opt-in BRISK enrollment/verification for all
eight identified chips. Templates are bound to the chip and image-processing
profile; sharing the matcher does not make different sensors' templates interchangeable.
FT9536 supports positively identified boot-A RAM recovery; FT9338-family boot-B
recovery additionally requires a matching runtime identity observed during the
current open. An unidentified blank FT9338 is not guessed from an absent reply.
New paths have software test coverage, not hardware
qualification or population-accuracy certification.

The transport uses a small Linux ACPI reset/IRQ glue module instead of a DMI
model/pin whitelist. SPI data goes through stock spidev. Reset and IRQ may reside on different controllers.
Chip identity comes from repeated runtime/word-register responses or a
ROM-family/OTP probe. Discovery can negotiate SPI chip-select polarity when
ACPI describes it incorrectly, without a machine table. Eight catalogued chips
now have capture backends across six protocol families. Each backend has its
own initialization, raw framing and cleanup; this is not a claim that all
eight have passed physical-device tests. See the [support matrix](docs/fte3600/architecture.md).

The [architecture](docs/fte3600/architecture.md) separates device lifecycle,
wire protocol, reusable image matching and build policy. The
[BRISK core](libfprint/matchers/brisk/README.md) accepts image dimensions and row
stride, depends only on GLib and libm, and returns numerical match evidence.
FTE3600 template compatibility and authentication decisions live in adapters.
New enrollments use a versioned sensor-aware template; existing compatible
wire-v1 FT9361 templates remain readable. Earlier wire-v2 templates with
diagnostic policy 6 / authentication policy 7 require re-enrollment after the
rotation and mosaic-quality corrections. See [family authentication](docs/fte3600/family-authentication.md).

See [dynamic discovery](docs/fte3600/dynamic-discovery.md) for the new module,
installation/migration, protocol limits and test evidence. Historical A1/GPD/
Medion observations in [hardware status](docs/fte3600/status.md) do not validate
this new transport on those machines.

## Build and use

Follow the [installation and rollback guide](docs/fte3600/install.md).
The driver is optional: select `-Ddrivers=fte3600` or
`-Ddrivers=all,fte3600`. Authentication remains disabled unless
`-Dfte3600_personal_auth=true` is selected. The downstream Arch package
explicitly opts into this experimental personal policy.

Keep a working password fallback. Multi-person, multi-session FAR/FRR for the
actual eight-template authentication flow remains unmeasured. Do not enable
experimental biometric authentication for system-wide sudo or root access.

- [Security and privacy](SECURITY.md)
- [Troubleshooting](docs/fte3600/troubleshooting.md)
- [Implementation and provenance](docs/fte3600/clean-room.md)
- [Windows hardware and protocol inventory](docs/fte3600/windows-hardware-inventory.md)
- [Contributing](CONTRIBUTING.md)

The host code is independently written under LGPL-2.1-or-later and does not run
a vendor matching library. The sensor firmware remains proprietary and is not
bundled. Protocol knowledge includes Windows transport/binary analysis and
hardware experiments; no exclusive bus-capture provenance claim is made.

See [upstream preparation](docs/fte3600/upstream-preparation.md) for the kernel,
libfprint and matcher review boundaries and remaining hardware evidence.
The optional 2D-IPA policy is a separate build choice from BRISK; neither
policy is qualified by synthetic tests alone. CI configuration and local tests
do not replace hardware or biometric evaluation.
