# Troubleshooting

## Discovery

Check `ls -l /dev/spidev* /dev/gpiochip*`, the DMI vendor/product strings and
reviewed `journalctl -u fprintd.service -b` output. Compare exact values with
[hardware status](status.md). Unknown DMI or a required unsupported/missing GPIO
controller HID should be rejected. Do not bypass this check or guess pin numbers.

If no SPI node exists, check the installed udev rule and kernel spidev support.
Use a narrowly scoped SPI udev trigger only after verifying the matched device;
do not unbind an unrelated existing driver.

## No expected MCU response

`00 00` only indicates that the expected response was not received. Missing RAM
firmware, reset, transport and power problems can all require investigation;
the bytes alone prove neither missing power nor the chip model.

Check the expected 10,396-byte file and SHA256 documented in
[installation](install.md), and read
`/sys/module/spidev/parameters/bufsiz`. Recovery needs a continuous 10,403-byte
transaction; the supplied setting is 32,768. A module setting change requires
a controlled reboot. Follow the backup/rollback procedure before modifying
configuration. Do not repeat unchanged failed recovery sequences on Medion;
use the known-good Mint comparison described in [status](status.md).

## Permissions

Inspect `systemctl cat fprintd.service` and actual access-denial logs before
changing permissions. The example `DeviceAllow=char-gpiochip rw` and SELinux
policy grant access to a GPIO device class, not just the fingerprint pins.
On Fedora, install the CIL module only after confirming the matching AVC denial
and recording whether a module already exists. Never disable SELinux.
See [installation and rollback](install.md) for exact affected paths.

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

