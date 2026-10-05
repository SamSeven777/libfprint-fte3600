# Image resolution metadata

[Documentation index](README.md) · [Architecture](architecture.md)

The sensor catalog records image resolution in pixels per millimeter (`ppmm`).
This describes the image's physical scale; pixel width and height alone do not
establish it. The conversion is `ppmm = dpi / 25.4`. A value of `0` means unknown.

## Catalog values and evidence

| Driver profile | Native image, pixels | Resolution, dpi | Catalog ppmm | Evidence |
| --- | --- | --- | --- | --- |
| FT9338 | 88 × 88 | 508 | 20 | Manufacturer resolution agrees with the published 4.4 × 4.4 mm sensor area |
| FT9348 | 96 × 96 | 508 | 20 | Manufacturer resolution agrees with the published 4.8 × 4.8 mm sensor area |
| FT9361 | 64 × 80 | 508 configured | 20 | Retained existing A1 profile value; physical pitch not independently confirmed by this review |
| FT9536 | 64 × 128 | 508 | 20 | Manufacturer resolution agrees with the published 3.2 × 6.4 mm sensor area |
| FT9365 | 64 × 80 | 552 nominal | `552.0 / 25.4` | Current Simplified Chinese manufacturer product table |
| FT9368 | 64 × 80 | Unknown | 0 | No model-specific physical resolution established |
| FT9369 / FW9369 | 64 × 80 | Unknown | 0 | No model-specific physical resolution established |
| FT9769 | 40 × 196 | 564 nominal | `564.0 / 25.4` | Current Simplified Chinese manufacturer product table |

The native image dimensions are the existing driver profiles. The sources below
establish the resolution metadata to the stated extent; they do not validate
every chip revision, OEM module, or Linux capture path.

## Manufacturer sources

The [November 27, 2018 Chinese product-page archive](https://web.archive.org/web/20181127080945id_/http://www.focaltech-systems.com:80/chipfinger/drsCase.html)
and [English-path archive from the same day](https://web.archive.org/web/20181127072645id_/http://www.focaltech-systems.com:80/en/chipfinger/drsCase.html)
list FT9338, FT9348 and FT9536 with sensor matrix, sensor area and resolution.
The resolution column contains `508`; the separate matrix and sensor-area
columns independently give 20 pixels/mm, equivalent to 508 dpi. These are
sensor-area dimensions, not the larger cutting or package dimensions in the
adjacent column. This is a cross-check of the manufacturer's specifications,
not a physical measurement of a tested device.

The [current Simplified Chinese FocalTech product page](https://www.focaltech-electronics.com/zh-CN/product/index/fingerprint),
checked on October 5, 2026, lists FT9365 at 552 dpi and FT9769 at 564 dpi. The
catalog adopts these nominal manufacturer values. The
[English product page](https://www.focaltech-electronics.com/en-global/product/index/fingerprint)
lists FT9365 at 554 dpi instead; the Simplified Chinese entry is the selected
reference for this implementation. Its package dimensions do not establish
active sensor area, and the nominal values have not been cross-checked against
pixel pitch for the particular SPI variants supported here.

FT9361 retains the existing 20 pixels/mm setting used by the maintainer's A1
profile. The reported working hardware path is preserved; this research did
not independently establish its physical pixel pitch. It is not a default for
other chips. FT9368 and FT9369 remain unknown because no model-specific pitch
or active-area evidence was established for them. A generic image-format
resolution or another model's specification is insufficient to fill these
entries.

## Effect on capture and matching

This change corrects image metadata: consumers of `FpImage.ppmm` receive the
catalog value, including `0` when resolution is unknown or unset. Native capture
dimensions, SPI framing, firmware selection and pixel values remain unchanged.
The current BRISK and 2D-IPA paths do not use `ppmm`: BRISK receives each
sensor's native image geometry, while the current IPA adapter remains restricted to FT9361's
64 × 80 input. Changing the metadata does not retune their features, thresholds
or stored templates. See [matcher architecture](matcher-architectures.md).
