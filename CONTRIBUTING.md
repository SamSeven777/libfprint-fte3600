# Contributing to the FTE3600 driver

General libfprint development guidance remains in [HACKING.md](HACKING.md).
This file adds rules for the experimental FTE3600 work.
The [current implementation guide](docs/fte3600/current-implementation.md) is the
entry point for installation, architecture, validation limits and provenance.
The [earlier documentation index](docs/fte3600/README.md) is historical reference.

The maintained driver lives in `libfprint/drivers/fte3600/`. Keep generic ACPI
enumeration in the framework and device protocol/authentication policy in the
driver. The attributed BRISK dependency remains separate from sensor geometry,
persistent template formats and authentication decisions. Changes to that
approved dependency need their own explicit scope and attribution.

## Before opening a change

Build production and the separate synthetic backend, then run their software checks:

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

### Documentation

Write maintained documentation in English. Add new documents to the
[index](docs/fte3600/README.md) and keep existing paths and section links usable.
Prefer linking to the document responsible for a topic over copying its
instructions into several files:

- `current-implementation.md`: current build, firmware, migration, architecture
  and the scope of hardware/software evidence.
- Existing guides and dated records: historical findings for their stated
  revision; preserve their paths and label them accordingly.

Write current guides in the present tense. Identify the source version and
scope of Windows observations. Mark historical records explicitly and link
them to current guidance; do not leave obsolete installation commands in an
active guide. Preserve register values, timing, source offsets and hashes when
translating or restructuring research. Do not turn an old hardware observation
or a software test into a claim about the current release.

Check relative links and section anchors after editing. Update the relevant
guide when behavior changes instead of adding another overlapping status page.

### Validation evidence

Record the source commit, build options and actual test logs. Distinguish mock
tests, hardware observations, offline pair comparisons and full authentication
attempts. Do not describe CI configuration as executed sanitizer evidence or
a configured GPIO profile as verified hardware support.

For authentication-policy changes, provide the data provenance, evaluation
protocol, independent test split, counts, failure handling and eight-subtemplate
decision rule. Synthetic tests are useful regressions, not population FAR/FRR.
Do not add unsupported provenance, memory-erasure or production-safety claims.
