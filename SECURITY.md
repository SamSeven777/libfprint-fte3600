# Security

FTE3600 authentication uses BRISK and is enabled with `-Ddrivers=fte3600`.
It is experimental: population accuracy and liveness are not established.
Keep password access and re-enroll after incompatible template changes.

Firmware remains external and is checked for exact size and SHA-256. Its hash
does not establish redistribution rights or compatibility with an unknown chip.
Follow the [installation guide](docs/fte3600/install.md) for device permissions,
module signing and rollback; retain the system's normal security controls.

fprintd manages stored templates. Keep biometric images, templates, descriptor
and SPI dumps, proprietary files and private logs out of public reports.
Driver cleanup does not guarantee erasure of all framework or system copies.

Report vulnerabilities privately through the repository's
[security reporting channel](https://github.com/SamSeven777/libfprint-fte3600/security/advisories/new).
