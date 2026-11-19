# FTE3600 support for libfprint

Linux SPI driver for FocalTech FTE3600 sensors, with BRISK enrollment,
persistent prints, verification and identification through libfprint/fprintd.

[Installation](docs/fte3600/install.md) · [Arch / NixOS](packaging/README.md) · [Hardware tools](docs/fte3600/tools.md) · [Contributing](CONTRIBUTING.md) ·
[Security](SECURITY.md)

Implemented profiles: FT9338, FT9348, FT9361, FT9536, FT9365, FT9368, FW9369
and FT9769. Tested devices and revisions are listed below.

## Hardware reports

| Device | Sensor | Reported results |
| --- | --- | --- |
| One-Netbook A1 | FT9361 | Hardware-confirmed on this project's original test device. |
| GPD Pocket 3 (G1621-02) | FW9369, raw ID `0x9362` | Enrollment, repeated verification, S3 suspend/resume, and lock-screen unlock passed on `6efa376` ([report](https://github.com/SamSeven777/libfprint-fte3600/issues/2#issuecomment-6212129012)). The additional calibration-recovery fix in [#13](https://github.com/SamSeven777/libfprint-fte3600/pull/13) still needs hardware validation. |
| OneMix 4 | FT9769-class, chip `0x9391`, ACPI `FTE6600` | Re-enrollment and verification confirmed by the contributor in merged [#11](https://github.com/SamSeven777/libfprint-fte3600/pull/11). |
| ONEXPLAYER X2 Mini | FT9365, chip `0x9365` | Enrollment and unlock reported working with the contributor's downstream finger-lift fix. [#15](https://github.com/SamSeven777/libfprint-fte3600/pull/15) ports the fix to this driver; its follow-up release-timeout changes still need hardware validation. |

Medion Akoya E3224 testing remains in progress ([#1](https://github.com/SamSeven777/libfprint-fte3600/issues/1)).
Sixunited DNTU has a hardware compatibility report, without driver testing yet
([#14](https://github.com/SamSeven777/libfprint-fte3600/issues/14)).

Use the [hardware tool](docs/fte3600/tools.md) to identify a sensor and collect
a report. This driver supports SPI sensors.

## Requirements and licensing

The driver requires the ACPI reset/IRQ module in `kernel/fte3600`, distribution
spidev and the [firmware required by the chip](docs/fte3600/install.md#firmware).

Host code is LGPL-2.1-or-later; kernel glue is GPL-2.0-only. Existing copyright
and dependency notices apply. The independent driver uses the attributed
SamSeven777 BRISK implementation; [source origins](docs/fte3600/independent-source.json)
and the dependency's `COPYING` / `UPSTREAM.json` identify the inputs.

## Contributors

- [@FlightDan](https://github.com/FlightDan)
- [@solarkennedy](https://github.com/solarkennedy)
- [@ttamakijp](https://github.com/ttamakijp)
- [@iwansugiarto](https://github.com/iwansugiarto)
- [@Tuxman2](https://github.com/Tuxman2)
- [@Kriketo](https://github.com/Kriketo)
- [@Evaner5580](https://github.com/Evaner5580)
- [@shokerplz](https://github.com/shokerplz)
