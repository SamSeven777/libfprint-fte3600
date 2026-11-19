# Security

FTE3600 authentication uses BRISK and is enabled with `-Ddrivers=fte3600`.
Population accuracy and liveness have not been evaluated. Keep password access
available alongside fingerprint authentication.

Firmware is supplied separately and checked for exact size and SHA-256.
The [installation guide](docs/fte3600/install.md) covers device permissions and
module signing. fprintd manages stored templates.

Use text diagnostics for public hardware reports. Keep fingerprint images,
templates, descriptor dumps and private files out of public attachments.

Report vulnerabilities through the repository's
[private security channel](https://github.com/SamSeven777/libfprint-fte3600/security/advisories/new).
