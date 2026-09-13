# Release history

## Unreleased

- Recover FT9361 after cold boot using one size- and SHA256-pinned,
  owner-supplied firmware image, followed by the verified startup sequence.
  The owner confirmed cold-boot initialization and fingerprint recognition
  on One-Netbook A1. Firmware remains separate from source and packages.
- Require a single 10,403-byte firmware transaction, redact its payload,
  and set the packaged spidev buffer to 32,768 bytes with a corrected checksum.
- Bound firmware file reads and post-reset MCU polling, clean up firmware on
  initialization errors, and cover invalid firmware inputs with generated tests.
- Replace obsolete raw SPI probes with an open/close diagnostic and update
  recovery, installation, and firmware-boundary documentation.
- Add a fail-closed One-Netbook A1 systemd helper that disables runtime PM for
  the verified Intel LPSS/pxa2xx SPI path before `fprintd`, rebinds the
  controller when required, and restores the previous policy on removal.
- Add fake-sysfs tests for DMI and topology gating, transactional power-policy
  handling, native-driver precedence, and state-file validation.
- Document the repeated all-zero/`0x95` SPI failure and its A1-only workaround.

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
