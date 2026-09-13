# Security

## Authentication and hardware

The default build exposes capture only. `-Dfte3600_personal_auth=true` enables
experimental enrollment/verification, without independent real-world FAR/FRR
calibration. Neither local successes nor synthetic tests establish security
rates. Use only for local lock-screen experiments with a tested password
fallback; keep it out of login, sudo, polkit, disk encryption, passkeys, key
release, and unattended authentication.

Only the verified One-Netbook A1 enables hardware reset and, if needed, one
pinned firmware upload to sensor RAM per open attempt. Arbitrary firmware updates
and flash/OTP writes are not implemented. Unknown hardware profiles are rejected.

## Data and reporting

Image and firmware SPI payloads are redacted in transfer debug logs. The driver
wipes its main owned capture/matching/template buffers, but does not guarantee
erasure of every temporary copy. Serialized templates persist through
libfprint/fprintd; callers requesting diagnostic images must protect them.

Use private GitHub security reporting for vulnerabilities when available.
Public reports should contain only sanitized versions, hardware identity,
operation, and error text with debug logging disabled. Never post fingerprint
images/templates/descriptors, raw image-bearing SPI captures, private logs,
vendor binaries/firmware, decompiler output, or full DSDTs.

See [implementation provenance](docs/fte3600/clean-room.md) and
[GPIO permission troubleshooting](docs/fte3600/troubleshooting.md#fprintd-cannot-open-gpio).
