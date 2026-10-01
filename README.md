# Medion E3224 experimental FTE3600 support

Medion development is restarting from tuxman2's reported working Mint 22.2
stack: vobademi's package and ctfdavis's SPI module. The previous Medion code
never worked on his machine and is not the basis for the new protocol.
The exact module chip remains unconfirmed. This is not a working driver release.

The ordinary driver now rejects Medion before opening SPI or claiming GPIO;
it cannot automatically enter the A1 FT9361 protocol. The new bounded ctfdavis
comparison makes no GPIO request, uses Mode 0 at no more than 1 MHz, and tests
the archived userspace C6/identity prefix with sequential SPI transfers.
This is not full initialization or capture. The later 4 MHz/GPIO attachment
was a proposed patch, not the code confirmed by the Mint success report.

Do not request another run of the unchanged FT9361 recovery sequence that
already failed. The diagnostic wrappers share an exclusive lock through device
and service cleanup. Direct diagnostic invocations bypass that lock.

- [Independent Medion rebuild and source provenance](docs/fte3600/medion-rebuild.md)
- [Bounded ctfdavis candidate test](docs/medion-legacy-id-test.md)
- [Evidence and hardware routes](docs/fte3600/status.md)
- [Diagnostic boundaries](docs/fte3600/troubleshooting.md)
- [Build, installation and rollback](docs/fte3600/install.md)
- [Security and biometric privacy](SECURITY.md)
- [Implementation provenance](docs/fte3600/clean-room.md)

A1 results do not establish Medion support. Default authentication is disabled;
the downstream Arch package explicitly opts into experimental personal
authentication. Keep a working password fallback and do not enable system-wide
sudo/root biometrics. The host code is LGPL-2.1-or-later; external vendor firmware
is not bundled or made open source by its extraction script.
