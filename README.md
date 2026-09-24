# Medion E3224 experimental FTE3600 support

This branch contains the Medion-specific GPIO routing, power diagnostics and
recovery work. It is not a successful Medion driver release. The latest reported
tests of this implementation did not obtain a valid device response or capture;
the exact module chip remains unconfirmed.

The same user's machine worked with an older Mint software stack. Preserve
that known-good result and compare startup/transport/firmware behavior against
it. Do not request another run of an unchanged recovery sequence that already
failed. A new hardware experiment needs a narrow question and an explicit
description of the state it changes.

Reset is currently modeled on `\_SB_.GPO1` pin 39 and IRQ on
`\_SB_.GPO2` pin 0. Active-low reset remains a hypothesis requiring validation.
The diagnostic recovery command resets hardware, uploads firmware and temporarily
changes runtime-power policy; it is not a read-only probe.

- [Evidence and hardware routes](docs/fte3600/status.md)
- [Diagnostic boundaries and known-good comparison](docs/fte3600/troubleshooting.md)
- [Build, installation and rollback](docs/fte3600/install.md)
- [Security and biometric privacy](SECURITY.md)
- [Implementation provenance](docs/fte3600/clean-room.md)

A1 results do not establish Medion support. Default authentication is disabled;
the downstream Arch package explicitly opts into experimental personal
authentication. Keep a working password fallback and do not enable system-wide
sudo/root biometrics. The host code is LGPL-2.1-or-later; external vendor firmware
is not bundled or made open source by its extraction script.
