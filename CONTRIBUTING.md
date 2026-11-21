# Contributing

Use `main` for the BRISK driver and hardware integration. Keep IPA development,
dual-matcher fusion and algorithm experiments on `research/algorithms`.
That branch preserves the IPA core and `tests/test-ipa-core.c`; it does not
currently connect IPA to enrollment, persisted prints or authentication.
Algorithm changes must include test results and an explicit template/policy
compatibility decision before integration into the driver.

Driver code lives in `libfprint/drivers/fte3600/`; general libfprint guidance
is in [HACKING.md](HACKING.md). Run the production and synthetic API checks:

```sh
./scripts/check-fte3600.sh
```

Install umockdev and its development package for enumeration tests. Use the synthetic backend for software tests. Keep regression tests with the
code and attach test results to the pull request.

Preserve source attribution and use independently written, redistributable
code. Use synthetic images and templates in tests.
Hardware support must follow ACPI resources and verified sensor identity.
Version incompatible template or authentication changes and include regression
coverage. Keep user instructions in [the installation guide](docs/fte3600/install.md).

For hardware reports, include the tested commit, output from the
[hardware tool](docs/fte3600/tools.md), and enrollment/verification results.
