# Build and installation

## 1. Confirm the hardware profile

Run these read-only checks before building:

```sh
cat /sys/class/dmi/id/sys_vendor
cat /sys/class/dmi/id/product_name
grep -H . /sys/bus/acpi/devices/FTE3600:*/hid
ls -l /dev/spidev*
```

This revision supports only `ONE-NETBOOK TECHNOLOGY CO., LTD. / A1`. Do not
force the DMI check on another model; an incorrect GPIO reset mapping can
affect unrelated hardware.

The image transaction cannot be split. Check the current kernel limit:

```sh
cat /sys/module/spidev/parameters/bufsiz
```

It must be at least `5128`; the supplied configuration sets it to `8192` after
a reboot.

## 2. Install build dependencies

Arch Linux:

```sh
sudo pacman -S --needed base-devel git meson ninja glib2 glib2-devel libgusb \
  libgudev libgpiod cairo fprintd
```

Fedora 42 or newer:

```sh
sudo dnf install git gcc gcc-c++ meson ninja-build pkgconf-pkg-config \
  glib2-devel libgusb-devel libgudev-devel libgpiod-devel systemd-devel \
  systemd cairo-devel fprintd
```

Ubuntu 26.04 or newer:

```sh
sudo apt install build-essential git meson ninja-build pkg-config \
  libglib2.0-dev libgusb-dev libgudev-1.0-dev libgpiod-dev libudev-dev \
  systemd-dev libcairo2-dev fprintd
```

Verify `pkg-config --modversion libgpiod` reports 2.0 or newer. Ubuntu 22.04
and 24.04 do not meet this requirement with their official development
package.

## 3. Build and test

The safe default exposes capture but not authentication:

```sh
meson setup build-fte3600-safe \
  -Ddrivers=fte3600 \
  -Dfte3600_personal_auth=false \
  -Dgtk-examples=false -Ddoc=false -Dintrospection=false \
  -Dwerror=true
meson compile -C build-fte3600-safe
meson test -C build-fte3600-safe --print-errorlogs \
  fpi-spi-transfer fte3600-driver fte3600-brisk fte3600-template
```

For the explicitly experimental enrollment/verification policy, change the
option to `-Dfte3600_personal_auth=true`. The repository helper tests both
configurations:

```sh
./scripts/check-fte3600.sh
```

To inspect installation contents without changing the host:

```sh
fte_stage="$PWD/build-fte3600-stage"
rm -rf -- "$fte_stage"
DESTDIR="$fte_stage" meson install -C build-fte3600-safe
find "$fte_stage" -type f -print
```

## 4. Install through a distribution package

Replacing a distribution's `libfprint` with `sudo meson install` can leave an
untracked library and break package upgrades. Arch users should use the
included [PKGBUILD](../../packaging/arch/PKGBUILD). Fedora and Ubuntu builds
are covered by CI, but native RPM/DEB replacement packages have not yet been
published; distribution packagers are welcome.

The Arch recipe explicitly enables `-Dfte3600_personal_auth=true`. Before
installing it, check that `pam_fprintd.so` is not already connected to login,
`sudo`, polkit, or another global PAM path. GitHub's generated source archives
also lack the Git metadata required by this development recipe; clone the
release tag as shown in the root [README](../../README.md).

Runtime installations need both supplied settings:

- `config/modprobe.d/fte3600-spidev.conf` so one 5,128-byte SPI transaction is
  permitted after reboot;
- `config/systemd/10-fte3600-gpio.conf` so sandboxed `fprintd` can open the
  GPIO character device.

After installation, reboot. Reloading udev alone cannot change an already
loaded spidev buffer limit or reliably re-enumerate the SPI device.

## 5. Enroll and verify

Keep a working password fallback. Then run:

```sh
fprintd-enroll -f left-index-finger "$USER"
fprintd-verify -f left-index-finger "$USER"
```

Deleting host-side enrolled data is destructive and requires re-enrollment:

```sh
fprintd-delete "$USER"
```

Do not enable login, `sudo`, polkit, or disk-unlock PAM integration for the
uncalibrated personal policy.
