# FTE3600 ACPI reset and IRQ glue

See [installation](../../docs/fte3600/install.md) for system setup. This module
pairs distribution spidev with a reset GPIO and an IRQ-only UIO device. It
performs no SPI transfers and does not replace the SPI controller.

## Resources

The ACPI device must provide one consumer eight-bit, four-wire SPI connection,
one single-pin output-capable reset GpioIo, and one edge-triggered interrupt
(GpioInt or IRQ/Interrupt). Reset accepts default, pull-up or disabled bias.
Named GPIO properties must agree with the resource mapping. Wiring comes from
ACPI, not laptop model names or fixed physical pin numbers.

```text
SPI device (spidev, FTE3600 ACPI identity)
  spidev class device
  fte3600-glue platform device
    one-line GPIO character device
    UIO device: name=fte3600-irq, version=2
```

Validate common ancestry and actual character-device identities; dynamic node
numbers do not establish pairing. The UIO device exposes no memory mappings.

## Metadata ABI 2

Read these attributes on the platform glue device:

| Attribute | Value |
| --- | --- |
| `fte3600_glue_abi` | `2` |
| `fte3600_ngpio` | `1` |
| `fte3600_generation` | Session generation |
| `fte3600_status` | `ready`, `suspended` or `removed` |
| `fte3600_acpi_mode` | ACPI CPHA/CPOL/CS_HIGH bits, decimal |
| `fte3600_cs_control` | Whether native CS polarity may be changed |
| `fte3600_acpi_speed_hz` | ACPI SPI speed limit |
| `fte3600_irq_active_low` | Falling edge=1, rising edge=0 |
| `fte3600_irq_source` | `gpio` or `acpi` |

The only GPIO is offset 0, named `reset`, exposing physical levels. Request it
as OUTPUT | ACTIVE_LOW with initial logical zero: zero releases physical high,
one asserts physical low. Do not apply a second inversion.

## Session

Effective UID zero is required. Claim reset first, then open UIO with
`O_RDONLY | O_NONBLOCK`. UIO open exclusively requests the interrupt; a read
of exactly four bytes returns the native-endian cumulative event count.
There is no write-to-enable protocol. Multiple edges may coalesce.

Close UIO before reset. Closing reset first invalidates the session and wakes
UIO; close both descriptors before reopening. Check status and generation
before and after SPI operations and during IRQ waits. Suspend/removal invalidates
leases; old descriptors cannot be reused after resume. A wake caused by session
loss is not a sensor event. Stock spidev does not enforce the glue's lease on
other root processes or provide atomic power-transition exclusion.

Only change CS polarity when `fte3600_cs_control=1`; otherwise retain the
controller-managed mode. Restore the ACPI mode on normal close and the next
controlled open. Abnormal process exit cannot guarantee immediate restoration.

## Build

Requires ACPI, GPIO character devices, UIO and matching kernel headers:

```sh
make KDIR=/lib/modules/"$(uname -r)"/build W=1
make check
```

The old private SPI bridge ABI is not supported; see
[migration](../../docs/fte3600/install.md#migrate-an-earlier-bridge-installation).
