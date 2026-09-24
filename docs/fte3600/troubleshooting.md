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
- `00 00` means the expected MCU response was not received. Missing runtime
  firmware is one cause, but this value alone cannot distinguish it from reset,
  transport or power problems. Check the verified firmware installation first
  (`./scripts/install-firmware.sh`).

## Medion E3224 recovery

From an updated `medion-e3224` checkout, run:

```sh
sudo bash scripts/test-medion-recovery.sh
```

This compiles and runs the current tool (not an older `./test_medion_e3224` binary),
temporarily masks fprintd to prevent concurrent access, then restores its prior
state. Requires a compiler and the GLib, libgpiod 2.x and GUdev development files.
The tool verifies the firmware hash, checks GPIO/SPI failures, and temporarily
holds the discovered SPI ancestors active. Original power policies are restored
on exit; no power service is installed. Recovery exit 0 means identity, configuration and
MCU idle checks passed; exit 2 means no valid idle response; other failures are
reported with an error. Share the complete terminal output, without fingerprint
images or templates.

The September 23 correction follows the **FT9361** call chain in vendor DLL
2.0.3.102: reset high 10 ms / low 20 ms / high, immediately `55 aa`, upload,
wait 2 ms, two more reset pulses separated by 10 ms, wait 160 ms, then two `70`
commands and MCU polling. Runtime configuration follows successful idle and ID
checks. The five `09 f6` writes belong to the FT9338 download path; treating
`30=bb` as the FT9361 firmware jump was incorrect. This corrected Medion flow
still requires an E3224 hardware result.

Audit anchors for the DLL (SHA256
`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`):
FT9361 vtable `0x1800490b8`, download `0x180039730`, post-upload target
`0x180039c50` (ReturnIdleByReset), runtime config `0x1800399f0`.

Older diagnostics used `g_file_set_contents()` on sysfs and ignored failure,
so their `Power: ... -> on` messages did not confirm a write. A controller shown
as `suspended` **before** a transfer can resume automatically for that transfer;
neither that snapshot nor all-zero SPI responses prove the sensor rail is off.
The corrected log distinguishes kernel PM state from a measured power rail.

The diagnostic defaults to 1 MHz, matching the [reported E3224 ACPI resource](https://github.com/SamSeven777/libfprint-fte3600/issues/1#issuecomment-5559055022);
`--speed` allows lower rates. Its separate `--chip-id` mode writes scratch RAM
and only classifies a chip family, not FT9361 specifically. It is not part of
normal recovery and refuses a running MCU or the unsupported Edition A path.

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
