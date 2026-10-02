# Provenance and fidelity boundaries

Recorded 2026-10-02. This document distinguishes observed history, archived
artifacts and this new diagnostic port. None is a new Medion hardware result.

## Historical baseline

- [tuxman2's Mint success and confirmation that this was the same Medion](https://github.com/SamSeven777/libfprint-fte3600/issues/1#issuecomment-5830566555).
- [Original successful ctfdavis-related report](https://github.com/vobademi/FTEXX00-Ubuntu/issues/1#issuecomment-3288100306).
- [Later proposed CS patch](https://github.com/vobademi/FTEXX00-Ubuntu/issues/1#issuecomment-3402710422).
  Do not relabel that later proposal as the proven successful version.
- [Our last reported real-machine test](https://github.com/SamSeven777/libfprint-fte3600/issues/1#issuecomment-5928567443)
  used `72737b0` / `--compare-soft-reset`, not this tool or a full archived stack.

The publicly archived files below are reproducible candidates from that lineage.
We do **not** have hashes of every file installed during the successful Mint
session. Thus this is an archived-stack reproduction attempt, not proof of
byte-identical reconstruction of that installation.

## Fixed inputs

Kernel reference: [`alt/focal_spi.c` at a0a35d3e47873a1712e02e3210f0e6e4fe75eeee](https://github.com/vobademi/FTEXX00-Ubuntu/blob/a0a35d3e47873a1712e02e3210f0e6e4fe75eeee/alt/focal_spi.c).
The complete source is retained unmodified in `reference/ctfdavis-focal_spi.c`,
including its copyright and GPL notice.

```text
Bytes: 15757
SHA256: a08ecd4e025c0d19272650960895cec91f9ce6b456771bf9a8e31314a76bc7ce
```

An earlier local analysis copy had one extra trailing newline, length 15758 and
SHA256 `fdcf6e583291ab719007896b38742a0ff2cfbdc26e5f9de10d22959932902b2c`.
The function bodies are identical; this branch pins the actual raw GitHub file.

Userspace reference: [`libfprint-2-2_1.94.4+tod1-0ubuntu1~22.04.2_spi_20250112_amd64.deb` at d534b7a3759a1e338f8d1b866db222d0e1674025](https://github.com/oneXfive/ubuntu_spi/blob/d534b7a3759a1e338f8d1b866db222d0e1674025/libfprint-2-2_1.94.4%2Btod1-0ubuntu1~22.04.2_spi_20250112_amd64.deb).

```text
Package bytes: 806394
Package SHA256: b48c93c3732f90aabbcc520e5538faeffbb87bb6847a01d03e14ea157f1d36c1
Member: ./usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0 (in data.tar.zst)
Library bytes: 2154176
Library SHA256: 4ee33eb988a62413698d1761b9c9baf09faca6a645f82ac19db6f33d04c8cb1b
```

This is a **library**, not the 10,396-byte `ft9361.bin` firmware image. The
filename is not proof of distribution compatibility: the ELF requires
`GLIBC_2.38`, among other versions. The ABI-only sandbox check is mandatory
before binding the device. No Ubuntu package installation is used on Fedora.

## Preserved behavior

At build time `prepare.py` checks the full reference hash and extracts six
complete function bodies into a generated include:

- `focal_spi_get_gpio_config`: optional `reset`, then `power`, then `enable`
  lookup using `GPIOD_OUT_LOW`. NULL means no manually selected GPIO. Errors,
  including probe defer, stop rather than trigger speculative pin access.
- `focal_spi_configure_spi`: Mode 0, 8 bits, maximum speed capped at 1 MHz;
  an existing lower nonzero speed is retained.
- `focal_spi_hw_reset`: with a descriptor, logical 0, 5–6 ms, logical 1,
  5–6 ms, logical 0, 50 ms. No descriptor means no helper delay/write.
- `focal_spi_reset`: logical 0, 10 ms, logical 1. The delay remains even with
  no descriptor. This is what the archived reset ioctl calls.
- `focal_spi_power_off` and `focal_spi_power_on`: logical 0 and 1 on the same
  descriptor. Their names alone do not prove a board-level power rail action.

`gpiod` logical values are not voltage levels. An active-low descriptor reverses
the requested physical level. The two archived reset functions also end at
different logical values; the port deliberately does not silently 'correct'
one of them based on a conjectured polarity.

The bridge preserves the packed 5-byte header and A5/5A/B9 read/cache interface,
write payload after that header, ioctl numbers 0x8086–0x808c, and the archived
event-number polling interface. A command/response read remains one
`spi_write_then_read` message, not two separate transactions. Successful
syscalls return the supplied count as in the reference. CS ioctl remains a
no-op. IRQ request flags and initial disable are retained when an IRQ exists.

The client selects only `focaltech`, calls public `fp_device_open_sync`, then
`fp_device_close_sync`. It contains no hand-written C6/ID/firmware-upload
sequence. Static examination of this fixed library placed reset ioctl before
C6 setup and identity checks, with further initialization/calibration afterward;
the purpose of calling the whole library is not to truncate that later work.

The library's modified enumeration was also checked: `fp_context_enumerate`
queries the `misc` subsystem at ELF VA `0x13a4c`, then matches sysfs paths with
the driver ID-table substring at `0x13d1d`–`0x13d34`. The FocalTech class at
`0x39a00` points to the table at `0x1f2520`: `FTE3600`, `FTE4800`, `FTE6900`,
then the fallback `focal_moh_spi` entry at `0x1f2580`.
This adapter preserves that misc-device name. A synthetic input-key node is not
part of this enumeration path. The supervisor waits for udev and checks the
device node before starting the library; it does not assume that installing a
spidev rule is sufficient for the archived library.

## Deliberate differences in this diagnostic adapter

Modified 2026-10-02; `module/focal_medion_baseline.c` is new adapter code around
the original core, **not a claim that the original full module is unchanged**.

- A dedicated temporary driver/module name; exact DMI/device guard; root-only
  misc node; single-open restriction. No permanent module installation.
- GPIO calls are instrumented with bounded diagnostic content, and use the
  sleeping-capable setter in process context. Logical values/delays are not
  inverted. GPIO acquisition itself can set the initial output direction/value.
- Bounds checks reject impossible/truncated frames; overlapping copy uses
  `memmove`; SPI errors are returned instead of being overwritten by a later
  successful copy. Mutexes protect buffers and removal.
- Managed resources, module ownership on open descriptors, and restoration of
  prior SPI mode/speed/bits on removal or failed probe.
- No unused synthetic input-key device, vendor printf facility or system-sleep
  callbacks. Sleep is inhibited for this short test; no suspend/resume support
  is claimed. Poll's unusual legacy blocking/event-number ABI is retained for
  compatibility, not proposed as a new upstream design.
- fprintd is stopped/runtime-masked only for this test; an existing mask is
  preserved. Parent runtime-PM controls are temporarily set to `on` and restored.
  This is a controlled test condition, not proof that power management caused
  the original failure, or a reproduction of all Mint power policy settings.
- Library logs and calibration data are not dumped. Only C6 and ID replies
  (1 and 4 bytes) are eligible for raw-byte logging; transfer payloads are not.

The module's limits and stricter error handling may expose an archived client
assumption. If initialization fails, inspect the recorded stage/error before
changing protocol, polarity, firmware or pin selection. If identity works but
opening fails later, investigate that later initialization stage. If this
entire baseline works, compare a future independent driver against it before
trying enrollment. A claim of 'no power' still requires evidence beyond zeros.

## Windows comparison is supporting evidence, not a new sequence here

The 2021 and 2025 FocalTech Windows reference packages examined locally contain
the same 10,396-byte FT9361 firmware but different GPIO write sequences: 0/5 ms/1
versus 1/10 ms/0/20 ms/1. Both end at 1; this did not establish opposite polarity.
Neither package was authenticated as tuxman2's exact factory package. This port
does not mix either Windows sequence into the ctfdavis baseline, nor infer
FT9362 from a failed SPI read. Raw Windows connection writes and Linux logical
descriptor writes must not be conflated.
