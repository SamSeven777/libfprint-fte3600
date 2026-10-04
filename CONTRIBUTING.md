# Contributing to the FTE3600 driver

General libfprint development guidance remains in [HACKING.md](HACKING.md).
This file adds rules for the experimental FTE3600 work.

The [architecture](docs/fte3600/architecture.md) defines the module boundaries.
Keep hardware commands in the wire module, protocol timing in its own header,
and device lifecycle in the driver. The reusable BRISK core must build with
only GLib and libm; it accepts image views and returns numerical evidence.
Sensor geometry profiles, persistent template formats and authentication
decisions belong in adapters. Do not import driver/build-policy headers into
the generic matcher to enable another sensor.

## Before opening a change

Run both policy configurations:

```sh
./scripts/check-fte3600.sh
```

All new matcher fixtures must be generated mathematical patterns. Never add a
real fingerprint image, template, descriptor dump, or raw image-bearing SPI
transaction to the repository.

Keep runtime code independently written and redistributable. Do not copy vendor source,
decompiler output, descriptor tables, firmware, DLL/ELF files, or proprietary
templates. Public papers and specifications may inform a new implementation
when the source is cited and the code is independently written.

## Extending hardware support

Do not add DMI model names, GPIO offsets, controller names or force-probe
overrides to runtime code. Board wiring comes from the device's ACPI resources.
Provide a sanitized resource excerpt when diagnosing a missing or ambiguous
resource, plus observed sensor identity, geometry, firmware/AGC versions and
SPI transfer limits. Describe reset polarity evidence independently of GpioIo:
that ACPI resource has no polarity field.

A new sensor protocol needs an independently documented identity test,
initialization/capture behavior and failure cleanup. Share implementations only
when the wire protocol and capability checks justify it. Do not enable an
unknown sensor by falling back to FT9361 firmware.

Include cancellation, cold start, close/reopen, suspend/resume and hardware
results where available. Never post biometric images/templates, a full DSDT,
vendor binaries or decompiler listings.

## Scope of pull requests

Prefer separate commits for SPI infrastructure, hardware transport, independent
matching/template changes, packaging, and documentation. Changes to a
persisted template schema or authentication policy must increment the
corresponding version and add golden-format tests.

## Evidence and audit claims

Record the source commit, build options and actual test logs. Distinguish mock
tests, hardware observations, offline pair comparisons and full authentication
attempts. Do not describe CI configuration as executed sanitizer evidence or
a configured GPIO profile as verified hardware support.

For authentication-policy changes, provide the data provenance, evaluation
protocol, independent test split, counts, failure handling and eight-subtemplate
decision rule. Synthetic tests are useful regressions, not population FAR/FRR.
Do not add unsupported provenance, memory-erasure or production-safety claims.
