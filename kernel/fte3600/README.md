# FTE3600 ACPI reset and IRQ glue

[Documentation index](../../docs/fte3600/README.md)

The current transport uses the unmodified Linux `spidev` driver for the physical SPI
device. Module `fte3600` creates a `fte3600-glue` platform child only while that
device is bound to `spidev` and has the FTE3600 ACPI identity. The platform driver
owns the ACPI reset/IRQ resources and exports a standard, one-line GPIO character
device plus an IRQ-only UIO device. It does not bind another driver to the SPI device, create a proxy SPI
controller, execute SPI messages, or change SPI mode.

The old private `/dev/fte3600-*` ioctl interface is not provided. Documents
describing that bridge concern the earlier implementation. Existing
installations need matching userspace and setup files; follow the
[migration guide](../../docs/fte3600/install.md#migrate-an-earlier-bridge-installation).

## Resources and device pairing

The supported resource shape is exactly one consumer, eight-bit, four-wire SPI
connection with a nonzero speed; one single-pin reset `GpioIo`; and exactly one
edge-sensitive interrupt, either single-pin `GpioInt` or an ordinary ACPI
`IRQ`/`Interrupt` (`EXTENDED_IRQ`) resource with one interrupt. Its polarity must
be active-high or active-low. GPIO resources may appear in either order and may
refer to different controllers. An ordinary interrupt does not consume a GPIO
resource index: it uses the IRQ already resolved by SPI core, while `GpioInt`
uses its real descriptor's IRQ. No DMI names, controller paths or physical pin
numbers are hard-coded.

Reset must allow output and use default, pull-up or disabled bias. Pull-down and
unknown bias are rejected before descriptor acquisition, because ACPI's
OutputOnly configuration can drive an initial level even with `GPIOD_ASIS`.
Named `_DSD` GPIO properties, if present, must reference the same validated
resources and agree with the reset polarity. This is a deliberate support
boundary, not proof that every device with FTE3600 ACPI identity meets it.

The sysfs ancestry is:

```text
physical SPI device (driver: spidev, firmware_node: FTE3600 ACPI device)
  spidev class device (/dev/spidevB.C)
  fte3600-glue platform device (driver: fte3600-glue)
    gpiochipN bus device (/dev/gpiochipN)
    uio class device (/dev/uioN, name: fte3600-irq, version: 2)
```

Match the GPIO device's actual parent to the glue and its parent to the exact
physical SPI device. Do not infer pairing from dynamic device numbers. The GPIO
cdev is a bus device, so its canonical sysfs parent is the glue; a `device`
symlink or the legacy GPIO sysfs interface is not required. Validate actual
character-device major/minor identities as well as this ancestry. Match the UIO
device's parent to that same glue. The UIO device provides no memory mappings or
port regions; its only purpose is IRQ notification.

After publishing metadata, the driver emits change events for the glue and
its GPIO/UIO children and for the SPI device's children. This permits udev to
retry pairing when the original add event preceded metadata publication.

## Metadata ABI 2

All these read-only attributes belong to the **platform glue device**:

| Attribute | Meaning |
| --- | --- |
| `fte3600_glue_abi` | `2` |
| `fte3600_ngpio` | `1`; also verify with `GPIO_GET_CHIPINFO_IOCTL` |
| `fte3600_generation` | Unsigned decimal session generation, initially zero |
| `fte3600_status` | `ready` or `suspended`; `removed` can appear during teardown |
| `fte3600_acpi_mode` | Decimal SPI CPHA/CPOL/CS_HIGH bits from the original ACPI resource |
| `fte3600_cs_control` | Required `1` only for native CS with controller `SPI_CS_HIGH` support; `0` means retain controller-managed CS |
| `fte3600_acpi_speed_hz` | Original ACPI SPI maximum speed, in hertz |
| `fte3600_irq_active_low` | `1` for falling-edge IRQ, `0` for rising-edge IRQ |
| `fte3600_irq_source` | `gpio` for GpioInt, `acpi` for an ordinary ACPI IRQ |

The only GPIO offset is 0, named `reset`. The subset exposes **physical
raw levels**. Userspace requests reset as `OUTPUT | ACTIVE_LOW` with initial
logical value zero: logical zero releases reset to physical high, logical one
asserts physical low. The underlying descriptor also has active-low metadata,
so the forwarding callbacks use raw operations to avoid a second inversion.
No debounce or alternative electrical configuration is part of this contract.
An ordinary interrupt is never represented as a GPIO or assigned a fabricated
GPIO value.

Requests require effective UID zero. Claim reset first to acquire the exclusive
cooperative session, then open UIO with `O_RDONLY | O_NONBLOCK`. The UIO open is
exclusive and requests the physical IRQ; no IRQ handler is installed while
closed. No `write(1)` enable/re-enable protocol is needed or supported. A read
must request exactly four bytes and returns the standard native-endian 32-bit
UIO cumulative event count. Multiple edges may coalesce into one readable
counter update. The IRQ is requested without `IRQF_SHARED`: an occupied line
fails rather than accepting interrupts that this sensor cannot identify without
SPI I/O.

Normally close UIO before reset. If reset closes first, the glue frees the IRQ,
advances the session generation and wakes a surviving UIO reader; its remaining
lease blocks a new reset session until UIO closes. This wakeup reports session
loss, not a sensor event. Other root processes must still respect the session:
stock spidev itself does not consult these leases.

## Power transitions and release

A system PM prepare notifier runs before device suspension, advances the
generation, invalidates existing leases, frees the physical IRQ, wakes UIO readers
and attempts to deassert reset. A post
notifier runs after providers resume and deasserts reset before reporting ready.
Failure leaves the glue unavailable. Old GPIO requests remain invalid even
after resume; userspace must close and open a new session. The IRQ is not
automatically requested on resume for an invalidated UIO fd. GPIO setters before
Linux 6.17 cannot return errors, so metadata checks are mandatory.

Check status and generation before **and after** each SPI operation and during
IRQ polling, and before interpreting any UIO notification. Reject results from
a changed or missing instance. This catches
session loss; it does not create atomic PM revocation inside stock spidev or
prevent a concurrent root process from directly sending SPI messages.

Normal reset-line release attempts physical high while providers are awake.
Unbinding first removes metadata, rejects new requests, frees the IRQ and releases reset. It
unregisters UIO, so old UIO fds see EIO/HUP. It
retains the GPIO chip and underlying descriptors until outstanding line fds
close. Userspace must react to missing metadata; a retained line fd is not
guaranteed to receive immediate GPIO-cdev HUP. Rebinding while old line requests
still own the resources can fail with EBUSY and requires a retry after closing
them.

The reset lifetime follows Linux 6.8's actual GPIO cdev ordering:
[`gpiod_free_commit()`](https://github.com/torvalds/linux/blob/v6.8/drivers/gpio/gpiolib.c#L2305)
clears the requested flag after that callback. Deferred cleanup waits for that
commit before removing the chip. The cdev ioctl read lock and
[`gpiochip_remove()`](https://github.com/torvalds/linux/blob/v6.8/drivers/gpio/gpiolib.c#L1044)
write lock serialize outstanding chip queries and failed requests. A dedicated
workqueue is drained before module exit; platform and ACPI references remain
held through cleanup.

UIO uses `UIO_IRQ_CUSTOM` so its open/release callbacks control the physical
IRQ lifetime. Linux 6.8's
[`uio_unregister_device()`](https://github.com/torvalds/linux/blob/v6.8/drivers/uio/uio.c)
clears the callback pointer under `info_lock`; an old fd's subsequent close does
not call the driver's release callback. The glue therefore frees its IRQ before
unregistering and retains the registration reference until unregister has
synchronized all open/release callbacks. UIO opens do not own private references
that depend on a post-unregister release callback. Unregister runs outside the
glue mutex to respect UIO's callback locking order. The GPIO IRQ provider's
normal IRQ resource hooks still control its underlying IRQ lock.

Userspace uses both-CS identification only when `fte3600_cs_control=1`, and
restores the original ACPI mode on normal close and on the next configure.
The attribute is derived from the actual SPI device's CS descriptor and the
controller's mode support, not from the number of child ACPI GPIO resources.
For GPIO CS or a controller without polarity control, userspace does not write
SPI mode at all; it retains the existing effective polarity and can still run
single-polarity identification and the bounded legacy `0x70` wake sequence.
Linux 6.8 [`spidev_ioctl()`](https://github.com/torvalds/linux/blob/v6.8/drivers/spi/spidev.c#L369)
masks GPIO `CS_HIGH` on mode reads and forces it on mode writes. Its word-size
and speed setters call [`spi_setup()`](https://github.com/torvalds/linux/blob/v6.8/drivers/spi/spi.c#L3667)
without replacing that mode; setup still invokes controller configuration and
CS deselection. Userspace restores those two settings while leaving fixed CS
untouched. A reported mode bit is not a GPIO-level measurement.

SIGKILL can leave a native-CS trial polarity until the next configure; fixed-CS
sessions cannot restore an independently changed hidden polarity. The glue
intentionally never changes `spi->mode`.
Opposite-polarity trials on a shared controller retain the electrical risk of
selecting the sensor during another device's traffic. The implementation does not
claim bus-wide isolation, measured reset waveforms, or suspend validation on
real hardware.

## Building and validation

```sh
make check
make KDIR=/lib/modules/"$(uname -r)"/build W=1
```

The host tests exercise the production resource, polarity and session policy,
including alternate resource ordering, duplicate/missing resources, stale
generations and both fd-close orders. They do not emulate the GPIO core, IRQ
delivery, driver unbinding or actual suspend. The module has been compiled with
Linux 6.8.0-146 headers and `W=1`; it has not been loaded as part of development.
The platform remove callback and GPIO setter signatures have compatibility
guards for later kernels, but newer-kernel compilation and hardware tests are
still required. In particular, no claim of upstream acceptance follows from
using standard spidev/GPIO/UIO interfaces. Building also requires `CONFIG_UIO`
along with ACPI and the GPIO character interface. Closed-IRQ suppression, IRQ
triggering, removal with open fds and suspend remain to be tested on hardware.
