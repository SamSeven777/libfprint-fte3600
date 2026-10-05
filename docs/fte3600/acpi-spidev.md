# Experimental ACPI GPIO glue and stock spidev

The current transport sends SPI transactions through the distribution's `spidev` driver. A
small out-of-tree `fte3600` module exposes the sensor's reset through a one-line
GPIO character device and its interrupt through a separate IRQ-only UIO device.
It does not expose the physical GPIO controller. The independently implemented sensor protocols,
identity checks and firmware validation remain in libfprint.

This is an experimental transport, not an upstream Linux or libfprint feature.
Metadata ABI 2 supports a GPIO interrupt or an ordinary edge-sensitive ACPI
IRQ/`Interrupt()` resource, including the layout in the latest GPD Pocket 3
report. That does not establish a hardware result for this branch. See the
[lifecycle coverage and outstanding work](windows-lifecycle-coverage.md).
See the [implementation validation record](validation-acpi-spidev-2026-10-04.md)
for completed checks and hardware-test limits.
The glue still needs a build for the running kernel and, under Secure Boot, a
signature trusted by that kernel. Using stock spidev does not remove that
requirement. The installer neither enrolls signing keys nor disables Secure
Boot or SELinux. A successful build or mock test is not a hardware result.

## Device contract and access

The physical SPI device must have ACPI HID `FTE3600` and bind to `spidev`.
The glue is a separate platform child bound to `fte3600-glue`; its parent is
that same SPI device. Its GPIO character device has one line, `reset`. Its UIO
companion is named `fte3600-irq`, version `2`, with no memory or port mappings.
This platform child avoids relying on the removed ACPI-driver API.

The glue publishes ABI 2, `ready`/`suspended` status, a generation counter,
ACPI SPI mode/speed, CS-control capability (`fte3600_cs_control`), IRQ polarity/source
and a one-line GPIO count. The CS-control attribute is mandatory and must be `0`
or `1`. Pair discovery verifies
all three nodes' parent relationships, character-device numbers, GPIO line count
and a stable generation. Device numbers, laptop names and physical GPIO-chip
numbers are not identity. Service configuration requires a ready pair;
udev can retain its labels while suspended. Metadata cannot replace a sensor
identity response over SPI.

For an ACPI instance `FTE3600:00`, the verified aliases are:

```
/dev/fte3600-spi-FTE3600:00
/dev/fte3600-gpio-FTE3600:00
/dev/fte3600-irq-FTE3600:00
```

The underlying numbered nodes remain root-owned mode `0600`. The fprintd
drop-in grants SPI/GPIO read-write access and IRQ read-only access only to these
validated nodes, and binds service lifetime to their
systemd device units. No physical `/dev/gpiochip*` wildcard is authorized.
SELinux uses separate `fte3600_spidev_t`, `fte3600_gpio_t` and `fte3600_irq_t` types assigned
only after the udev helper validates the graph. There is no numbered-node
file-context wildcard or permission for the generic GPIO-device type.

At boot, udev can bind only an unbound `FTE3600` SPI device to stock spidev.
The add/bind/change event loads the glue. After publication, a narrowly scoped
refresh requests a change event for that SPI device's spidev node, so either
node creation order can establish the aliases. It does not trigger every SPI
or GPIO device or take ownership from another bound driver.

## Build the current working tree

Use the distribution dependencies in [install.md](install.md#1-prerequisites--dependencies).
Include the `systemd` runtime package: the device-permission helper uses its
`systemd-escape` tool to generate the exact device-unit names.
The kernel must enable `CONFIG_UIO=y` or `m`. Also install the exact headers/development package for the kernel you will
boot, DKMS if using it, and SELinux administration tools on SELinux systems.
For example, Fedora needs `kernel-devel-$(uname -r)`, `dkms` and
`policycoreutils`; Arch needs the matching kernel's headers and `dkms`.
A missing development package for a newer kernel is a build blocker, not
evidence that this module supports it.

The developer PKGBUILD packages committed HEAD. A local Meson build includes
working-tree changes and can be tested before committing:

```sh
meson setup build-acpi-spidev --prefix=/usr \
  -Ddrivers=fte3600 -Dfte3600_personal_auth=false \
  -Dgtk-examples=false -Ddoc=false -Dintrospection=false \
  -Dinstalled-tests=false -Dwerror=true
meson compile -C build-acpi-spidev
meson test -C build-acpi-spidev --print-errorlogs \
  fte3600-context fte3600-resources fpi-spi-transfer \
  fte3600-driver fte3600-lifecycle fte3600-auth-lifecycle \
  fte3600-pair fte3600-setup
make -C kernel/fte3600 KDIR=/lib/modules/$(uname -r)/build W=1
```

These steps compile and test; they do not load a module or install system
configuration. Capture-only is the default. Firmware requirements and the
separate opt-in authentication policy are unchanged; see
[firmware installation](install.md#2-install-the-firmware-for-the-identified-chip)
and [family authentication](family-authentication.md). FW9369, whose raw
silicon ID is `9362`, does not need an application firmware blob.

## Migrate an earlier bridge installation

Do not install this branch over an active DKMS 0.1 bridge. Keep its exact source
revision, package versions and rollback copies before making changes. Record
whether each destination below was absent or belonged to an existing package:

- The library and generated udev rules listed by
  `meson introspect build-acpi-spidev --installed`.
- `/usr/src/fte3600-0.1`, its DKMS records or manually installed module, and any
  signing configuration needed to reinstall it.
- Old `10-fte3600-bridge.conf`/`10-fte3600-gpio.conf` fprintd drop-ins,
  FTE3600 binding overrides, and `fte3600-bridge`/`fte3600-gpio` SELinux modules.
- This branch's `/usr/libexec/fte3600-pair`,
  `/etc/udev/rules.d/70-fte3600-acpi-spidev.rules`,
  `/etc/udev/rules.d/71-fte3600-acpi-spidev-selinux.rules`,
  `/etc/modprobe.d/fte3600-acpi-spidev.conf`,
  `/etc/systemd/system/fprintd.service.d/10-fte3600-acpi-spidev.conf`,
  DKMS 0.2 sources/module and `fte3600-acpi-spidev` SELinux policy.

Close fingerprint clients and use the previous revision's documented uninstall
procedure to remove its own integration. Review separately installed or edited
rules instead of deleting them by name alone. Remove or restore only policies
that belonged to that installation; old broad GPIO rules would still grant
access even after adding a narrower new policy. Reboot before installing the
new module. The setup helper rejects a detected old bridge, old drop-ins,
DKMS 0.1 source directory or either old SELinux module. It does not silently
revoke an unrelated administrator's configuration.

Keep password login working throughout the experiment. Restoring the old
module, library, rules and policy from the recorded version, then rebooting,
provides the rollback path. No enrolled templates are deleted by these steps.

## Install and verify the integration

After migration and the local tests, the manual `/usr` workflow is:

```sh
sudo meson install -C build-acpi-spidev
sudo ./scripts/setup-fte3600.sh install-all
sudo ./scripts/setup-fte3600.sh check
```

The helper installs/builds DKMS **0.2** when DKMS is available, otherwise it
installs the manually built module for the current kernel. Loading it may
require your distribution's normal module-signing procedure. Updating a loaded
module does not replace it; reboot to activate its new code.

Stock spidev must have **at least 32768 bytes** of actual transfer capacity:

```sh
cat /sys/module/spidev/parameters/bufsiz
```

The helper saves `options spidev bufsiz=32768`. If spidev is already loaded
with a smaller buffer, it fails explicitly and asks for a reboot. It never
unloads global spidev, which may serve other devices. For a built-in spidev,
apply the distribution's boot-parameter procedure (`spidev.bufsiz=32768`)
and verify the running value. Installing a firmware blob cannot enlarge it.

The systemd drop-in is generated from a fresh complete pair and published
atomically. A failed update keeps the prior file. The SELinux step installs
the dedicated types before installing its label rule, then triggers only
the verified companion nodes and checks their **actual** labels. The same
restricted helper explicitly labels both `add` and `change` events: udev's
`SECLABEL` alone is skipped on a change when owner/mode already match. The
helper validates the device again and uses `O_PATH` plus `/proc/self/fd` to
pin the inode while setting and reading back its label. It does not open the
UIO driver or request GPIO lines. Installation invokes this operation
synchronously too, so a udev child-process failure cannot be mistaken for
successful labeling. Installing
the CIL module alone does not label existing nodes. Earlier successful steps
can remain after a later failure; a nonzero exit status means installation
is incomplete and is not a request to weaken security settings.

Read-only installation checks, without starting a sensor transaction:

```sh
sudo /usr/libexec/fte3600-pair
systemctl cat fprintd.service
ls -lZ /dev/fte3600-spi-* /dev/fte3600-gpio-* /dev/fte3600-irq-*
cat /sys/module/spidev/parameters/bufsiz
```

The pair helper opens the one-line GPIO chip only to query its line count;
it requests no lines, does not open UIO, and performs no SPI exchange or reset. The command above
does not prove fprintd can obtain anonymous GPIO line-request file descriptors,
set their flags, perform ioctls, or read UIO interrupt events in its actual SELinux
domain. That requires a real enforcing-system run. Use the normal journal and
AVC records to identify a denied operation; do not disable SELinux or generate
a broad `audit2allow` policy. Synthetic policy compilation validates syntax
and intended type boundaries, not the full distribution policy or device I/O.

Then test actual initialization and capture separately. Keep the sensor clear
while an FW9369/`9362` device obtains its startup reference; do not treat
successful installation or an ACPI name as a successful chip identification.
Use an explicit local image output only when needed and do not attach biometric
images or templates to an issue. The independent sensor and lifecycle tests
remain useful regressions, but cannot establish cold-boot, IRQ or image quality
on a new machine.

## Failure and power-management boundaries

The runtime validates the glue generation/status around operations and rejects
a changed epoch after suspend or rebind. A failed session must be closed and
reopened; continuing with stale GPIO/SPI/UIO state is not supported. The reset
GPIO request establishes an exclusive lease; opening UIO then requests the
physical IRQ. Standard four-byte UIO reads report cumulative event counters,
which may coalesce or wrap. The backend always checks sensor status after a
notification. No UIO write/re-enable is needed. Normal close releases UIO before
reset, freeing the IRQ handler even if chip-level shutdown failed. PM also
frees the IRQ and invalidates the old lease; a notification cannot revive it.
The IRQ must be available for exclusive use. SPI `flock` serializes cooperating
clients; stock spidev does not enforce it against a client that ignores locks.

CS negotiation is enabled only for native chip select when the controller
advertises `SPI_CS_HIGH` support. In that case, normal close restores the ACPI
SPI mode, and each open reestablishes that baseline before identification.
GPIO chip select, or a controller without polarity control, retains its current
mode throughout open, discovery, software wake and close. The runtime never
writes `SPI_IOC_WR_MODE32` on this path: stock spidev hides the GPIO `CS_HIGH`
readback bit and forces that bit on a mode write. Reported mode therefore is
not a measurement of physical GPIO CS polarity. Fixed-CS mode must already
have supported mode-0 clock flags; the driver can identify and wake the sensor
on that one effective polarity but cannot correct a wrong polarity.

Both paths configure eight-bit words and at most 1 MHz, bounded by the original
speed and ACPI limit, and restore saved word size/speed at normal close. Those
settings call the controller's setup callback without changing the SPI mode;
controller setup and deselection are not guaranteed to be electrically inert.
SIGKILL or a process crash cannot guarantee immediate native-CS restoration
because stock spidev has no per-open rollback hook. A new open repeats identity
negotiation. Kernel removal or power transitions are
separate lifecycle events, not a promise that every interrupted transaction
has been undone. These are material differences from the previous custom SPI
bridge and require real suspend/resume, crash/reopen and concurrent-client
validation before deployment.

FW9369 final close is separate from per-action cleanup: it verifies awake idle,
masks/acknowledges known sensor events, sends C1 and waits 1 ms before resource
release. Every cleanup stage is attempted and the first error is preserved.
C1 delivery is not a measured sleep or power result. Reopening repeats discovery
and initialization; it does not reuse the previous baseline. Other backends
retain their documented reusable-idle cleanup and do not claim equivalent sleep.

## Remove this experimental integration

```sh
sudo ./scripts/setup-fte3600.sh uninstall
```

This stops fprintd, unloads only the glue, removes this setup's DKMS 0.2 and
configuration, restores default labels on surviving verified nodes, then removes
its default-priority SELinux module. Label restoration requires `matchpathcon`
from the distribution's SELinux tools. It only touches nodes carrying this
module's exact types, pins each inode, and validates its sysfs identity and
device number. A surviving spidev is validated from its physical SPI driver and
ACPI HID even when GPIO, UIO or aliases have already disappeared; a complete
pair is not required for removal. Failed restoration retains the policy and
label helper/rules for a retry. Before restoration, uninstall waits for queued
udev workers and checks that an old SPI event has not reloaded the glue; a
timeout or reload aborts removal while retaining configuration and policy. Busy
glue or another failed step produces an error and does not claim completion.
A previously active service is restarted; a previously stopped one stays
stopped. Global spidev is not unloaded. Package-owned copies under `/usr/lib`
must be handled by the package manager; manual uninstall does not remove them.

Restore the recorded library, previous module and configuration for rollback,
reload systemd/udev and reboot. Review exact file ownership and content before
removing any manually installed library; do not use a broad directory removal
or assume the package manager recorded a manual Meson installation.
