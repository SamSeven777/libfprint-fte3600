# Security policy

## Experimental authentication boundary

The FTE3600 transport and enrollment path are functional, but the optional
personal verification policy has not completed independent, population-scale
false-accept and false-reject calibration. A successful local smoke test is
not a security-rate measurement.

Builds with `-Dfte3600_personal_auth=false` expose capture only. Builds with
`-Dfte3600_personal_auth=true` additionally expose eight-stage enrollment and
single-template verification for local experimentation.

Do not use the personal policy for login, `sudo`, polkit, disk encryption,
passkeys, key release, or any unattended security decision. The only tested
interactive integration is an Omarchy lock screen with an independently
tested password fallback.

## Hardware safety

The driver performs volatile SPI register operations and RAM image reads. It
does not upload firmware or write sensor flash/OTP. GPIO routing and hardware
reset are enabled only for the exact verified One-Netbook A1 DMI profile; an
unknown platform fails closed.

## Biometric data in memory and logs

The FTE3600 image transfer is marked sensitive, so `FP_DEBUG_TRANSFER` logs
its lengths and result but redact both SPI buffers. The driver wipes its main
raw capture, worker-image, feature, and comparison buffers, plus the in-memory
template objects it owns, at their ownership boundaries. Serialized template
bytes are deliberately handed to libfprint/fprintd for host-side persistence
and are not erased by the driver after that ownership transfer. Temporary
canonicalization/matching stack copies and image-processing-library derived
allocations may still be released normally rather than securely erased.

The explicit diagnostic capture API intentionally transfers ownership of an
`FpImage` to its caller. A caller that requests a raw image is responsible for
protecting and deleting it; normal enrollment and verification do not expose
that image.

## Reporting a vulnerability

Use the repository's private GitHub security-advisory reporting channel when
available. Do not attach fingerprint images, enrolled templates, raw SPI
captures containing biometric pixels, private system logs, proprietary vendor
binaries, or firmware to a public issue.

For a public report, include only sanitized software versions, the exact DMI
vendor/product strings, the ACPI HID, the failing operation, and a redacted
log produced with libfprint debug logging disabled unless a maintainer asks for
specific additional fields.
