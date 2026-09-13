# Troubleshooting

## Device missing or initialization fails

Check the exact [hardware profile](status.md), then:

```sh
ls -l /dev/spidev* /dev/gpiochip*
cat /sys/module/spidev/parameters/bufsiz
systemctl status fprintd.service --no-pager
```

Device nodes may appear only after installation and reboot. The supplied SPI
limit is `32768`: A1 recovery needs `10403`, images `5128`.

For A1 MCU `00 00`, check the current driver and
[validated firmware](install.md#firmware-for-cold-boot-recovery). All-zero reads
alone do not distinguish missing firmware from transport/power failure.

Medion reset and firmware upload remain disabled pending hardware validation;
installing A1 firmware does not enable them. Do not bypass the profile checks.

## fprintd cannot open GPIO

`systemctl cat fprintd.service` must include `DeviceAllow=char-gpiochip rw`
from the supplied GPIO drop-in.

On Fedora 43/44 with enforcing SELinux and `Permission denied`, inspect:

```sh
sudo ausearch -m AVC,USER_AVC,SELINUX_ERR,USER_SELINUX_ERR \
  -ts recent -c fprintd -i
ls -lZ /dev/gpiochip*
ps -eZ | grep '[f]printd'
```

Only if the AVC confirms source `fprintd_t`, target `gpio_device_t`, and the
matching GPIO denial, load the optional module from the repository root:

```sh
sudo semodule -i config/selinux/fte3600-gpio.cil
sudo semodule -lfull | grep -F fte3600-gpio
sudo systemctl restart fprintd.service
```

`semodule` comes from Fedora's `policycoreutils` package. The module permits
`getattr/open/read/write/ioctl` on **all** GPIO devices labeled `gpio_device_t`
for `fprintd_t`; the systemd allow-list still applies. It is never automatically
loaded, including on Arch/Ubuntu. Do not disable SELinux or apply this to
unmatched contexts. Remove it with `sudo semodule -r fte3600-gpio`.

## A1 power workaround

The retained helper sets both Intel LPSS/pxa2xx policies to `on` and rebinds
the controller once per service activation. It did not fix the original cold
boot; whether it remains necessary with corrected firmware is unverified.
Check its state without changing it:

```sh
systemctl status fte3600-a1-spi-power.service --no-pager
cat /sys/devices/pci0000:00/0000:00:1e.3/power/control
cat /sys/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.4/power/control
```

Both controls should be `on` while active. A successful service status does
not establish sensor health. Removing the workaround needs a controlled
on/auto comparison and a cold boot without it; fprintd can automatically
restart it. Never rebind during an active fingerprint operation.

## Enrollment and reports

Lift completely between presses and vary placement slightly. Verify directly
with `fprintd-verify` before [Omarchy setup](arch-omarchy.md).

Report software versions, the failing operation, and sanitized DMI/ACPI/error
text. Keep debug logging disabled; omit images, templates, raw captures,
firmware, vendor binaries, serial numbers, usernames, and private logs.
