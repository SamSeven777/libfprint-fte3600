# Experimental FTE3600 / FT9361 support

This branch adds a clean-room `libfprint` driver for the FocalTech fingerprint
sensor exposed as `ACPI\FTE3600` in the **One-Netbook A1**. It supports the
FT9361 SPI transport, capture, eight-stage enrollment, and an explicitly
opt-in personal verification policy.

> [!WARNING]
> The verification policy has not completed independent, multi-person,
> multi-session FAR/FRR calibration. Use it only for a local lock screen with
> a tested password fallback. Do not enable it for login, `sudo`, polkit, disk
> encryption, passkeys, or unattended security decisions.

The hardware path is currently validated only on Arch Linux / Omarchy on the
exact DMI profile `ONE-NETBOOK TECHNOLOGY CO., LTD. / A1`. Fedora and Ubuntu
can use the same Linux driver core when they provide `libgpiod >= 2.0`,
`spidev`, and `fprintd`; those distribution integrations still need additional
hardware reports. Ubuntu 22.04 and 24.04 ship libgpiod 1.x and therefore cannot
build this revision without a libgpiod 2.x backport.

- [Status and supported hardware](docs/fte3600/status.md)
- [Build and installation](docs/fte3600/install.md)
- [Arch / Omarchy setup](docs/fte3600/arch-omarchy.md)
- [Security boundary](SECURITY.md)
- [Clean-room implementation notes](docs/fte3600/clean-room.md)
- [Troubleshooting](docs/fte3600/troubleshooting.md)
- [Contributing hardware reports](CONTRIBUTING.md)

No private FTE3600 capture, enrolled template, proprietary vendor binary,
firmware image, or decompiler database is included in this branch. The normal
upstream libfprint test fixtures remain unchanged.

---

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
