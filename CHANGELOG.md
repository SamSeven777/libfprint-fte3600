# Release history

[Documentation index](docs/fte3600/README.md)

## Unreleased

- Replace the custom SPI bridge and DMI pin profiles with stock spidev and an
  external ACPI reset/IRQ glue module. ABI 2 exposes one reset GPIO and an
  IRQ-only UIO companion, with identity-checked pairing and session invalidation
  across suspend/removal. This transport still requires an out-of-tree module
  and is not an upstream kernel feature.
- Select eight sensor profiles across six protocol families from observed
  hardware responses. Add bounded legacy wake, family-specific initialization,
  capture, cleanup and identity-conflict handling; unknown responses do not
  select a default FT9361 backend or firmware.
- Validate separately supplied firmware by exact size/hash. Legacy RAM recovery
  and the explicit FT9368 application-flash update remain distinct operations;
  unidentified blank FT9338 devices have no automatic recovery path.
- Use native sensor geometry for BRISK enrollment and verification. New templates
  use schema 3 / diagnostic policy 7 / authentication policy 8, with isolated
  legacy wire-v1 compatibility. Unsupported template revisions require
  re-enrollment. Optional IPA/dual authentication remains a separate opt-in
  restricted to FT9361; equal image dimensions do not enable it on other chips.
  Neither matcher has population FAR/FRR or liveness qualification.
- Separate FW9369 final shutdown from reusable awake-idle cleanup, add bounded
  communication recovery, and implement FT9368's documented wake-response
  retry. Windows background calibration and all system-power paths are not
  claimed to be ported.
- Restrict systemd/SELinux access to verified companion nodes. Apply labels on
  delayed udev pairing events, restore surviving nodes during incomplete-pair
  removal, and drain queued label work before deleting policy. Require the
  `systemd` runtime package for device-unit escaping.
- Expand synthetic protocol, lifecycle, matcher, transport and installer
  regression coverage. See the
  [release validation record](docs/fte3600/validation-release-2026-10-05.md) for
  exact revisions, executed checks and remaining hardware limits.

Earlier development notes described Policy Version 3 and historical offline
comparisons. Those results are not calibration evidence for the current
eight-sample-or-mosaic decision, and are not a current authentication guarantee.

## fte3600-v0.1.0 - 2026-08-25

Historical release record: the scope and limitations below describe v0.1.0,
not the current transport or sensor-family support.

First experimental, source-only release of the clean-room FTE3600 / FT9361
libfprint driver.

### Verified

- One-Netbook A1 hardware discovery, DMI-gated GPIO reset, SPI transport, and
  IRQ-driven 64 x 80 capture;
- eight-stage enrollment, versioned template persistence, genuine verification,
  and local impostor smoke testing;
- `fprintd` verification and desktop lock-screen unlock with a password fallback;
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
