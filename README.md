# FTE3600 support for libfprint

This downstream libfprint fork provides experimental Linux SPI support for
FocalTech FTE3600 devices. The current driver uses the independently written
0.2.1 device implementation and the attributed SamSeven777 BRISK matcher.
Enrollment, saved prints, verification and identification use libfprint's
normal API. Experimental personal-use BRISK authentication is enabled when
the `fte3600` driver is built; there is no separate authentication option.

**Start with the [current implementation and installation guide](docs/fte3600/current-implementation.md).**
It covers firmware, the ACPI reset/IRQ glue, service permissions, native Meson
builds, migration and rollback.

```sh
meson setup build -Ddrivers=fte3600 -Ddoc=false -Dintrospection=false \
  -Dinstalled-tests=false
meson compile -C build
```

The driver uses distribution spidev plus this repository's ACPI reset/IRQ
glue. It discovers the device through ACPI resources rather than a laptop
model whitelist. Firmware remains external and is not bundled.

On 2026-10-12, the 0.2.1 production driver completed enrollment and three
positive verification attempts on a One-Netbook A1 with an FT9361 sensor,
running NixOS x86_64. That observation used the stable libfprint integration;
this publication integrates the same driver logic with official libfprint
master. The other implemented sensor families, negative controls, population
accuracy and physical suspend behavior remain unvalidated. Windows parity is
not established, and USB devices are outside this implementation.

When migrating from the previous driver, preserve the existing prints and
your rollback configuration, then enroll new prints. Compatibility with the
previous SamSeven777 template format has not been established. Keep password
access while evaluating the experimental authentication policy.

- [Current guide](docs/fte3600/current-implementation.md): build, use, limits and provenance.
- [Contributing](CONTRIBUTING.md): software checks and contribution scope.
- [Historical documentation index](docs/fte3600/README.md): retained earlier implementation and validation references.
- [Security reporting](SECURITY.md).

The host implementation is LGPL-2.1-or-later. No vendor matching library is
executed; proprietary firmware, biometric captures and templates are excluded
from this repository.
