# Contributing to the FTE3600 driver

General libfprint development guidance remains in [HACKING.md](HACKING.md).
This file adds rules for the experimental FTE3600 work.

## Before opening a change

Run both policy configurations:

```sh
./scripts/check-fte3600.sh
```

All new matcher fixtures must be generated mathematical patterns. Never add a
real fingerprint image, template, descriptor dump, or raw image-bearing SPI
transaction to the repository.

Keep runtime code clean-room and redistributable. Do not copy vendor source,
decompiler output, descriptor tables, firmware, DLL/ELF files, or proprietary
templates. Public papers and specifications may inform a new implementation
when the source is cited and the code is independently written.

## Adding a hardware profile

An `ACPI\FTE3600` identifier alone is not enough. GPIO offsets and polarity
are platform-specific. A new profile should provide, in a sanitized issue:

- exact `/sys/class/dmi/id/sys_vendor` and `product_name` strings;
- sensor ID, geometry, application version, and AGC version;
- the ACPI controller path and a minimal resource excerpt, not a full DSDT;
- verified reset and interrupt GPIO offsets and polarity;
- SPI mode, speed, word size, and required single-transfer length;
- enrollment, genuine verification, impostor smoke-test, cancellation, close,
  and repeated-open results.

Do not describe a few local rejections as a measured false-accept rate.

## Scope of pull requests

Prefer separate commits for SPI infrastructure, hardware transport, clean-room
matching/template changes, packaging, and documentation. Changes to a
persisted template schema or authentication policy must increment the
corresponding version and add golden-format tests.
