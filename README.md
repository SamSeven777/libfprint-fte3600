# Experimental FTE3600 / FT9361 support

[![FTE3600 CI](https://github.com/SamSeven777/libfprint-fte3600/actions/workflows/fte3600-ci.yml/badge.svg)](https://github.com/SamSeven777/libfprint-fte3600/actions/workflows/fte3600-ci.yml)

This unofficial downstream libfprint fork adds an independently written driver
for the FocalTech fingerprint sensor exposed as `ACPI\FTE3600` in the
**One-Netbook A1**. It supports the FT9361 SPI transport, capture, eight-stage
enrollment, and an explicitly opt-in personal verification policy. It is not
an official libfprint release or an upstream-supported device.

> [!WARNING]
> The verification policy has not completed independent, multi-person,
> multi-session FAR/FRR calibration. Use it only for a local lock screen with
> a tested password fallback. Do not enable it for login, `sudo`, polkit, disk
> encryption, passkeys, or unattended security decisions.

## Supported hardware

| Item | Verified value |
| --- | --- |
| Computer | One-Netbook A1 |
| Exact DMI profile | `ONE-NETBOOK TECHNOLOGY CO., LTD. / A1` |
| ACPI hardware ID | `FTE3600` |
| Sensor | FocalTech FT9361, 64 x 80 pixels |
| Transport | SPI mode 0, 8 bits, 1 MHz, plus platform GPIO reset/IRQ |
| Hardware-tested system | Arch Linux / Omarchy |

`FTE3600` is an ACPI family identifier, not a complete compatibility claim.
The driver fails closed on every DMI/GPIO profile except the One-Netbook A1.
Do not bypass this check on another computer; open a sanitized
[hardware report](https://github.com/SamSeven777/libfprint-fte3600/issues/new?template=hardware-report.yml)
instead.

The dependency set supports Fedora 43+ and Ubuntu 26.04+. CI currently builds
the driver on Fedora 43 and Ubuntu 26.04; neither distribution has been tested
on FTE3600 hardware. Ubuntu 22.04 and 24.04 ship libgpiod 1.x and cannot build
this revision without a libgpiod 2.x backport.

## Quick start

### 1. Confirm the exact hardware profile

The first three checks are read-only and must produce the verified DMI and ACPI
identity above:

```sh
cat /sys/class/dmi/id/sys_vendor
cat /sys/class/dmi/id/product_name
grep -H . /sys/bus/acpi/devices/FTE3600:*/hid
```

The device-node checks below are diagnostic. `/dev/spidev*` and the spidev
module parameter may not exist until this fork's udev rule has been installed
and the computer has rebooted:

```sh
ls -l /dev/spidev* /dev/gpiochip*
cat /sys/module/spidev/parameters/bufsiz
```

After installation and reboot, the FTE3600 SPI node must exist and the spidev
buffer must be at least 10,403 bytes for cold-boot firmware recovery (5,128
bytes for images). The supplied configuration sets it to 32,768 bytes.

### 2. Install build and runtime dependencies

Arch Linux / Omarchy:

```sh
sudo pacman -S --needed base-devel git meson ninja glib2 glib2-devel \
  libgusb libgudev libgpiod cairo fprintd
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

Confirm that `pkg-config --modversion libgpiod` reports 2.0 or newer.

### 3. Clone the current development branch

GitHub's automatically generated source archive can be used for a direct Meson
build, but not for the included Arch development `PKGBUILD`, which requires Git
metadata. These instructions target `main`, including the A1 SPI power fix;
the older `fte3600-v0.1.0` tag does not contain that fix or its service files.

```sh
git clone --branch main \
  https://github.com/SamSeven777/libfprint-fte3600.git
cd libfprint-fte3600
```

### 4. Build and test with Meson and Ninja

The safe build below exposes capture only and cannot authenticate:

```sh
meson setup build-fte3600 --prefix=/usr \
  -Ddrivers=fte3600 \
  -Dfte3600_personal_auth=false \
  -Dgtk-examples=false -Ddoc=false -Dintrospection=false \
  -Dwerror=true
ninja -C build-fte3600
meson test -C build-fte3600 --print-errorlogs \
  fpi-spi-transfer fte3600-driver fte3600-brisk fte3600-template \
  fte3600-a1-spi-power
```

Before enabling or installing the personal policy, audit existing PAM files:

```sh
grep -R --line-number 'pam_fprintd.so' /etc/pam.d 2>/dev/null
```

Do not continue if the output connects fingerprint authentication to login,
`sudo`, polkit, GDM, or another system-wide path. On Debian/Ubuntu,
`sudo pam-auth-update` is the system-wide configurator: leave **Fingerprint
authentication** unchecked, or deselect it while the password still works.

Only after this audit, explicitly enable the experimental personal policy and
rebuild:

```sh
meson configure build-fte3600 -Dfte3600_personal_auth=true
ninja -C build-fte3600
meson test -C build-fte3600 --print-errorlogs \
  fpi-spi-transfer fte3600-driver fte3600-brisk fte3600-template \
  fte3600-a1-spi-power
```

### 5. Install

Cold-boot recovery requires the owner's matching FT9361 firmware, installed
separately at `/usr/lib/firmware/fte3600/ft9361.bin`. The source tree and Arch
package do not include it. The driver checks its exact size and SHA256 before
uploading it to sensor RAM; see [firmware installation](docs/fte3600/install.md#firmware-for-cold-boot-recovery).

On the verified Arch/Omarchy system, use the checkout-based package recipe. It
replaces the stock `libfprint`, enables the experimental personal policy, adds
the spidev/GPIO configuration, and installs a DMI-gated One-Netbook A1 SPI
runtime-power workaround. A reboot is required:

```sh
cd packaging/arch
makepkg -si
sudo reboot
```

Fedora and Ubuntu do not yet have native packages for this fork. For a
disposable test system, the already-built tree can be installed manually, but
this is not tracked by the package manager and may be overwritten by upgrades:

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

The power service is a One-Netbook A1 platform workaround, not part of the
portable matching algorithm. It checks the exact DMI, ACPI, PCI, and SPI
topology before changing anything and is skipped on every other computer. It
keeps only the affected Intel LPSS parent and pxa2xx SPI
child out of runtime suspend while the service is active; this can cause a
small increase in idle power use.

Distribution packagers should turn the staged Meson install into an RPM or DEB
instead of recommending the manual replacement above.

### 6. Enroll, verify, and configure PAM safely

`fprintd` is normally D-Bus activated; it does not need to be permanently
enabled as a service. Keep a working password, then test it directly:

```sh
fprintd-enroll -f left-index-finger "$USER"
fprintd-verify -f left-index-finger "$USER"
```

For this release, keep `pam_fprintd.so` out of login, `sudo`, polkit, GDM, and
other system-wide authentication stacks.

The only validated PAM integration is Omarchy's separate fingerprint flow for
the lock screen, which keeps password authentication independent. Return to
the repository root before installing it:

```sh
sudo install -Dm644 config/pam.d/omarchy-lock-fingerprint \
  /etc/pam.d/omarchy-lock-fingerprint
omarchy restart shell
omarchy system lock
```

Test the enrolled finger, an unenrolled finger, and the password fallback.
Delete host-side enrollment later with `fprintd-delete "$USER"`; deletion is
not reversible.

## Upstream base, license, and provenance

This fork is based on upstream libfprint development commit
[`c4654fdc85c25afdd9115bec2f95a44145ae3b94`](https://gitlab.freedesktop.org/libfprint/libfprint/-/commit/c4654fdc85c25afdd9115bec2f95a44145ae3b94),
whose project version is `1.94.100` (one commit after the upstream
`v1.94.100` tag). The project and the new FTE3600 sources are distributed
under `LGPL-2.1-or-later`; see [COPYING](COPYING) and the individual SPDX
headers. Existing upstream components retain their own copyright and license
notices.

No proprietary vendor binary blob, firmware image, vendor template format, or
vendor source code is distributed in this source tree or its package recipe.
Cold-boot recovery loads one size- and SHA256-pinned firmware image supplied
separately by the owner into sensor RAM. The host does not execute a vendor
DLL or ELF. Register behavior, transport sequencing, and the high-level
host-matching architecture were established through interoperability analysis
of the Windows package and direct experiments on the owner's hardware. The
Linux implementation, descriptor table, template format, and matching code
were independently written; see the
[clean-room boundary](docs/fte3600/clean-room.md).

No private FTE3600 capture or enrolled template is included. The existing
public upstream libfprint test fixtures remain unchanged.

## More documentation

- [Status and supported hardware](docs/fte3600/status.md)
- [Build and installation](docs/fte3600/install.md)
- [Arch / Omarchy setup](docs/fte3600/arch-omarchy.md)
- [Security boundary](SECURITY.md)
- [Clean-room implementation notes](docs/fte3600/clean-room.md)
- [Troubleshooting](docs/fte3600/troubleshooting.md)
- [Contributing hardware reports](CONTRIBUTING.md)
- [Release history](CHANGELOG.md)

## Related work

This project does not claim that Linux support for FTE3600 had no precedent:

- [FTEXX00-Ubuntu](https://github.com/vobademi/FTEXX00-Ubuntu) documents an
  earlier FocalTech stack for FTE3600, FTE4800, FTE6600, and FTE6900. Its
  published kernel/DKMS transport was paired with a vendor-modified
  `libfprint` package whose corresponding userspace source was not published.
  This repository does not redistribute or derive code from that package.
- Open upstream libfprint merge requests
  [!572](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/572)
  and
  [!588](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/588)
  propose experimental match-on-host support for FT9201 and FT9362
  respectively. Both use USB transports and different protocols; this
  ACPI/SPI FT9361 implementation is independent of them.

The intentionally narrow contribution here is a fully source-published,
auditable implementation for the exact One-Netbook A1 hardware profile. Other
FTE3600 computers are not enabled until their DMI, GPIO, SPI, and sensor
details have been independently verified.

## Acknowledgements

Special thanks to the [Omarchy](https://omarchy.org/) project and community.
Omarchy provided the Arch/Hyprland desktop and lock-screen environment used
for real-world integration testing on the One-Netbook A1.

This project was developed with substantial assistance from
[OpenAI Codex](https://openai.com/codex/), including implementation,
debugging, automated testing, documentation, security review, and release
preparation. Physical fingerprint enrollment, genuine and impostor smoke
tests, and lock-screen validation were performed by Runyu Qi on the actual
hardware.

Thanks also to the libfprint and fprintd maintainers and contributors whose
open-source foundations make an independent device driver possible. These
acknowledgements do not imply endorsement, sponsorship, or official support by
Omarchy, OpenAI, or the upstream libfprint project.

---

The remainder of this file is the original upstream libfprint README, retained
for its project history, licensing notes, and community links.

<div align="center">

# LibFPrint

*LibFPrint is part of the **[FPrint][Website]** project.*

<br/>

[![Button Website]][Website]
[![Button Documentation]][Documentation]

[![Button Supported]][Supported]
[![Button Unsupported]][Unsupported]

[![Button Contribute]][Contribute]
[![Button Contributors]][Contributors]

</div>

## History

**LibFPrint** was originally developed as part of an
academic project at the **[University Of Manchester]**.

It aimed to hide the differences between consumer
fingerprint scanners and provide a single uniform
API to application developers.

## Goal

The ultimate goal of the **FPrint** project is to make
fingerprint scanners widely and easily usable under
common Linux environments.

## License

`Section 6` of the license states that for compiled works that use
this library, such works must include **LibFPrint** copyright notices
alongside the copyright notices for the other parts of the work.

**LibFPrint** includes code from **NIST's** **[NBIS]** software distribution.

We include **Bozorth3** from the **[US Export Controlled]**
distribution, which we have determined to be fine
being shipped in an open source project.

## Get in *touch*

 - [IRC] - `#fprint` @ `irc.oftc.net`
 - [Matrix] - `#fprint:matrix.org` bridged to the IRC channel
 - [MailingList] - low traffic, not much used these days

<br/>

<div align="right">

[![Badge License]][License]

</div>


<!----------------------------------------------------------------------------->

[Documentation]: https://fprint.freedesktop.org/libfprint-dev/
[Contributors]: https://gitlab.freedesktop.org/libfprint/libfprint/-/graphs/master
[Unsupported]: https://gitlab.freedesktop.org/libfprint/wiki/-/wikis/Unsupported-Devices
[Supported]: https://fprint.freedesktop.org/supported-devices.html
[Website]: https://fprint.freedesktop.org/
[MailingList]: https://lists.freedesktop.org/mailman/listinfo/fprint
[IRC]: ircs://irc.oftc.net:6697/#fprint
[Matrix]: https://matrix.to/#/#fprint:matrix.org

[Contribute]: ./HACKING.md
[License]: ./COPYING

[University Of Manchester]: https://www.manchester.ac.uk/
[US Export Controlled]: https://fprint.freedesktop.org/us-export-control.html
[NBIS]: http://fingerprint.nist.gov/NBIS/index.html


<!---------------------------------[ Badges ]---------------------------------->

[Badge License]: https://img.shields.io/badge/License-LGPL2.1-015d93.svg?style=for-the-badge&labelColor=blue


<!---------------------------------[ Buttons ]--------------------------------->

[Button Documentation]: https://img.shields.io/badge/Documentation-04ACE6?style=for-the-badge&logoColor=white&logo=BookStack
[Button Contributors]: https://img.shields.io/badge/Contributors-FF4F8B?style=for-the-badge&logoColor=white&logo=ActiGraph
[Button Unsupported]: https://img.shields.io/badge/Unsupported_Devices-EF2D5E?style=for-the-badge&logoColor=white&logo=AdBlock
[Button Contribute]: https://img.shields.io/badge/Contribute-66459B?style=for-the-badge&logoColor=white&logo=Git
[Button Supported]: https://img.shields.io/badge/Supported_Devices-428813?style=for-the-badge&logoColor=white&logo=AdGuard
[Button Website]: https://img.shields.io/badge/Homepage-3B80AE?style=for-the-badge&logoColor=white&logo=freedesktopDotOrg
