# Experimental FTE3600 / FT9361 support

[![FTE3600 CI](https://github.com/SamSeven777/libfprint-fte3600/actions/workflows/fte3600-ci.yml/badge.svg)](https://github.com/SamSeven777/libfprint-fte3600/actions/workflows/fte3600-ci.yml)

Unofficial libfprint fork with an independently written FT9361 SPI driver:
capture, eight-stage enrollment, and opt-in host-side verification.

> [!WARNING]
> Authentication is experimental, not independently population-calibrated.
> Use only for a local lock screen with a tested password fallback, never for
> login, `sudo`, polkit, disk encryption, passkeys, or unattended decisions.
> Default builds expose capture only; authentication requires
> `-Dfte3600_personal_auth=true`. See [SECURITY.md](SECURITY.md).

## Hardware

The verified platform is **One-Netbook A1**, exact DMI
`ONE-NETBOOK TECHNOLOGY CO., LTD. / A1`, with ACPI `FTE3600`.
Capture, enrollment, verification, and a direct Linux cold boot without the
power workaround were tested on Arch Linux / Omarchy. A1 recovery needs the
owner's separately installed firmware; no firmware is distributed here.

This branch also enables the experimental exact Medion profile:
`sys_vendor=MEDION`, `product_name=E3224`, `product_version=FT`,
`board_name=YS13G`. End-to-end validation is incomplete; its separate GPIO
mapping does not enable hardware reset or firmware recovery.

Unknown DMI/GPIO profiles fail closed; `FTE3600` alone does not imply support.
See [hardware status](docs/fte3600/status.md) before installing.

## Get started

```sh
git clone --branch medion-e3224 \
  https://github.com/SamSeven777/libfprint-fte3600.git
cd libfprint-fte3600
```

- [Installation](docs/fte3600/install.md): dependencies, firmware, build/test,
  packaging, and safe enrollment. The Arch recipe requires a Git checkout.
- [Troubleshooting](docs/fte3600/troubleshooting.md): cold boot, GPIO,
  and optional Fedora SELinux policy.
- [Optional Omarchy lock-screen setup](docs/fte3600/arch-omarchy.md).
- [Contributing](CONTRIBUTING.md) · [Release history](CHANGELOG.md).

## License and provenance

Based on upstream libfprint
[`c4654fdc85c25afdd9115bec2f95a44145ae3b94`](https://gitlab.freedesktop.org/libfprint/libfprint/-/commit/c4654fdc85c25afdd9115bec2f95a44145ae3b94)
(version `1.94.100`). New FTE3600 code is `LGPL-2.1-or-later`; see
[COPYING](COPYING) and SPDX headers. Existing upstream copyright and license
notices remain intact.

No vendor source, binary, firmware, template format, or private biometric
capture is distributed. The host driver and matcher were independently
implemented from interoperability analysis and hardware experiments; the host
does not execute a vendor DLL/ELF. A1 recovery uploads only the owner's pinned
firmware to sensor RAM. See the [clean-room boundary](docs/fte3600/clean-room.md).

Earlier work includes
[FTEXX00-Ubuntu](https://github.com/vobademi/FTEXX00-Ubuntu), whose published
kernel transport accompanied vendor-modified userspace without corresponding
published source. This project neither redistributes nor derives code from
that package. Upstream USB proposals
[!572 (FT9201)](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/572)
and [!588 (FT9362)](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/588)
use different transports/protocols; this SPI implementation is independent.

Thanks to libfprint/fprintd contributors and [Omarchy](https://omarchy.org/)
for the integration environment. [OpenAI Codex](https://openai.com/codex/)
substantially assisted implementation, testing, review, and documentation.
Runyu Qi performed physical enrollment, genuine/impostor smoke tests, and
lock-screen validation. These acknowledgements imply no endorsement or
official support.

---

Original upstream README, preserved below:

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
