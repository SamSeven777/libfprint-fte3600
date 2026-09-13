# Release history

## Unreleased

- Add bounded A1 cold-boot recovery using separately supplied, size/hash-pinned
  firmware; protect sensitive transfers and set the SPI buffer to 32,768 bytes.
- Keep minimum inlier and mutual-match gates at 7 as experimental settings,
  without claiming optimality or real-population security calibration.
- Share GPIO discovery improvements and the optional Fedora SELinux module;
  load it only after confirming matching local AVC denials.
- Retain the tested, exact-A1 runtime-PM helper as a precaution. It did not
  solve the original `00 00` fault and remains unproven necessary post-fix.
- Preserve the experimental exact Medion DMI profile and separate GPIO
  controllers; hardware-reset and firmware recovery remain disabled.
- Remove development-only synthetic calibration tools and duplicate handover
  documentation; retain runtime/regression tests and the open/close diagnostic.

## fte3600-v0.1.0 - 2026-08-25

First experimental, source-only release of the clean-room FTE3600 / FT9361
libfprint driver.

### Verified

- One-Netbook A1 hardware discovery, DMI-gated GPIO reset, SPI transport, and
  IRQ-driven 64 x 80 capture;
- eight-stage enrollment, versioned template persistence, genuine verification,
  and local impostor smoke testing;
- `fprintd` verification and Omarchy lock-screen unlock with a password fallback;
- both authentication-policy build configurations on Fedora 43 and Ubuntu 26.04
  CI, plus local Arch package and sanitizer testing.

The default build exposes capture only. Enrollment and verification require
the explicit experimental `-Dfte3600_personal_auth=true` build option.

### Known limitations

- The verification policy has not completed independent, population-scale
  FAR/FRR calibration and is restricted to local lock-screen experimentation.
  Do not use it for login, `sudo`, polkit, disk encryption, passkeys, key
  release, or unattended authentication.
- Only the exact `ONE-NETBOOK TECHNOLOGY CO., LTD. / A1` hardware profile is
  enabled. Other FTE3600 systems require separate GPIO/SPI validation.
- Fedora and Ubuntu source builds are covered by CI but have not yet been
  verified on FTE3600 hardware; native RPM and DEB packages are not provided.
- The included Arch `PKGBUILD` is a checkout-based development recipe, not a
  fixed-tag AUR release recipe.
- This release contains source archives only. No private FTE3600 capture or
  enrolled template, vendor binary, firmware, or prebuilt package is
  distributed. The normal upstream libfprint test fixtures remain unchanged.
