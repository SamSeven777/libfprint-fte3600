# Special-family cold identification

[Documentation index](README.md)

This specification records observable protocol facts from
`ftWbioUmdfDriverV2.dll` version 2.0.3.102, SHA-256
`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`.
It contains no firmware, register tables, or vendor implementation. The Linux
state machine is an independent implementation of the transactions below.

## Why read-only identification is insufficient

The Windows factory identifies the special family before it has chosen the
FW9369 or FT93xx backend. That factory path writes SFR `c6 = 01` and checks its
readback **before** reading the chip ID. A read-only `1a8b` request therefore
does not reproduce the established cold identification path.

The helper is labelled `config_spi_mode` in the reference diagnostics. This is
a chip-register setting; the inspected call chain does not configure host SPI
clock phase, clock polarity, or chip-select polarity. The evidence does not
establish C6's reset value, bit meanings, persistence across power loss, or
whether every cold device requires this write. Calling this register read-only
or claiming that an absent ID rules out the family would be incorrect.

## Reference factory ordering

| RVA | Fact |
| --- | --- |
| `0x2411c–0x24123` | After the separate FT9368 probe fails, the SPI factory calls the wake helper (`0xfba0` with argument zero), then shared ID helper `0x10a74`. |
| `0x10a74–0x10afd` | Shared ID helper configures C6 to one through `0xfd94`, then calls `0x10d70`. It does not clear IRQs or change pad voltage. |
| `0x10d70` | Configures C6 to one again, then reads word `1a8b` using `0x183b0`. |
| `0xfd94–0xfecf` | Writes C6, waits 4 ms, reads C6 and compares the requested value; up to 31 attempts. It does not save or restore the previous value. |
| `0x2414f–0x24185` | Factory accepts IDs `9362`, `9365`, `9391`, and `9392`. |
| `0x2418b–0x241ae` | On mismatch, calls the transport hardware-reset operation, waits another 10 ms, and repeats wake plus shared ID helper. |
| `0x241da–0x24216` | After the repeated special ID also fails, proceeds to the legacy detection branches. It does not restore a saved C6 value. |
| `0x24620–0x2465e` | After the factory already selected FT93xx from a positive ID, constructs that backend, resets it, and calls `0x196b8`. |
| `0x196b8 → 0x19978 → 0x1aa7c` | FT93xx backend initialization calls its own probe, which selects the chip's 1.8 V pad setting before its internal ID revalidation. This is **after family selection**, not part of the unknown-family factory probe. |

The backend-specific FW9369 probe at `0x10088` must not substitute for the
shared factory helper: it additionally clears all IRQ flags (`1a84 = ffff`) even
when the eventual ID is not `9362`.

## Wire transactions

All lengths below include command bytes and receive clocks. Multibyte payload
words use big-endian byte order.

| Operation | TX bytes / length | Result |
| --- | --- | --- |
| Wake | `5a a5 00`, 3 bytes | Wait 1 ms. |
| Read state | `08 f7 80 00 00`, 5 bytes | Byte at offset 4. If it is not `50`, send idle below. |
| Idle | `c0 3f 00`, 3 bytes | Wait 1 ms. |
| Finish wake | `a5 5a 00`, 3 bytes | No additional delay in the reference helper. |
| Configure C6 | `09 f6 c6 01`, **4 bytes** | Wait 4 ms; there is no trailing dummy byte in this write. |
| Read C6 | `08 f7 c6 00 00`, 5 bytes | Byte at offset 4 must equal `01`. |
| Shared factory ID | `04 fb 9a 8b 00 01` followed by 6 zero bytes, 12 bytes total | First two response bytes at offset 6 form the raw chip ID. |
| FT9391 variant | `04 fb 98 16 00 00` followed by 4 zero bytes, 10 bytes total | Value at offset 6; trailer at offset 8 follows the FT93xx register CRC convention. |

Wake ordering is established by `0xfba0 → 0x10f0c`; SFR packet formats by
`0x1830c` and `0x18368`; shared ID framing by `0x183b0`. The FT93xx variant
rule is established by `0x19978`: ID `9391` with word `1816 == 0fff` is the
separate `9395` variant. Its register framing and CRC rules are specified in
[ft93xx-protocol.md](ft93xx-protocol.md).

For completeness, the post-selection pad operation `0x1aa7c` writes
`FD = 0A`, then `FE = 7F`, waits 1 ms, and verifies FE, up to five attempts
for its argument-one/1.8 V branch. Its argument-zero branch instead selects
the reference-labelled 1.2 V setting and uses a different delay. Those writes
are excluded from unknown-family negotiation; restoring an arbitrary saved
value would also require electrical semantics not established here.

## Linux policy and cleanup boundary

The fallback runs only after the non-configuring family probes have failed.
The caller selects one supported CS polarity and owns whether another
polarity should be tried. This child never changes CS.

1. Wake using the sequence above; write C6 and require its readback to be one.
   C6 readback failure permits at most four write/wait/read attempts. This is
   a Linux bound matching the FT93xx helper's count, not the larger FW9369
   helper count.
2. Read ID twice on the same CS and require exact agreement. A mismatch is a
   protocol error, including a positive ID followed by an empty response.
   Only the four IDs selected by the shared reference factory can select a backend.
3. For `9391`, additionally read and validate the variant twice. `0fff` is
   reported as known but unsupported `9395`, never as FT9769. Invalid CRC or
   inconsistent observations terminate discovery after reset cleanup. They
   must not be returned as an unidentified device that permits legacy ROM probing.
4. On success, leave the chip awake with C6 set to one and return the positive
   identity for backend initialization. No firmware, pad, IRQ-mask, or image
   calibration operation occurs here.
5. On no match, I/O failure, or cancellation after the first submitted wake,
   perform uncancellable GPIO cleanup: controller-level H for at least 10 ms,
   L for at least 20 ms, then H and a 160 ms Linux recovery wait. The reset
   restores a startup boundary, not the prior application's transient state.
   Its GPIO interpretation is specified in [gpio-polarity.md](gpio-polarity.md).

Cleanup always attempts final deassertion even if an earlier GPIO operation
fails. The first operation error is preserved. A negative result is returned
without an error **only if cleanup succeeded**; I/O, cancellation and cleanup
errors, inconsistent IDs and variant validation failures must stop the caller's
discovery sequence. A known unsupported ID is
preserved as diagnostic evidence so the caller cannot reinterpret it as a
blank device and enter legacy firmware recovery. Cancellation before any
transaction and an insufficient transfer limit perform no GPIO reset.

The independent asynchronous mock tests model a device whose ID is unavailable
until wake and C6 negotiation succeed. They exercise every supported ID,
variant and CRC rejection, changing IDs, bounded C6 retries, every transfer
failure/cancellation boundary, GPIO failures and minimum reset delays. This
validates software sequencing, not cold-start success or voltage levels on
physical boards.
