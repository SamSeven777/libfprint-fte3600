# Troubleshooting

[Documentation index](README.md)

## Discovery

Start with the read-only integration checks:

```sh
sudo ./scripts/setup-fte3600.sh check
sudo /usr/libexec/fte3600-pair
cat /sys/module/spidev/parameters/bufsiz
```

The current transport needs three related character devices: stock spidev,
the glue's one-line reset GPIO chip and its IRQ-only UIO device. The physical
SPI device binds to `spidev`; its platform child binds to `fte3600-glue` and
publishes `fte3600_glue_abi=2`. The pair helper validates their relationship.
The former `/sys/class/misc/fte3600-*` ABI 1 interface is not used. Follow
[installation and migration](acpi-spidev.md) when replacing that interface;
do not combine old bridge rules or permissions with the current setup.

The `fte3600` module must be built for the running kernel, with UIO support.
On a Secure Boot system, a module-load rejection may require the distribution's
module signing and key-enrollment procedure. Setup does not perform enrollment.
Check the kernel journal for the actual error before diagnosing the sensor.

The glue requires one SPI connection, one single-pin reset GpioIo and one
edge-sensitive interrupt: single-pin GpioInt or a single ordinary ACPI
IRQ/Interrupt. Ambiguous resources, missing controllers, conflicting reset
properties and unsupported trigger modes are explicit failures. DMI is not
consulted, and guessed GPIO numbers are not a remedy. See the
[resource contract](../../kernel/fte3600/README.md#resources-and-device-pairing).

## No expected MCU response

All-zero/all-one replies do not identify a chip or prove missing firmware.
Current discovery includes a bounded legacy software-wake retry; ROM recovery
still requires its own positive identity evidence. Firmware must match the
identified chip's exact size/hash. See the [support matrix](status.md#implemented-functions-and-test-limits)
and [identification rules](dynamic-discovery.md#two-independent-decisions).

Setup requires a running `spidev.bufsiz` of at least 32768. A configuration
file does not enlarge an already loaded module's buffer; follow the documented
reboot procedure. Each backend also checks its largest transaction. The SPI
controller may reject a transfer below that buffer size; a full legacy image
or RAM readback cannot be split to hide that error. See
[transaction sizes](dynamic-discovery.md#electrical-and-protocol-limits).

Automatic CS trials are available only when the glue reports
`fte3600_cs_control=1`. GPIO-controlled CS and controllers without polarity
control retain their current mode. A reported spidev mode bit is not a
measurement of physical GPIO CS polarity. Do not infer Medion's chip or CS
requirements from a GPD result.

## Permissions and suspend

The validated device nodes are root-owned mode `0600`. The current setup grants
fprintd access to those exact SPI/reset/UIO nodes and uses dedicated SELinux
types `fte3600_spidev_t`, `fte3600_gpio_t` and `fte3600_irq_t`. Use the setup
helper's checks and actual service/journal/AVC errors to distinguish missing
nodes, systemd device restrictions and SELinux denials. A successful pair check
does not prove fprintd can request reset or read IRQ events in its own domain.
Do not grant a physical GPIO wildcard, install the old broad GPIO policy or
disable SELinux. See [system integration](acpi-spidev.md#install-and-verify-the-integration).

Suspend, rebind and an invalidated transport end the open session. Close it and
reopen after the resources are ready; repeatedly capturing on the failed
session cannot recover it. The glue releases the physical IRQ on close or
invalidation. Stock spidev cannot atomically revoke an in-flight transfer or
restore native CS after SIGKILL; these and real suspend/resume behavior still
need hardware validation.

## Enrollment

Default builds expose capture only. Enrollment and verification require the
explicit personal-auth build described in [installation](install.md#5-enrollment--verification).
Keep a password fallback and use direct `fprintd-verify` experiments first.
Lift fully between enrollment stages and vary placement slightly. Low-quality
or duplicate samples may need retry; not every backend has a verified release
observer. Keep FW9369 uncovered during opening for its baseline calibration.
A successful personal test is not a population authentication evaluation.

When the driver reports an incompatible old template, deliberate deletion with
`fprintd-delete "$USER"` removes that user's enrolled prints; re-enroll afterward.
Do not delete all of `/var/lib/fprint`. Do not use experimental biometrics for
system-wide sudo/root access.

Public reports should include the tested commit, build options, identified chip
if available, sanitized resource information and relevant errors. Do not post
fingerprint images, templates, descriptors, full private logs or dumps.
