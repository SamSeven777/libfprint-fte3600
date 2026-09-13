# Build and installation

Use the branch-specific clone command in [README](../../README.md), then run
commands from the repository root unless stated otherwise. Do not bypass the
[hardware profile](status.md) checks.

This branch accepts the verified `ONE-NETBOOK TECHNOLOGY CO., LTD. / A1`
and experimental Medion profile with all four DMI fields:
`sys_vendor=MEDION`, `product_name=E3224`, `product_version=FT`,
`board_name=YS13G`. Medion uses IRQ `0x00` on `\_SB_.GPO2`; reset `0x27`
on `\_SB_.GPO1` is not driven, and firmware recovery is disabled.

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

## Firmware for cold-boot recovery

A1 recovery requires an owner-supplied 10,396-byte image at
`/usr/lib/firmware/fte3600/ft9361.bin`, with SHA256
`027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f`.
Neither the repository nor its packages bundle or download firmware.

For the matching Windows `ftWbioUmdfDriverV2.dll`, extract from file offset
`0x22a00` (141,824). Replace the DLL path below with your own copy; installation
runs only if extraction and checksum validation succeed:

```sh
fte_fw_dir=$(mktemp -d) &&
dd if=/path/to/ftWbioUmdfDriverV2.dll of="$fte_fw_dir/ft9361.bin" \
  bs=1 skip=141824 count=10396 status=none &&
printf '%s  %s\n' \
  027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f \
  "$fte_fw_dir/ft9361.bin" | sha256sum -c - &&
sudo install -Dm644 "$fte_fw_dir/ft9361.bin" \
  /usr/lib/firmware/fte3600/ft9361.bin
```

The offset is driver-version-specific; do not bypass a hash mismatch.
`FTE3600_FIRMWARE_PATH` overrides the path for a diagnostic process, preserving
validation and hardware gating. Shell variables do not configure D-Bus fprintd.

## Build and install

The default build exposes capture only. Experimental enrollment and verification
require `-Dfte3600_personal_auth=true`; the Arch package already opts in.
Before using either, check existing PAM integration:

```sh
grep -R --line-number 'pam_fprintd.so' /etc/pam.d 2>/dev/null
```

Remove any global login/sudo/polkit integration through your distribution's
PAM tools while password access still works. See [SECURITY](../../SECURITY.md).

### Arch package

From the committed Git checkout (GitHub source archives lack required metadata):

```sh
(cd packaging/arch && makepkg -si)
sudo reboot
```

This replaces stock libfprint and installs the SPI/GPIO/service settings below.

### Meson build

From the repository root:

```sh
meson setup build-fte3600 --prefix=/usr \
  -Ddrivers=fte3600 -Dfte3600_personal_auth=false \
  -Dgtk-examples=false -Ddoc=false -Dintrospection=false \
  -Dinstalled-tests=false -Dwerror=true
meson compile -C build-fte3600
meson test -C build-fte3600 --print-errorlogs \
  fpi-spi-transfer fte3600-driver fte3600-brisk fte3600-template \
  fte3600-a1-spi-power
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
sudo install -Dm755 scripts/fte3600-a1-spi-power \
  /usr/lib/libfprint/fte3600-a1-spi-power
sudo install -Dm644 config/systemd/fte3600-a1-spi-power.service \
  /etc/systemd/system/fte3600-a1-spi-power.service
sudo install -Dm644 config/systemd/20-fte3600-a1-spi-power.conf \
  /etc/systemd/system/fprintd.service.d/20-fte3600-a1-spi-power.conf
sudo systemctl daemon-reload
sudo reboot
```

After reboot, `/dev/spidev*` must exist and
`cat /sys/module/spidev/parameters/bufsiz` should report `32768`.
Recovery needs at least `10403` bytes; images need `5128`. Neither transfer
may be split. The GPIO drop-in permits `char-gpiochip rw` inside fprintd's sandbox.
For permission failures, see the optional
[Fedora SELinux policy](troubleshooting.md#fprintd-cannot-open-gpio).

The A1 power helper remains a precaution; its necessity after firmware recovery
is unproven. It does nothing on other models. See
[power troubleshooting](troubleshooting.md#a1-power-workaround).

## Verify

With the personal policy enabled, keep a working password and run:

```sh
fprintd-enroll -f left-index-finger "$USER"
fprintd-verify -f left-index-finger "$USER"
```

For A1 cold-boot acceptance, shut down and boot directly into Linux, then verify.
[Omarchy lock-screen setup](arch-omarchy.md) is separate. Removing enrollment
with `fprintd-delete "$USER"` requires re-enrollment.
