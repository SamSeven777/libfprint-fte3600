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

The image and firmware transactions cannot be split. Check the current kernel
limit:

```sh
cat /sys/module/spidev/parameters/bufsiz
```

It must be at least `10403` for firmware recovery (`5128` for images); the
supplied configuration sets it to `32768` after a reboot. Cold-boot recovery
also needs the owner's FT9361 firmware at
`/usr/lib/firmware/fte3600/ft9361.bin` (10,396 bytes, validated by SHA256).
The firmware is not supplied by the public source tree.

### Firmware for cold-boot recovery

Use the matching firmware obtained from the owner's Windows driver package.
Neither the public repository nor its Arch package distributes this image or
downloads it automatically. The accepted image has these exact properties:

- Size: 10,396 bytes.
- SHA256: `027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f`.
- Installed path: `/usr/lib/firmware/fte3600/ft9361.bin`.

For the matching `ftWbioUmdfDriverV2.dll`, the image begins at file offset
`0x22a00` (141,824). To extract and check it locally, use a new temporary
directory and your own copy of that DLL:

```sh
fte_fw_dir=$(mktemp -d)
dd if=/path/to/ftWbioUmdfDriverV2.dll of="$fte_fw_dir/ft9361.bin" \
  bs=1 skip=141824 count=10396 status=none
printf '%s  %s\n' \
  027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f \
  "$fte_fw_dir/ft9361.bin" | sha256sum -c -
```

This offset applies only to the matching driver version. If the check fails,
do not install the extracted file or bypass the driver's validation. Keep the
DLL and extracted image local; do not attach them to public reports.

After checking the size and hash of your local copy, install it separately:

```sh
wc -c /path/to/ft9361.bin
sha256sum /path/to/ft9361.bin
sudo install -Dm644 /path/to/ft9361.bin \
  /usr/lib/firmware/fte3600/ft9361.bin
```

If you used the extraction above, the local copy is
`$fte_fw_dir/ft9361.bin`.

`FTE3600_FIRMWARE_PATH` overrides the path for a diagnostic process; it does
not bypass size or hash validation. An environment variable set in a shell
does not configure the D-Bus-activated `fprintd` service. The driver loads the
image only if the MCU remains non-idle after reset recovery, uploads it to
sensor RAM, completes the startup sequence, and requires `a5 5a` plus the
normal sensor identity checks. Missing or mismatched firmware fails the open
when recovery is needed. Sensor flash/OTP writes are not implemented.

## 2. Install build dependencies

Arch Linux:

```sh
sudo pacman -S --needed base-devel git meson ninja glib2 glib2-devel libgusb \
  libgudev libgpiod cairo fprintd
```

Fedora 43 or newer:

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
  fpi-spi-transfer fte3600-driver fte3600-brisk fte3600-template \
  fte3600-a1-spi-power
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
current `main` branch as shown in the root [README](../../README.md).

Runtime installations need the supplied transport and service settings:

- `config/modprobe.d/fte3600-spidev.conf` so the 10,403-byte firmware upload
  and 5,128-byte image read each fit in a single SPI transaction after reboot;
- the separately installed, validated FT9361 firmware described above for
  cold-boot recovery;
- `config/systemd/10-fte3600-gpio.conf` so sandboxed `fprintd` can open the
  GPIO character device;
- `scripts/fte3600-a1-spi-power`, its systemd unit, and the fprintd drop-in so
  the verified A1's Intel LPSS/pxa2xx controller cannot enter the observed
  broken runtime-suspend state before a fingerprint operation.

The SPI power service performs exact DMI, ACPI, PCI, and SPI topology checks.
It is skipped on every non-A1 profile, and it yields to a future native kernel
driver instead of rebinding that driver to spidev.

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
