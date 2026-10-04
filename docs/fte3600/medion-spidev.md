# Medion E3224 spidev diagnostics

The `medion-spidev` branch provides an experimental, standalone diagnostic path
for the Medion E3224 wiring under investigation. It reuses this repository's
sensor identification, initialization and capture state machines. It does not
establish that the Medion sensor works, or that every laptop sold as an E3224
has the same sensor and wiring.

This is a board-specific tool, not the normal FTE3600 installation procedure.
The normal driver continues to require the ACPI resource bridge. The standalone
tool does not enumerate devices through fprintd or install a replacement system
libfprint.

## What is known about the Medion sensor

Tuxman2's [E3224 hardware report](https://github.com/SamSeven777/libfprint-fte3600/issues/1#issuecomment-5559055022)
identifies an SPI device with ACPI ID `FTE3600`, product version `FT` and board
`YS13G`. It does not contain a confirmed sensor model.

The [FocalTech repository's issue #9](https://github.com/focaltech-systems/drivers-input-touchscreen-FTS_driver/issues/9)
lists `FTE3600` and `2808:9338`, explicitly obtained from a Windows INF.
[Tuxman2's reply](https://github.com/focaltech-systems/drivers-input-touchscreen-FTS_driver/issues/9#issuecomment-2726462940)
says his laptop has the same reader, but supplies no chip-register response.
In the audited Windows package 2.0.3.102, `ACPI\FTE3600` selects the SPI install
section and `USB\VID_2808&PID_9338` selects the USB install section. These are
separate device matches in one package, not evidence that every FTE3600 SPI
device contains FT9338 silicon. The diagnostic therefore does not force an
FT9338 backend based on that issue title or the USB product ID.

Linux Hardware's [published index](https://github.com/linuxhw/TestDays/blob/main/Location/Belgium/README.md)
contains E3224 probe [`9def2aed31`](https://linux-hardware.org/?probe=9def2aed31).
Its separate [E3224 DMI record](https://github.com/linuxhw/DMI/blob/master/Convertible/Medion/E/E3224/4DDCFDF3E5EE)
records the Akoya family, YS13G board and SKU `30033784`. These establish a
matching computer model, not a fingerprint chip identity or ownership by
Tuxman2. The original probe's device logs could not be retrieved during this
investigation.

Medion's [official driver download 18518](https://www1.medion.de/downloads/index.pl?id=18518&lang=uk&op=detail&type=treiber)
lists `drve322x_w10.exe`, dated 2018-07-24, for E3221 through E3228. Its archive
could not be downloaded during this check, so no claim about its fingerprint
driver contents follows from the download page.

## Requirements and build

Use the `medion-spidev` branch and run commands from the repository root. The
tool requires the distribution's `spidev` driver and the
[GPIO character-device v2 interface](https://docs.kernel.org/userspace-api/gpio/chardev.html),
available since Linux 5.10. The GPIO interface is used directly; libgpiod is not
required.

For Fedora, the compiler and library dependencies follow the existing
[build guide](install.md#1-prerequisites--dependencies):

```sh
sudo dnf install git gcc gcc-c++ meson ninja-build pkgconf-pkg-config \
  glib2-devel libgusb-devel libgudev-devel systemd-devel \
  systemd cairo-devel python3
```

Build the standalone target:

```sh
meson setup build-medion -Ddrivers=fte3600 \
  -Dfte3600_medion_spidev=true -Dfte3600_personal_auth=false \
  -Dintrospection=false -Ddoc=false -Dgtk-examples=false --prefix=/usr
ninja -C build-medion examples/fte3600-medion
```

Do not run `ninja install` or `setup-fte3600.sh` for this diagnostic path. The
`--prefix=/usr` option does not install anything when building this target.
No custom kernel module, DKMS setup, module signing or service installation is
needed. Keep Secure Boot and SELinux enabled; this tool does not change either
policy. Distribution module loading and access to SPI/GPIO nodes must still be
permitted by the running system.

## Inspect before operating the sensor

The launcher is `scripts/medion-spidev.py`. Its default stage, `--inspect`, only
checks the host configuration. It must find an unambiguous ACPI association
between the FTE3600 SPI device and the expected GPIO controllers. A matching
ACPI name alone is not a sensor identity. This checks the known board's ACPI
namespace paths and hardware IDs; it does not parse or validate the running
firmware's `_CRS` resource descriptors.

The board configuration is deliberately fixed:

| Resource | Configuration |
| --- | --- |
| SPI | Existing mode 0 and active-low chip select; at most 1 MHz, preserving an existing lower speed limit |
| Reset | ACPI controller `GPO1`, line offset 39; physical high/low/high |
| Finger interrupt | ACPI controller `GPO2`, line offset 0; rising edge |

These are controller-relative line offsets, not global GPIO numbers. The
launcher resolves the controllers from their ACPI associations instead of
assuming `/dev/gpiochip0` and `/dev/gpiochip1`. It refuses ambiguous or missing
resources, and a SPI controller with another attached device. It does not switch
chip-select polarity: a device initially configured with active-high CS is
rejected. This configuration is an explicit Medion experiment, not a claim
that these offsets apply to other FTE3600 devices.

Only an unbound SPI device or one already using `spidev` is eligible. A device
owned by the custom `fte3600` bridge or another driver is left alone. The
launcher may load the distribution's `spidev` module with a 32768-byte buffer
request; it does not unload an already loaded module to enlarge a smaller
buffer. An existing buffer of at least 64 bytes allows execution to proceed,
with its actual capacity reported. The backend checks each chip's transaction
requirements and may reject an identified chip when the buffer is too small;
protocol frames are not split to make them fit. A temporary
binding and driver override are restored on normal completion and handled
failures; a module loaded for the experiment remains loaded.

## Stages and side effects

| Stage | What it establishes | Sensor operations |
| --- | --- | --- |
| `--inspect` | Reports the known ACPI associations, candidate nodes and current driver binding | None |
| `--probe` | Whether the existing protocol discovery can establish a supported identity | SPI queries, wake commands and, when needed, reset/ROM negotiation |
| `--init` | Whether that identified chip completes its existing initialization and cleanup | Probe plus chip initialization; eligible recovery may load matching firmware |
| `--capture OUTPUT` | Whether initialization and a finger-triggered acquisition produce an image | Initialization, capture and cleanup; writes the requested local PGM |

Run each stage separately, starting with host inspection:

```sh
python3 scripts/medion-spidev.py --inspect
sudo python3 scripts/medion-spidev.py --probe
sudo python3 scripts/medion-spidev.py --init
sudo python3 scripts/medion-spidev.py --capture "$PWD/medion-capture.pgm"
```

The launcher uses `build-medion/examples/fte3600-medion` by default. Use
`--tool /absolute/path/to/examples/fte3600-medion` only when using a different
build directory. Do not advance after an earlier stage fails: keep its exact
error and determine whether it came from host access, identification or sensor
initialization.

Inspection needs no root privileges. Operating the sensor requires root. The
diagnostic requests cancellation after 60 seconds per protocol stage; Ctrl+C
also requests cancellation and cleanup. A running kernel SPI ioctl must still
complete before its resources can be released, so this is not a guarantee that
every stalled controller returns within 60 seconds.
If the launcher must forcibly kill an unresponsive diagnostic after an
interrupt, the diagnostic cannot run its SPI/GPIO cleanup. That outcome is
reported as a failure; restoration of SPI parameters and reset state is then
not guaranteed.

`--probe` is **not read-only hardware inspection**. Discovery includes the
bounded wake and ROM negotiation paths documented in
[dynamic discovery](dynamic-discovery.md) and [special probing](special-probe.md).
It does not upload firmware. Initialization can use firmware only after the
existing positive identity checks authorize the specific recovery path; an
all-zero response never selects an FT9361 fallback.
The standalone diagnostic clears an inherited `FTE3600_FT9368_UPDATE` setting,
so this tool does not opt into the separate FT9368 persistent-flash update path.

Operational stages temporarily mask and stop fprintd to prevent concurrent
access. On normal completion and handled failures, the launcher restores its
prior state: it removes only a runtime mask that it added and restarts the
service only if it was previously active.
It does not install a permanent service override or change authentication
configuration. Restoration failures must be reported as failures, not hidden
behind a successful probe or capture result.

The sensor should be uncovered before initialization. In particular, the
FW9369 backend measures an uncovered baseline during initialization; placing a
finger on the sensor at that point can spoil calibration. Touch the sensor
when capture is waiting for a finger.

## Reading results

Keep host-access failures separate from sensor responses. Failure to load the
distribution module, claim a GPIO line, configure SPI or open a node is not an
unsupported-chip result. Likewise, successful host inspection does not show
that the sensor is powered, reset correctly or answering SPI commands.

Reports should identify the stage, the confirmed identity and the error, without
printing fingerprint pixels, feature descriptors or templates. A requested PGM
is biometric data: the tool writes it only to an explicit local output file with
mode `0600`. Do not add that image or image-bearing SPI dumps to this repository
or attach them to an issue. The output path must not already exist, and a failed
save is removed when cleanup can do so. With the commands above, the file is
owned by root.

The name **FW9369** denotes the audited Windows backend/profile. Its confirmed
raw silicon response is **`0x9362`**; a report must preserve both facts rather
than rename a raw `9362` response to `9369`. Other legacy responses, such as the
application's width/height register pair, describe runtime geometry and are not
automatically a silicon product number. See the
[hardware inventory](windows-hardware-inventory.md) and
[FW9369 protocol](fw9369-protocol.md).

A successful `--probe` does not prove initialization, IRQ delivery or a complete
image transfer. The `IDENTITY` line can precede a failed probe: backend selection
also checks whether a complete transaction fits the transport. Read the stage
result and its error as well as the identity. A successful capture does not
validate enrollment, matching or
population FAR/FRR. Authentication is disabled in the build above. Record the
source commit, kernel, stage and sanitized result when reporting this Medion
experiment; no Medion hardware success is claimed by the existence of this
branch or its mock tests.
