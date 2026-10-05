# FTE3600 documentation

[Repository overview](../../README.md) · [Contributing](../../CONTRIBUTING.md)

All maintained documentation is in English. The guides below describe the
current `main` implementation unless explicitly marked as historical. Existing
document paths are retained so links in hardware reports remain usable.

## Use and test the driver

Start with installation, then complete the transport integration. The support
status distinguishes implemented code from results reported on physical devices.

| Document | Purpose |
| --- | --- |
| [Installation](install.md) | Dependencies, firmware, library build, enrollment and rollback |
| [ACPI glue and spidev](acpi-spidev.md) | Kernel module, old-bridge migration, udev, systemd, SELinux and removal |
| [Support and hardware status](status.md) | Eight chip profiles, recovery limits, A1/GPD/Medion evidence and untested behavior |
| [Troubleshooting](troubleshooting.md) | Resource discovery, permissions, sensor responses and capture failures |
| [Security and privacy](../../SECURITY.md) | Authentication limits, biometric data handling and reporting |

The [Medion E3224 standalone diagnostic](https://github.com/SamSeven777/libfprint-fte3600/blob/medion-spidev/docs/fte3600/medion-spidev.md)
has its own instructions on `medion-spidev`. Do not combine its physical GPIO
procedure with the normal `main` installation.

## Develop and review

| Document | Purpose |
| --- | --- |
| [Architecture](architecture.md) | Module boundaries, lifecycle, ownership and failure contracts |
| [Image resolution](image-resolution.md) | Per-chip physical-scale metadata, manufacturer sources and unknown values |
| [Dynamic discovery](dynamic-discovery.md) | ACPI resource selection, sensor identity and firmware authorization |
| [Kernel interface](../../kernel/fte3600/README.md) | Reset GPIO, IRQ-only UIO, metadata ABI and lease behavior |
| [Matcher architecture](matcher-architectures.md) | Algorithm cores, adapters and standalone checks |
| [BRISK reuse](../../libfprint/matchers/brisk/README.md) | Image dimensions, stride, match evidence and ownership |
| [2D-IPA reuse](../../libfprint/matchers/ipa/README.md) | Alternative core API and its fixed image geometry |
| [Family authentication](family-authentication.md) | Sensor profiles, enrollment, decision policy and stored-template compatibility |
| [Contributing](../../CONTRIBUTING.md) | Code, tests, documentation and evidence requirements |
| [Tests](../../tests/README.md#fte3600-lifecycle-tests) | Synthetic FTE3600 lifecycle fixtures and test commands |
| [Upstream preparation](upstream-preparation.md) | Separate kernel/libfprint submissions and unresolved interface questions |
| [Implementation provenance](clean-room.md) | Source boundaries, external firmware and independently written host code |

## Sensor protocols

These references retain wire values, timing and source offsets. They describe
observed Windows behavior separately from Linux implementation choices.

| Document | Scope |
| --- | --- |
| [GPIO polarity](gpio-polarity.md) | Physical Windows GPIO writes and Linux logical reset values |
| [Legacy protocol evidence](sensor-protocol-evidence.md) | FT9348/FT9361 A8 framing and FT9338/FT9536 differences |
| [Legacy RAM recovery](legacy38-recovery.md) | FT9338/FT9536 boot identity, upload and complete readback |
| [Special-family probing](special-probe.md) | State-changing identification and bounded cleanup |
| [FW9369 / ID 9362](fw9369-protocol.md) | Commands, interrupts, baseline and image calibration |
| [FT9365 / FT9769](ft93xx-protocol.md) | Host configuration, sample framing and image processing |
| [FT9368](ft9368-protocol.md) | Application protocol and explicit persistent update |

## Windows research

These are source-specific observations, not a list of hardware verified on Linux.

| Document | Scope |
| --- | --- |
| [Hardware inventory](windows-hardware-inventory.md) | Chip identities, protocol families and firmware metadata ([JSON](windows-hardware-inventory.json)) |
| [INF baseline](windows-inf-baseline.md) | Package identity, hardware IDs and install-section mappings |
| [Registry baseline](windows-registry-baseline.md) | Common settings and transport differences ([JSON](windows-registry-baseline.json)) |
| [Runtime adaptation](windows-runtime-adaptation.md) | Resource parsing, runtime selection, ROM/OTP identity and vendor fallbacks |
| [Lifecycle coverage](windows-lifecycle-coverage.md) | Implemented behavior, deliberate differences and remaining evidence gaps |

## Validation and historical records

Start with the [October 5 release validation](validation-release-2026-10-05.md)
for the published ACPI/spidev implementation. Each record applies to its stated
revision, configuration and environment; software tests do not establish device
compatibility or biometric accuracy. Use the current guides above for commands.

| Date | Record | Context |
| --- | --- | --- |
| 2026-10-05 | [Release validation](validation-release-2026-10-05.md) | Integrated transport, local checks and published CI results |
| 2026-10-05 | [Independent transport audit](audit-acpi-spidev-2026-10-05.md) | Findings and subsequent fixes |
| 2026-10-04 | [Transport validation](validation-acpi-spidev-2026-10-04.md) | Earlier ACPI/spidev development snapshot |
| 2026-10-04 | [Windows comparison](windows-recheck-2026-10-04.md) | Earlier custom-bridge behavior review |
| 2026-10-04 | [Linux/Windows audit](linux-windows-audit-2026-10-04.md) | Legacy wake and custom-bridge fixes |
| 2026-10-04 | [Local validation](validation-2026-10-04.md) | Pre-migration driver and matcher checks |

The [changelog](../../CHANGELOG.md) records project changes. Dated audit records
preserve earlier findings; a resolved finding is not an outstanding defect in
the current implementation unless the current status explicitly says so.
