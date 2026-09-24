# Security Policy

## Authentication Policy

Default builds expose image capture only. Opt-in host-side verification is enabled with
`-Dfte3600_personal_auth=true`.

Host-side authentication (policy version 3) operates with geometric consensus gates
(minimum 5 mutual matches and inliers, spatial variance constraints, and error residual bounds).
In offline evaluation across 342,720 pairwise test comparisons, zero false accepts were observed
under specific test conditions. However, multi-person, multi-session population FAR/FRR across
the full 8-subtemplate pipeline has not been independently measured. Authentication is provided
strictly for experimental personal use; always ensure a reliable root/password fallback is available.

## Hardware Safety & Gating

The driver enforces a fail-closed hardware profile table during device probe. Reset and
interrupt GPIO lines are claimed and operated only on verified platforms (One-Netbook A1 in main).
Unknown hardware identities fail probe to protect unverified motherboards from incorrect GPIO assertions.

Cold-boot recovery uploads only the size-pinned (10,396 bytes) and SHA256-verified firmware
directly into volatile sensor SRAM. Persistent flash/OTP writes and unverified firmware updates
are not implemented.

## Privacy & Data Handling

- SPI transfer payloads containing raw biometric images or firmware are marked sensitive and redacted in debug logs.
- The driver explicitly zeroes its primary owned capture and template buffers after processing, though transient heap copies or GLib/GBytes buffers during serialization are not cryptographically sanitized.
- Enrolled templates are persisted by `libfprint` / `fprintd` in `/var/lib/fprint/`.
- Please report security vulnerabilities via GitHub Private Vulnerability Reporting.
- Public issue reports should include only sanitized diagnostics, hardware IDs, and error messages.
  Never share raw biometric images, enrolled templates, or proprietary binaries.
