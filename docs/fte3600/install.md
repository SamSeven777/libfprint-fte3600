# Installation

Build on the target distribution. The driver uses Linux spidev, the included
ACPI reset/IRQ module (ABI 2), matching kernel headers and fprintd.
Run the commands below from the repository root.

| Distribution | Build dependencies |
| --- | --- |
| Ubuntu | `build-essential pkg-config meson ninja-build python3 libglib2.0-dev libgusb-dev libgudev-1.0-dev libudev-dev systemd-dev` |
| Fedora | `gcc gcc-c++ make pkgconf-pkg-config meson ninja-build python3 glib2-devel libgusb-devel libgudev-devel systemd-devel` |
| Arch | `base-devel pkgconf meson ninja python glib2 libgusb libgudev systemd` |

Install fprintd and the headers for the running kernel separately. On NixOS,
package the library and kernel module in derivations, and configure firmware
and fprintd permissions declaratively. The installation commands below target
conventional distributions.

## Build

```sh
meson setup build --prefix=/usr --buildtype=release -Ddrivers=fte3600 \
  -Dgtk-examples=false -Ddoc=false -Dintrospection=false -Dinstalled-tests=false
meson compile -C build
```

`-Ddrivers=fte3600` selects this reader. Add other libfprint drivers to that
option as needed. BRISK authentication is enabled with the FTE3600 driver.

## Kernel module and chip identification

```sh
sudo ./scripts/setup-fte3600.sh install-kernel
./scripts/fte3600-tool.sh check
cat /sys/module/spidev/parameters/bufsiz
```

The SPI buffer must be at least `65536` bytes. Reboot after changing the buffer
size of a loaded spidev module or replacing a loaded glue module. Secure Boot
requires a trusted module signature. Reset and IRQ resources come from ACPI.

Use the [hardware tool](tools.md) to identify the chip and select its firmware.
`info` reports device connections; `probe` runs chip detection and initialization.
A missing-firmware result includes the available profile information.

## Firmware

| Chip | Required files |
| --- | --- |
| FT9338, FT9348, FT9361, FT9536 | Corresponding chip payload |
| FT9368 | Application and PRAM boot files, plus `ft9536.bin`; install both `ft9368` and `ft9536` selections |
| FT9365, FW9369, FT9769 | No external payload required |

Example for FT9361:

```sh
sudo ./scripts/install-firmware.sh --chip ft9361 \
  --input /path/to/driver.dll --destdir /usr/lib/firmware
```

Run `./scripts/install-firmware.sh --help` for supported inputs and download
options. The installer verifies file sizes and hashes. The runtime reads raw,
regular payload files from `/usr/lib/firmware/fte3600`; it rejects compressed
files and payload symlinks.

For another location, set `-Dfte3600-firmware-dir=/path/to/firmware/fte3600`
when building. On NixOS, use a store directory containing the raw files.

## Library and service

Use distribution packaging to integrate the build, or install directly:

```sh
sudo meson install -C build
sudo ldconfig
sudo ./scripts/setup-fte3600.sh install-systemd
sudo ./scripts/setup-fte3600.sh install-selinux
sudo systemctl restart fprintd.service
```

fprintd requires root access to the paired SPI/GPIO/UIO devices. Inspect the
service permissions with `./scripts/fte3600-device-allow.sh` and
`systemctl cat fprintd.service`. On enforcing SELinux systems, the helper
installs the scoped device policy.

## Enrollment and verification

Keep the sensor untouched during startup. Lift and replace the finger between
the eight accepted enrollment samples:

```sh
fprintd-enroll -f right-index-finger "$USER"
fprintd-verify -f right-index-finger "$USER"
```

Low-quality, duplicate and inconsistent samples trigger retries. The current
main branch implements explicit release detection for FT9368, FW9369 and
FT9769. See the [hardware reports](../../README.md#hardware-reports) for tested
devices, suspend/resume results and pending fixes.

## Troubleshooting and removal

Run `./scripts/fte3600-tool.sh check`, collect a `probe` report, and inspect
`journalctl -u fprintd -b`. These cover device pairing, permissions, firmware
and startup errors. Include the tested commit in a hardware report.

If setup reports an old bridge installation, remove it with that version's
installer and reboot before continuing.

To remove the kernel and service integration:

```sh
sudo ./scripts/setup-fte3600.sh uninstall
```

Firmware and enrolled prints remain in place. To restore the distribution
library, reinstall its libfprint package and restart fprintd.
