# Troubleshooting

## Device missing or initialization fails

Check that your device matches a [supported hardware profile](status.md), then verify the character nodes and module configuration:

```sh
ls -l /dev/spidev* /dev/gpiochip*
cat /sys/module/spidev/parameters/bufsiz
systemctl status fprintd.service --no-pager
```

- `/dev/spidev*` requires the matching udev rule and successful binding; inspect
  the actual rule, modalias and device logs if it is absent.
- Record `/sys/module/spidev/parameters/bufsiz`. Recovery needs at least 10,403
  bytes and the supplied configuration uses `32768`; the two six-byte status
  reads do not require that larger buffer. Do not change the setting or reboot
  just to collect a baseline.
- `00 00` means the expected MCU response was not received. Missing runtime
  firmware is one cause, but this value alone cannot distinguish it from reset,
  transport or power problems. Inspect existing logs and the current firmware
  file's presence, size and hash against [installation](install.md). Do not
  automatically download/install firmware or reset the sensor before observing
  its current state.

## Medion E3224 recovery

The same reported machine worked with the older Mint stack. Treat that as the
known-good control. Do not rerun the unchanged recovery sequence that has already
failed; first identify a concrete difference to test.

The current diagnostic's default / `--status-no-reset` mode uses only the two
existing application-state/geometry SPI reads. It does not discover/claim GPIO,
reset, enter ROM, write scratch RAM or upload firmware. It is still an **active
SPI query**, not a passive electrical measurement or a promise to preserve every
device state. This mode saves SPI mode, word size and bit order before changing
them, and attempts to restore them along with runtime-power policies on exit.
Restoration errors are reported and return a failure status. Speed is selected
per transfer; the shared maximum-speed setting is not changed. As with other
cleanup, forced termination or process/system failure can prevent restoration.

The legacy `--probe` mode sends two software resets and is explicitly mutating.
`--chip-id` also writes scratch RAM. Neither is a read-only baseline.
A maintainer-directed recovery experiment may use
`sudo bash scripts/test-medion-recovery.sh`, but only after explaining the new
hypothesis, expected evidence and state changes; this is not the next automatic
step for the existing failure.

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

Inspect `systemctl cat fprintd.service` and the actual denial logs first. The
example `DeviceAllow=char-gpiochip rw` grants access to a GPIO device class, not
just these fingerprint pins. SELinux is not necessarily the cause of a failure.

Consider the provided CIL policy only after confirming an actual AVC denial
for `fprintd_t` accessing the required GPIO device (typically `gpio_device_t`).
Review the policy, record whether a module named `fte3600-gpio` already exists,
and retain its previous configuration as described in
[installation and rollback](install.md). Only if that evidence warrants the
change:

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
