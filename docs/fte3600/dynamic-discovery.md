# Dynamic ACPI and sensor discovery

This change removes the DMI model/controller/pin table. It independently
implements resource discovery and chip-specific runtime/ROM identity protocols.
It does not reproduce the Windows fallback that assumes a chip after failed
reads, or turn an unknown response into firmware-upload authorization.

## Two independent decisions

1. The `fte3600` Linux SPI module binds `ACPI\\FTE3600`. Linux resolves the SPI
   controller, chip select and ACPI connection parameters. The module walks the
   evaluated `_CRS`, requiring one SPI connection, one single-pin reset GpioIo
   and one single-pin edge GpioInt. It maps the reset by its GPIO resource
   index and obtains the interrupt through the ACPI GPIO API. The resources
   can point at different GPIO controllers. There is no DMI read or board pin
   constant. Ambiguous/missing resources and level/both-edge interrupts fail
   explicitly rather than silently selecting a pin.
2. libfprint opens `/dev/fte3600-*`, checks its ABI and transfer capabilities,
   and probes the sensor. The runtime pairs `5858`, `6060`, `4050`, `4080`
   identify FT9338, FT9348, FT9361 and FT9536 respectively. Further probes read
   word `1a8b` using the distinct FW9369 and FT93xx framing, or wake/query FT9368
   using `ff00`/`9180`. Each positive application identity must repeat unchanged.
   Raw `9391` also requires register `1816 != 0fff`; the 9395 variant is rejected.
   Empty/unrecognized responses try the alternate CS polarity when supported.
   Successful discovery keeps that polarity. Failure restores the original.
   If these probes fail, the shared factory wake and verified `c6=01` sequence
   retries identification on each supported CS polarity. This state-changing
   fallback requires repeated IDs and the FT9391 variant check; it never writes
   FD/FE pad-voltage controls before selecting a family. Negative results require
   successful reset cleanup. I/O or cleanup errors stop discovery; known
   unsupported IDs cannot fall through to legacy firmware recovery. See the
   [factory negotiation specification](special-probe.md).
   Only after these probes fail does an empty legacy response enter ROM
   discovery. A recognized A8 ROM family **and** the SPI OTP variant must agree
   on FT9348 or FT9361. FT9536 instead permits positive boot-A identification;
   boot-B recovery for FT9338/FT9536 requires current-session runtime identity
   plus matching SPI OTP. Every RAM upload requires its own ROM check.
   Unknown/unsupported results stop before firmware loading. There is no
   fallback to an assumed FT9361.

Runtime `0x14/0x15` values also describe sensor geometry in the Windows driver;
they are not claimed to be immutable silicon IDs. Geometry, application and AGC
versions are checked again after initialization. The existing FT9361 extractor
and authentication policy remain compatible; the sensor-aware template format
and additional profiles are described in [family authentication](family-authentication.md).

## Electrical and protocol limits

Windows sends physical GPIO-controller pin levels `1,0,1`. Linux uses an
active-low descriptor and logical reset values `0,1,0` to produce the same
high/low/high sequence. GpioIo has no polarity field. The fallback mapping is
active-low; an existing `reset-gpios`/`reset-gpio` property must refer to the
validated resource, pin zero, active-low, with exactly one entry. Contradictory
or ambiguous mappings fail. PullDown and unknown bias are rejected before GPIO
acquisition because Linux ACPI can override the initial output value. See
[GPIO polarity evidence](gpio-polarity.md) for the binary-to-API proof and the
limits of this guarantee. Interrupt polarity/trigger comes from GpioInt. Board
inverters, actual voltages and the single-reset convention still need hardware
validation.

The bridge starts with ACPI SPI mode/chip select, caps speed at 1 MHz and uses
8-bit words. Optional ABI 1 capability `CAP_CS_POLARITY` allows changing only
the physical active level of CS under the exclusive session lock. It does not
alter reset polarity, CPOL/CPHA, clock or word size. A failed `spi_setup` rolls
back; failure to restore invalidates the session. The backends accept mode 0.
Its maximum transaction
is the smaller of the controller transfer/message limits and 32,768 bytes.
Capture needs 7,752 / 9,224 / 5,128 / 8,200 bytes for
FT9338 / FT9348 / FT9361 / FT9536. Firmware recovery needs 10,319 bytes for
FT9348 or 10,403 bytes for FT9361 in one transfer. FT9338/FT9536 RAM readback
requires 14,192/11,942 bytes respectively.
FW9369 needs 10,246 bytes and FT9368 5,127 bytes in a single transaction.
FT9365/9769 use documented FIFO chunks with a maximum 1,798-byte transaction;
their total raw image size is not imposed as a single-transfer requirement.
One transaction is never split implicitly across chip selects.
The spidev module and its global bufsiz no longer participate in this driver.

The minimal cold identification sequence is specified by the static evidence
in [Windows runtime adaptation](windows-runtime-adaptation.md):

- `90 00 00` distinguishes boot-A (`ef` response). After the documented reset,
  sync and boot-register preparation, `fe=02` positively identifies FT9536.
  Other boot-A responses do not default to FT9338. Four-byte boot38 register
  framing is distinct from the five-byte A8 format.
- The other branch uses `06 f9 00`, a four-byte register query staged at mailbox
  `85c0`, a 2 ms settle, and the mailbox trigger/read to obtain the family.
- Accepted families are `2b50`, `95a8`, `23dd`. OTP is selected through the
  `c8/f1/f4/f3` register sequence. SPI low nibble `1..3` identifies FT9348;
  `4/e/f` identifies FT9361. USB's different OTP adjustment is not used.
- The OTP access bit is cleared and a bounded hardware reset returns the chip
  from discovery mode. The reset cleanup also runs on errors/cancellation.
- A positive FT9348/FT9361 ROM identity can reach that sensor's external
  firmware size/SHA-256 validation and upload. A cached runtime identity cannot
  authorize an upload. Firmware bytes remain outside the repo.
- Boot-B38 recovery requires runtime `5858`/`4080` in the current open and the
  corresponding OTP upper nibble `1`/`2`. A previous enumeration does not count.
  FT9338's first unidentified blank boot remains unsupported. The independent
  [legacy38 recovery](legacy38-recovery.md) compares a complete RAM readback
  before restarting; its four-byte boot framing is not interchangeable with A8.

FT9338-family boot-B is entered only after a runtime geometry was positively
observed in this open, with matching SPI OTP revalidation. An unknown A8 family
alone is not evidence of boot-B38. This deliberately leaves first-time blank
FT9338 discovery unsupported, while allowing proven current-session recovery.
FT9338/FT9536 require 14,192/11,942-byte complete readback transactions and their
own externally supplied, exact hash-validated firmware.

Wire encoding was checked at DLL RVAs `22c78`, `2f1b0`, `2fc30`, `2fab0`,
`2f4c0`, with identity flow at `28114`, `2819c`, `278f4`. These are independently
implemented technical facts, not copied vendor code or a formal clean-room
separation claim. No hardware capture has yet verified this new cold probe.

## Kernel/userspace boundary

The MIT-licensed `fte3600-bridge.h` fixes ABI version 1. One exclusive opener
owns a session. The kernel offers bounded `SPI_IOC_MESSAGE(1)`, logical reset,
atomic/coalesced interrupt consumption, poll, and optional bounded CS control.
The info structure remains 32 bytes; the capability consumes a formerly zero
reserved slot. Old ABI 1 bridges advertise no CS control. There are no arbitrary GPIO
number, arbitrary SPI configuration, memory-map or multi-transfer ioctls. Nodes are
root-only. Removal invalidates the fd; suspend invalidates an open session,
which must be closed and reopened after resume. Kernel buffers containing
transaction data are cleared on free; this is not a complete erasure claim.

## Build and migrate

The module is required in addition to libfprint. The tested kernel header API
is Linux 6.8; compatibility with other kernel releases needs a compile check.
Install headers matching the kernel you will boot. From the repository root:

```sh
make -C kernel/fte3600
sudo install -D -m644 kernel/fte3600/fte3600.ko \
  "/lib/modules/$(uname -r)/extra/fte3600.ko"
sudo depmod -a
```

An explicit compile-only alternative is
`make -C kernel/fte3600 KDIR=/path/to/matching/kernel/headers W=1`.
The Arch package installs the sources through DKMS; it still needs the selected
kernel's headers. A Secure Boot system may require its normal local module
signing/enrollment procedure. No module was loaded by this development task.

Before changing an existing installation, save its FTE3600-specific rules and
service/module configuration for rollback. Stop fingerprint clients. Replace
the old generated libfprint rule with the newly built one, and remove only old
FTE3600-specific rules that force `driver_override=spidev`. This change's udev
generator does not issue spidev binding writes. A reboot clears a previous
in-memory override and lets the module's ACPI alias bind. Do not unbind an
unrelated SPI device. Old FTE3600-only spidev bufsiz and GPIO permission files
are obsolete; preserve settings another device needs.

After boot, verify `fte3600` is the SPI device's driver and that
`/sys/class/misc/fte3600-*/fte3600_abi` contains `1`. If the fprintd service has a
device sandbox, generate exact-node permissions. With no arguments the script
only prints configuration; it fails unless a real, correctly bound ABI 1 bridge
node exists. Its explicit `--install` and `--remove` modes modify the drop-in:

```sh
sh scripts/fte3600-device-allow.sh > /tmp/fte3600-bridge.conf
cat /tmp/fte3600-bridge.conf
sudo install -D -m644 /tmp/fte3600-bridge.conf \
  /etc/systemd/system/fprintd.service.d/10-fte3600-bridge.conf
sudo systemctl daemon-reload
sudo systemctl restart fprintd
```

Run the install steps only if generation succeeded and the paths match the
present sensor. Back up any preexisting destination first. This avoids granting
the entire misc/GPIO device class. If device names change, regenerate the
drop-in. Local SELinux denials need a policy for the actual bridge node; the
old GPIO policy is not a bridge policy.

Rollback restores the saved library/rules/configuration and removes only the
module installed for that kernel (or the corresponding DKMS package), followed
by `depmod` and a reboot. Preserve separately supplied firmware/enrollment.

## Validation scope

Linux 6.8.0-146 Ubuntu headers compiled the bridge with `W=1`. WSL has no target
sensor, so this is compile evidence only. The actual libfprint driver runs in
mock lifecycle tests covering capture/reopen without DMI, ABI/mode/buffer
rejection, unsupported ROM families/OTP, boot-A, failed/cancelled OTP reads,
reset release, and firmware identity gating (including stale runtime IDs).
Both `fte3600_personal_auth=false/true` configurations are checked by
`scripts/check-fte3600.sh`. The optional valid-proprietary-firmware fixture is
not required and is skipped when it has not been supplied.

Enumeration briefly opens the exclusive bridge to select per-device features.
It never loads firmware or starts capture. ROM probing can change boot state,
so it includes reset cleanup. Every open repeats identification and rejects a
sensor different from the one enumerated. All eight identified chips advertise
verification and eight enrollment stages when the personal policy is compiled.
The driver selects a matching profile only when its geometry agrees with the
sensor catalog; it rejects templates from another profile before starting capture.
Default builds continue to expose capture without enrollment or verification.

Capture completion runs a bounded return-to-idle sequence, including the
mode-dependent stop writes, before publishing the image. If idle cannot be
confirmed, the session rejects further captures without sensor I/O; close
releases it and a fresh open must initialize it again. See
[architecture](architecture.md) and [protocol evidence](sensor-protocol-evidence.md).

Still required on hardware: ACPI resource correctness, electrical polarity,
cold ROM probe, full firmware upload, capture, cancellation, reopen and
suspend/resume. Capture implementations for eight chips and all-OEM compatibility are
different claims; the latter is not established by these tests.
