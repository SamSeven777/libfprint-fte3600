# FTE3600 security and privacy policy

## Experimental authentication

With `-Ddrivers=fte3600` and the default
`-Dfte3600_personal_auth=false`, the driver exposes capture but not host
authentication. Personal authentication requires explicit opt-in.

Historical offline comparisons do not establish population accuracy. The full
eight-subtemplate decision, retries, capture/enrollment failures and multi-person,
multi-session FAR/FRR have not been independently measured. This is not merely
a missing laboratory certificate. A small synthetic rejection test is not a
measured FAR. Keep a working password fallback; do not enable experimental
biometrics for system-wide sudo, root, or high-assurance authentication.

Persisted extractor/decision-policy versions must change when their semantics
change. An incompatible template must be rejected and re-enrolled, not silently
accepted under a different policy.

## Hardware and external firmware

Unknown DMI profiles are rejected; model-specific controller HID requirements
must also be satisfied before GPIO configuration. A profile's existence is not
hardware validation. A1 has maintainer-reported results; GPD profiles and the
separate Medion implementation remain experimental. See [status](docs/fte3600/status.md).

The driver has no production force-probe or GPIO-offset override intended to
bypass this selection. GPIO access still grants the process substantial
privilege. The downstream systemd/SELinux examples permit a class of GPIO
devices, not an isolation boundary around only fingerprint pins. Install a
policy only after confirming the relevant denial; do not disable SELinux.

Implemented recovery targets volatile sensor RAM and validates the expected
image length and SHA256. It does not implement persistent flash/OTP updates.
The device executes external proprietary firmware: the host-code license and
content hash neither establish its redistribution rights nor certify hardware
compatibility.

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

