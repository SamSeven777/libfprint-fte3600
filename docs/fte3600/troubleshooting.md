# Troubleshooting

## The device is not listed

Confirm the exact hardware profile and device nodes:

```sh
cat /sys/class/dmi/id/sys_vendor
cat /sys/class/dmi/id/product_name
grep -H . /sys/bus/acpi/devices/FTE3600:*/hid
ls -l /dev/spidev* /dev/gpiochip*
fprintd-list "$USER"
```

An unknown DMI profile is rejected intentionally. Do not bypass that check;
open an issue with the sanitized evidence requested in `CONTRIBUTING.md`.

## A capture says the SPI message is too large

```sh
cat /sys/module/spidev/parameters/bufsiz
```

The result must be at least 5128. Install
`config/modprobe.d/fte3600-spidev.conf` through the package and reboot. The
5,128-byte full-duplex image read cannot be split across chip-select cycles.

## fprintd cannot open GPIO

Check the installed service drop-in and restart after fixing the package:

```sh
systemctl cat fprintd.service
systemctl status fprintd.service --no-pager
```

The effective unit must allow `char-gpiochip rw`. Keep the service sandbox in
place and do not grant broad access to all devices merely to bypass a GPIO
configuration error.

## Enrollment repeatedly asks for another press

Lift the finger completely, pause briefly, and vary position slightly on the
next press. Low contrast, too few features, a duplicate placement, or an
inconsistent finger are deliberately rejected. Do not weaken the matcher to
make enrollment complete.

## Omarchy does not show fingerprint unlock

First confirm `fprintd-verify` succeeds. Then check that
`/etc/pam.d/omarchy-lock-fingerprint` exists and restart only the shell:

```sh
omarchy restart shell
omarchy system lock
```

Do not use the general Omarchy fingerprint setup command with this package.

## Collect a safe log

Start with versions and non-biometric state:

```sh
uname -a
pkg-config --modversion libfprint-2 libgpiod
systemctl status fprintd.service --no-pager
```

Before posting, redact usernames, hostnames, serial numbers, paths, and any
binary dump. Never upload a fingerprint image, template, or raw capture.
