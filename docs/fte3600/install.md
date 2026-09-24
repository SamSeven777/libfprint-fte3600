# Build and installation

Clone the repository and run all commands from the repository root:

```sh
git clone --branch medion-e3224 https://github.com/SamSeven777/libfprint-fte3600.git
cd libfprint-fte3600
```

See [hardware status](status.md) for reported platforms, evidence and requirements.

## Dependencies

Arch Linux:

```sh
sudo pacman -S --needed base-devel git meson ninja glib2 glib2-devel libgusb \
  libgudev libgpiod cairo fprintd
```

Fedora 43+:

```sh
sudo dnf install git gcc gcc-c++ meson ninja-build pkgconf-pkg-config \
  glib2-devel libgusb-devel libgudev-devel libgpiod-devel systemd-devel \
  systemd cairo-devel fprintd
```

Ubuntu 26.04+:

```sh
sudo apt install build-essential git meson ninja-build pkg-config \
  libglib2.0-dev libgusb-dev libgudev-1.0-dev libgpiod-dev libudev-dev \
  systemd-dev libcairo2-dev fprintd
```

Requires libgpiod 2.x; Ubuntu 22.04/24.04 need a backport. Fedora and Ubuntu
are build-tested; hardware validation is on Arch/Omarchy.

## Record a rollback plan before installing

Prefer the distribution package path on a test machine. Manual `--prefix=/usr`
installation can overwrite package-owned files and is not tracked as a separate
package; a later system update can overwrite this fork. Keep the build directory,
source commit, build options and `meson-logs/install-log.txt`.

Before installing, list the destinations with
`meson introspect build-fte3600 --installed` after configuration. Record which
files were already present and which package owned them. Back up each preexisting
file that you will overwrite and retain the original package version.

Do the same **before** installing any of these optional/manual items:

- `/etc/modprobe.d/fte3600-spidev.conf`
- `/etc/systemd/system/fprintd.service.d/10-fte3600-gpio.conf`
- `/usr/lib/firmware/fte3600/ft9361.bin`
- The SELinux module named `fte3600-gpio`, if applicable.

Record an absent destination explicitly; absence and an existing identical file
are different rollback cases. Keep a copy/hash of the newly installed file as
well. If an existing configuration or SELinux module belongs to somebody else,
review/merge it instead of blindly overwriting it. Do not proceed without a
working password login and a recovery path.

## Firmware for cold-boot recovery

Cold-boot recovery requires a 10,396-byte image at
`/usr/lib/firmware/fte3600/ft9361.bin`, with SHA256
`027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f`.
Neither the repository nor its packages bundle firmware.

### Automated installer (Recommended)

Run the included extraction script. It downloads the signed vendor
package directly from the Microsoft Update Catalog, cuts and verifies the
firmware image, and installs it to `/usr/lib/firmware/fte3600/ft9361.bin`:

```sh
./scripts/install-firmware.sh
```

*(Requires `cabextract` or `7z`, and `curl` or `wget`).*

### Manual extraction from Windows driver

If you already have a copy of `ftWbioUmdfDriverV2.dll`, you can pass it to the
installer script or extract manually using `dd`:

```sh
# Automated extraction from local DLL or CAB:
./scripts/install-firmware.sh /path/to/ftWbioUmdfDriverV2.dll

# Or manual extraction (offset 489824 for driver 2.0.3.102, 141824 for 2.0.3.100):
fte_fw_dir=$(mktemp -d) &&
dd if=/path/to/ftWbioUmdfDriverV2.dll of="$fte_fw_dir/ft9361.bin" \
  bs=1 skip=489824 count=10396 status=none &&
printf '%s  %s\n' \
  027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f \
  "$fte_fw_dir/ft9361.bin" | sha256sum -c - &&
sudo install -Dm644 "$fte_fw_dir/ft9361.bin" \
  /usr/lib/firmware/fte3600/ft9361.bin
```

Offsets vary across driver versions; do not bypass a hash mismatch.
`FTE3600_FIRMWARE_PATH` overrides the path for a diagnostic process, preserving
validation and hardware gating. Shell variables do not configure D-Bus fprintd.

## Build and install

The default build exposes capture only. Host-side enrollment and verification
require `-Dfte3600_personal_auth=true` (the Arch package enables this by default).
Ensure you have a working root/sudo password as fallback before enabling biometric
authentication on your system.

> [!IMPORTANT]
> Policy Version 3 uses revised experimental geometric consensus gates.
> If upgrading from earlier prototype builds, existing stored templates will be rejected by design;
> simply re-enroll using `fprintd-enroll "$USER"`. Note that multi-person, population-level
> FAR/FRR remains unmeasured; keep biometric usage confined to personal experiments with
> a working password fallback.

### Arch package

From the committed Git checkout (GitHub source archives lack required metadata):

```sh
(cd packaging/arch && makepkg -si)
sudo reboot
```

This replaces stock libfprint and installs the SPI buffer and GPIO settings below.


### Meson build

From the repository root:

```sh
meson setup build-fte3600 --prefix=/usr \
  -Ddrivers=fte3600 -Dfte3600_personal_auth=false \
  -Dgtk-examples=false -Ddoc=false -Dintrospection=false \
  -Dinstalled-tests=false -Dwerror=true
meson compile -C build-fte3600
meson test -C build-fte3600 --print-errorlogs \
  fpi-spi-transfer fte3600-driver fte3600-brisk fte3600-template fte3600-lifecycle \
  medion-power medion-diagnostic
```

To opt in, configure `meson configure build-fte3600 -Dfte3600_personal_auth=true`
and repeat compile/test. `./scripts/check-fte3600.sh` tests both policies.

Fedora/Ubuntu have no native fork packages. On a test system, manual installation
is possible but untracked by the package manager and may be overwritten:

```sh
sudo meson install -C build-fte3600
sudo install -Dm644 config/modprobe.d/fte3600-spidev.conf \
  /etc/modprobe.d/fte3600-spidev.conf
sudo install -Dm644 config/systemd/10-fte3600-gpio.conf \
  /etc/systemd/system/fprintd.service.d/10-fte3600-gpio.conf
sudo systemctl daemon-reload
sudo reboot
```

After reboot, `/dev/spidev*` must exist and
`cat /sys/module/spidev/parameters/bufsiz` should report `32768`.
Recovery needs at least `10403` bytes; images need `5128`. Neither transfer
may be split. The GPIO drop-in permits `char-gpiochip rw` inside fprintd's sandbox.
For permission failures, see the optional
[Fedora SELinux policy](troubleshooting.md#fprintd-cannot-open-gpio).

## Verify

With the personal policy enabled, keep a working password and run:

```sh
fprintd-enroll -f left-index-finger "$USER"
fprintd-verify -f left-index-finger "$USER"
```

For A1 cold-boot acceptance, shut down and boot directly into Linux, then verify.
[Omarchy lock-screen setup](arch-omarchy.md) is separate. Removing enrollment
with `fprintd-delete "$USER"` requires re-enrollment.

## Removing this fork and restoring the previous installation

Close fingerprint clients and stop `fprintd.service` while restoring libraries.
Do not run a broad `rm` against library directories or an unreviewed
`ninja uninstall`: those can remove files shared with the distribution package.

1. **Arch package installation:** reinstall the distribution's `libfprint`
   with `sudo pacman -S libfprint`, accepting replacement of the conflicting
   fork package only after checking the transaction. Its package-owned service
   and modprobe drop-ins are removed by the package manager; the removal hook
   reloads systemd/udev. Files installed manually in `/etc` or the firmware path
   are separate and are not removed by this transaction.
2. **Manual library installation:** compare the retained Meson install log with
   your pre-install record. Restore overwritten files from the original package
   or backup. Reinstall the original library package (for example
   `sudo dnf reinstall libfprint` on Fedora or
   `sudo apt install --reinstall libfprint-2-2` on Ubuntu).
   Reinstallation does not necessarily remove extra files introduced by a newer
   source build: review the exact logged destinations and remove only files
   recorded as previously absent, still matching your installed copy, and not
   now owned by another package. If those conditions are unknown, stop and
   resolve ownership instead of deleting files.
3. **Manual configuration and firmware:** for each exact path in the rollback
   list above, restore the saved original if one existed. Remove a file only if
   you recorded it as absent before this installation and it still matches the
   copy you installed. Leave subsequently edited/unrecognized files untouched
   for manual review. Do not delete their parent directories.
4. **SELinux:** only if this procedure added a previously absent
   `fte3600-gpio` module, remove that exact module with
   `sudo semodule -r fte3600-gpio`. If a policy existed before, restore its saved
   original instead. Do not remove other modules or disable SELinux.
5. Reload the library cache with `sudo ldconfig`, run
   `sudo systemctl daemon-reload` and `sudo udevadm control --reload`, then reboot
   to restore the prior spidev module configuration. Verify password login and
   the restored package/service state before changing authentication settings.

These steps do not delete enrolled fingerprints. If you deliberately wish to
remove enrollment, `fprintd-delete "$USER"` deletes that user's enrolled prints;
it is separate from removing the driver and requires subsequent re-enrollment.
Restore any PAM changes from their own pre-change backups; this guide does not
authorize removal of an existing password authentication flow.
