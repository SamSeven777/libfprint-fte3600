# Contributing

Driver code lives in `libfprint/drivers/fte3600/`; general libfprint guidance
is in [HACKING.md](HACKING.md). Run the production and synthetic API checks:

```sh
./scripts/check-fte3600.sh
```

Install umockdev and its development package for enumeration tests. Synthetic
builds are for tests only. Keep regression tests with the code; keep test logs
and audit reports outside the release tree.

Preserve source attribution and use independently written, redistributable
code. Do not submit vendor code, firmware, real fingerprints or templates.
Hardware support must follow ACPI resources and verified sensor identity.
Version incompatible template or authentication changes and include regression
coverage. Keep user instructions in [the installation guide](docs/fte3600/install.md).
