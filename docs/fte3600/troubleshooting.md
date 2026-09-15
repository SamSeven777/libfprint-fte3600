# Troubleshooting

## Device missing or initialization fails

Check that your device matches a [supported hardware profile](status.md), then verify the character nodes and module configuration:

```sh
ls -l /dev/spidev* /dev/gpiochip*
cat /sys/module/spidev/parameters/bufsiz
systemctl status fprintd.service --no-pager
```

- `/dev/spidev*` nodes are bound by udev rules upon installation and system reboot.
- `cat /sys/module/spidev/parameters/bufsiz` must report `32768` (required for the 10,403-byte cold-boot firmware payload). If it reports `4096` or `8192`, install `config/modprobe.d/fte3600-spidev.conf` and reboot.
- If the journal reports `FT9361 MCU did not return to idle (00 00)`, the sensor has lost its runtime SRAM code after power-off. Ensure `/usr/lib/firmware/fte3600/ft9361.bin` is installed (run `./scripts/install-firmware.sh`).

## fprintd cannot open GPIO (Permission denied)

Ensure `systemctl cat fprintd.service` includes `DeviceAllow=char-gpiochip rw` (installed by `10-fte3600-gpio.conf`).

On Fedora with SELinux in Enforcing mode, `fprintd_t` is blocked from opening `gpio_device_t` by default. Install the provided SELinux CIL policy:

```sh
sudo semodule -i config/selinux/fte3600-gpio.cil
sudo systemctl restart fprintd.service
```

Verify that the module is active:
```sh
semodule -l | grep fte3600
```

## Enrollment tips

- **Lift completely** between presses and vary placement slightly (center, edges, slight rotation) across the 8 enrollment stages.
- Test authentication locally using `fprintd-verify "$USER"`. Keep biometric authentication confined to local lock-screen experiments and maintain a working password fallback; do not enable for system-wide sudo or root authentication.
- When filing issues, provide distribution details, kernel version, DMI information, and relevant journal logs (`journalctl -u fprintd -b`). Do not attach private logs or biometric templates.
