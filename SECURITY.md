# FTE3600 security and privacy policy

[Documentation index](docs/fte3600/README.md)

## Experimental authentication

With `-Ddrivers=fte3600` and the default
`-Dfte3600_personal_auth=false`, the driver exposes capture but not host
authentication. Personal authentication requires explicit opt-in.

Historical offline comparisons do not establish population accuracy. The full
eight-subtemplate-or-mosaic decision, retries, capture/enrollment failures and multi-person,
multi-session FAR/FRR have not been independently measured. This is not merely
a missing laboratory certificate. A small synthetic rejection test is not a
measured FAR. Keep a working password fallback; do not enable experimental
biometrics for system-wide sudo, root, or high-assurance authentication.

Persisted extractor/decision-policy versions must change when their semantics
change. An incompatible template must be rejected and re-enrolled, not silently
accepted under a different policy.

New BRISK templates use extractor schema 3, diagnostic policy 7 and opt-in
authentication policy 8, with each identified chip's native image profile.
Verification accepts a passing individual sample or the mosaic reconstructed
from eight samples in canonical order. This decision rule has no population
FAR/FRR calibration. Compatible wire-v1 FT9361 templates retain their isolated
legacy policy 5/6 path; unsupported revisions require re-enrollment. Changing
stored version fields is not a migration. See
[template compatibility](docs/fte3600/family-authentication.md).

The separate `fte3600_ipa_auth=true` option enables experimental IPA/dual
authentication for FT9361 only and requires personal authentication to be
enabled. Other 64 × 80 sensors do not inherit IPA support from their dimensions.
Its optional status and synthetic tests do not establish biometric accuracy
or liveness. See [matcher architectures](docs/fte3600/matcher-architectures.md).

## Hardware and external firmware

The current transport pairs stock spidev with an external ACPI reset/IRQ glue
module. It validates device ancestry, character-device numbers, metadata and
session generation; it does not admit devices through a DMI model/pin whitelist.
Runtime identity selects the sensor backend. Unknown or conflicting identities
do not authorize a default FT9361 firmware upload. Historical A1/GPD results and
the separate Medion diagnostic do not validate this transport on every machine;
see [hardware status](docs/fte3600/status.md).

The glue exposes one reset GPIO and an IRQ-only UIO device, rather than the
physical GPIO controller. Systemd and SELinux integration grants access to
verified companion nodes. This narrows access, but actual enforcing-system
access, electrical behavior and suspend/resume still need hardware testing.
Stock spidev provides cooperative locking, not enforced cross-client exclusion
or atomic PM revocation. Abnormal process exit cannot guarantee immediate
native-CS restoration; the next controlled open restores the baseline.
Follow [installation and migration](docs/fte3600/acpi-spidev.md); do not disable
SELinux or Secure Boot to bypass an installation failure.

Legacy recovery writes volatile sensor RAM and checks the selected image's
exact length and SHA-256. FT9368 additionally has an explicit, opt-in persistent
application-flash update path requiring a positively identified running
application and both validated images. No OTP programming is implemented.
Installing firmware does not itself request a flash update. See
[firmware installation and update limits](docs/fte3600/install.md#2-install-the-firmware-for-the-identified-chip).
The sensor executes external proprietary firmware: the host-code license and
content hash establish neither redistribution rights nor hardware compatibility.

## Biometric data

Sensitive SPI payloads are excluded from normal byte-dump logging. Major owned
capture and template buffers are explicitly cleared on release. This does not
guarantee erasure of temporary feature copies, worker stacks, GLib/GBytes
serialization, allocator copies, dumps, swap, or framework-managed data.

fprintd handles persistent templates; verify the actual permissions and retention
policy on the target distribution. Do not assume that driver-side cleanup
removes enrolled templates. Delete enrollment only deliberately, with a working
password fallback.

Report vulnerabilities privately through the repository's Security reporting
channel when available. Public reports should contain only reviewed, sanitized
hardware identifiers and errors, never fingerprint images, templates,
descriptor dumps, process dumps, proprietary binaries or decompiler listings.
