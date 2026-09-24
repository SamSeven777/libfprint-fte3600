# Experimental FTE3600 / FT9361 Linux support

This downstream libfprint fork implements SPI image capture, external RAM
firmware recovery, and opt-in BRISK host-side enrollment/verification for the
FT9361 sensor family. It is not a production-security or population-accuracy
certification.

| Platform | Current evidence |
| --- | --- |
| One-Netbook A1 | Maintainer-reported capture, enrollment, verification and cold-boot recovery; independent replication remains useful. |
| GPD Pocket 3 | Experimental Jasper Lake/Tiger Lake profiles with distinct controller-HID routing; real-hardware validation is incomplete. |
| Medion E3224 | Separate `medion-e3224` experiment, currently without a successful device-response/capture result. The same reported machine worked with an older Mint stack. |

See [hardware status](docs/fte3600/status.md) for exact routes and evidence limits.
Do not infer pin safety or chip identity from `ACPI\FTE3600` alone.

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
- [Contributing](CONTRIBUTING.md)

The host code is independently written under LGPL-2.1-or-later and does not run
a vendor matching library. The sensor firmware remains proprietary and is not
bundled. Protocol knowledge includes Windows transport/binary analysis and
hardware experiments; no exclusive bus-capture provenance claim is made.

The `upstream-submission` branch excludes downstream installation/packaging
tools. The separate 2D-IPA matcher branch is an experiment, not the matcher
shipped on this branch. CI configuration and local tests do not replace
hardware or biometric evaluation.
