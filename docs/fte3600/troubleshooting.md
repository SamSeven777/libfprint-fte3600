# Troubleshooting

## Discovery

Check `ls -l /dev/fte3600-*`, `/sys/class/misc/fte3600-*/fte3600_abi`, the bound
SPI driver and relevant kernel/fprintd errors. DMI is not consulted. The
`fte3600` kernel module must be installed for the running kernel. Remove only
old FTE3600-specific spidev override rules as described in
[dynamic discovery](dynamic-discovery.md); do not unbind another device.

The bridge rejects ambiguous ACPI descriptions, missing GPIO controllers and
unsupported interrupt resource formats. This is resource validation, not a
missing machine entry. Never guess GPIO numbers to work around it.

## No expected MCU response

All-zero/all-one responses do not prove the sensor model or missing firmware.
Cold recovery requires an accepted ROM-family/OTP response, then the exact
10,396-byte external FT9361 image. Unknown responses stop before firmware load.
The controller must allow an unsplit 10,403-byte transfer for recovery and
5,128 bytes for capture; changing spidev.bufsiz does not affect the bridge.

## Permissions and suspend

Nodes are root-only. If the systemd device sandbox blocks access, generate
exact-node DeviceAllow entries using `scripts/fte3600-device-allow.sh` and review
them before installing a service drop-in. GPIO-class permissions and the old
GPIO SELinux policy are no longer needed by this transport. Diagnose actual
AVC denials for the bridge node rather than granting broad GPIO/misc access.

Suspend invalidates an open session; cancel/close it and reopen after resume.
Automatic suspend/resume recovery has not been validated on hardware.

## Enrollment

Keep a password fallback and use direct `fprintd-verify` experiments first.
Lift fully between enrollment stages and vary placement slightly. Low-quality
or duplicate samples may need retry. A successful personal test is not a
population authentication evaluation.

When the driver reports an incompatible old template, deliberate deletion with
`fprintd-delete "$USER"` removes that user's enrolled prints; re-enroll afterward.
Do not delete all of `/var/lib/fprint`. Do not use experimental biometrics for
system-wide sudo/root access.

Public reports should include only reviewed identifiers and relevant errors,
not full private logs, fingerprint images, templates, descriptors or dumps.

