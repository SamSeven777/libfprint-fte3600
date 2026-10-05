# Preparing separate kernel and libfprint submissions

The current downstream implementation separates ACPI reset/IRQ ownership from
SPI messages, sensor protocols and host matching. It is a reviewable development
base, not an accepted upstream interface or a claim of hardware qualification.
DKMS, local binding rules and SELinux helpers are distribution integration tools;
they are not the intended contents of a kernel patch series.

## Kernel review boundary

`kernel/fte3600/` owns ACPI resource selection, physical reset forwarding, IRQ
lifetime, exclusive leases and suspend/removal invalidation. It does not contain
sensor commands, firmware uploads, image processing or a biometric matcher.
The physical SPI device currently binds to the distribution's unmodified spidev;
the platform companion exports one reset GPIO and an IRQ-only UIO device.
The [metadata and lifetime contract](../../kernel/fte3600/README.md) specifies
how userspace pairs and validates these endpoints.

Before proposing this interface upstream, resolve with the relevant maintainers
whether the companion arrangement, GPIO/UIO exports, metadata and FTE3600 SPI
binding are appropriate. In particular, stock spidev does not enforce the glue's
lease around each transfer, provide atomic PM revocation, or restore a per-open
CS mode on process death. These limits must be part of that discussion; using
existing userspace ioctls does not settle the architecture decision.

A prospective kernel series should contain the driver, Kconfig/Makefile and
MAINTAINERS integration, ABI documentation and the hardware evidence supporting
its resource and polarity decisions. This repository's external-module build
and version guards are development aids, not a substitute for building against
the target upstream tree. No such submission has been sent by this change.

## libfprint review boundary

Keep reviewable changes separate:

1. Generic SPI transfer checks and composite device enumeration, with tests
   showing that existing transports are unaffected.
2. The resource resolver and transport adapter implementing the agreed kernel
   contract, including failed acquisition, IRQ waits, cancellation and teardown.
3. Sensor identity, firmware validation, per-family initialization and capture.
   Preserve chip-specific geometry and framing; no default FT9361 fallback.
4. Host matcher adapters and their independently reusable algorithm cores.
   Authentication policy and stored-template compatibility need their own review
   and measurement evidence; capture support does not establish matching accuracy.

The standalone `medion-spidev` diagnostic remains a separate branch. Its known
board wiring and explicit FT9338/FT9348 experiment are not normal enumeration
rules and must not become an upstream laptop whitelist or automatic blind flash.

## Evidence still required

- A1, GPD Pocket 3 and Medion results tied to an exact commit, actual chip
  responses and sanitized resource metadata. Prior reports using older
  transports do not validate this implementation.
- Cold boot, close/reopen, cancellation, IRQ behavior, suspend/resume and removal
  with open descriptors. Where applicable, check native and GPIO-controlled CS
  separately and record physical reset/CS evidence rather than inferring it
  from a userspace mode bit.
- Builds on the intended upstream kernel and target distribution kernels;
  actual installation and device access under Fedora SELinux enforcing and
  Secure Boot with the distribution's module-trust procedure.
- For authentication, independently collected evaluation data and the complete
  enrollment/decision rule. Do not post fingerprint images, templates or vendor
  binaries in public issues.

Record completed software checks and their limits separately from hardware
results. The [October 5 audit](audit-acpi-spidev-2026-10-05.md) documents the
previous test gaps; subsequent fixes do not turn a simulated device into a
physical-device validation result.
