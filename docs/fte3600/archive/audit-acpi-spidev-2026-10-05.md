<a id="acpi-spidev-独立复审2026-10-05"></a>

# Independent ACPI/spidev audit — 2026-10-05

[Documentation index](README.md)

**Historical audit.** This report records the failures observed before the
fixes published in `6e25b20`. The original audit covered the `acpi-spidev`
branch at `1ce4c740ac759b69537312f46ac956e39c4a9b53` **plus all uncommitted
changes present when the audit began**. Line numbers below refer to that
working tree, not necessarily to the named commit or current source.

The audit itself did not change production code or existing tests, commit or
push changes, or access hardware. Reproduction programs and logs were kept in
the development workspace under `work/`. Subsequent fixes are summarized at
the end and in the [release validation record](validation-release-2026-10-05.md).

Four issues required correction, including two P1 issues. Earlier passing
tests remained valid for their scope, but their simulators did not cover the
states and system behavior below. They could not establish hardware readiness.

<a id="1-p1fw9369-通信恢复停止手指检测后可能一直等待中断"></a>

## 1. P1: FW9369 communication recovery could stop detection and then wait forever

Location at audit time:
`libfprint/drivers/fte3600-fw9369.c:366–374, 633–637, 978–986`.

Trigger: while waiting for finger-down or finger-up, communication validation
fails and `wake()` recovers it; the next event is neither the requested
down event nor an unambiguous release.

The recovery sent C0 unconditionally, stopping the previous detection mode,
and verified SFR 80 as 50. On an unrelated event, the event handler returned
directly to `CAPTURE_WAIT` without configuring and starting C2 again.
Setting `self->armed = TRUE` changed a software flag, not the sensor.
If the sensor produced no further interrupt, the action could wait until
cancellation. An unrelated-event count limit cannot bound a wait with no
subsequent event.

Reproduction used an isolated copy of the test fixture with the actual
production backend. The model required C2 to start detection, C0 to stop it,
and detection to be running when waiting for an interrupt. The first injected
event was UP while the action was waiting for DOWN:

- With initial communication failure and recovery, the second interrupt wait
  failed `audit_fdt_running should be TRUE`, exit status -6.
- With the same unrelated UP event but no initial communication failure,
  detection stayed active and the following DOWN completed capture, exit status 0.

Both scenarios used the same production objects. The control ruled out an
inherently invalid unrelated-UP fixture. The original simulator did not require
the detector to be running before delivering an interrupt and missed the issue.

Required correction: track whether communication recovery stopped detection
and rearm the appropriate mode before waiting again, preserving release-event
latching and acknowledgement order. A software `armed` flag alone is insufficient.

Evidence files: `work/fw9369-audit/reproduce.py`, `result.txt`,
`control-result.txt`. This reproduction does not establish the cause of the
periodic interrupts reported on hardware after close.

<a id="2-p1selinux-标签规则的-change-事件不能保证给现有节点打标签"></a>

## 2. P1: SELinux CHANGE rules did not guarantee labeling existing nodes

Location at audit time: `scripts/setup-fte3600.sh:117–133`, especially 122–124.

Trigger: the base rules have already made the nodes `root:root 0600`.
After installing SELinux policy and label rules, the installer only issues
`udevadm trigger --action=change`; ownership and permissions do not change.

In the checked systemd v255 implementation, `apply_mac` is true only for ADD.
The code containing explicit SECLABEL application runs only when ownership or
permissions change, or `apply_mac` is true. A CHANGE event can therefore match
the rule without applying the requested label. The installer's subsequent
actual-label check fails. Broadening SELinux access does not fix this.

The same condition affects delayed pairing at boot: if the GPIO/UIO ADD event
arrives before complete pairing metadata is published, a later CHANGE alone
does not guarantee labeling.

Sources:

- [systemd v255 udev-event.c](https://raw.githubusercontent.com/systemd/systemd/v255/src/udev/udev-event.c):
  `apply_mac = device_for_action(dev, SD_DEVICE_ADD)`.
- [systemd v255 udev-node.c](https://raw.githubusercontent.com/systemd/systemd/v255/src/udev/udev-node.c):
  the ownership/permission-change or `apply_mac` condition encloses explicit
  SECLABEL handling.

This was confirmed from source; the installer was not run on an enforcing
Fedora machine. The earlier installer fixture directly returned the desired
label from `stat`, without modeling the conditions under which udev applies it.

Required correction: explicitly set labels on exact nodes whose identities
have been revalidated, including delayed pairing after ADD. Tests must model
actual label application, not just assert that a trigger command ran.

<a id="3-p2gpio-片选被错误声明为可翻转探测可能在唤醒前退出"></a>

## 3. P2: GPIO-controlled CS was incorrectly advertised as switchable

Location at audit time: `libfprint/drivers/fte3600-transport.c:523, 662–664`
and discovery's `IDENTIFY_NEXT_POLARITY` path.

Trigger: the SPI controller uses a GPIO descriptor for chip select, the current
active-low polarity is correct, and the sleeping sensor does not answer the
initial identity read.

Userspace unconditionally advertised CS polarity switching. Linux v6.8 stock
spidev hides SPI_CS_HIGH in MODE/MODE32 reads for GPIO-controlled CS and forces
it internally on writes; physical polarity also involves the GPIO descriptor.
This is not the ordinary userspace ability to flip CS and read back the same
mode. After requesting active-high CS, the driver required identical readback
and failed before trying 0x70 wake on the original working polarity.
Counting ACPI resources does not establish which mechanism the controller
uses for chip select.

Source: [Linux v6.8 spidev.c](https://raw.githubusercontent.com/torvalds/linux/v6.8/drivers/spi/spidev.c),
the `use_gpio_descriptors` and `spi_get_csgpiod()` branches in MODE/MODE32 handling.

Reproduction used an isolated lifecycle fixture modeling this readback behavior
with an FT9361 that needed wake on active-low CS. Production discovery returned
from state 14 with:

```text
Cannot configure FTE3600 chip-select polarity: Input/output error
no 0x70 wake sent on working CS
```

The reproduction's exit status 0 meant that it successfully asserted this
failure, not that the defect was fixed. Evidence:
`work/audit-2026-10-05/cs_gpio_repro.py`, `cs-gpio-result.txt`.

Required correction: expose the actual CS-control capability and allow wake
and recovery on the existing configuration when polarity cannot be changed.
There was no evidence that the reported GPD or Medion machines actually used
GPIO-controlled CS; this defect cannot be assigned as their failure cause.

<a id="4-p2配对不完整时卸载遗漏标签恢复却报告成功"></a>

## 4. P2: incomplete-pair removal skipped label restoration but reported success

Location at audit time: `scripts/setup-fte3600.sh:159–164, 193–198`.

Trigger: the glue has been unloaded or GPIO/UIO pairing is incomplete, but a
spidev node survives with a dedicated FTE3600 label.

The uninstaller converted a `pair_data` failure to an empty string, recording no
nodes for restoration. It then removed SELinux policy and skipped all
`restorecon` calls because the list was empty, returning success.
The surviving node retained its dedicated label, whose type might no longer
exist in the policy.

Reproduction executed the actual uninstaller in an isolated copy of the
existing installation fixture, with policy installed, glue unloaded, pairing
failed and spidev still present:

```text
exit: 0
surviving-spidev-node: True
policy-removed: True
restorecon-calls: []
```

Only temporary paths and mocked system commands were used; host policy was
not modified. Evidence: `work/audit_setup_incomplete_pair.py`.

Required correction: independently validate surviving nodes and device numbers
when the full pair is unavailable, or report incomplete cleanup. Do not turn
pairing failure into successful removal, and do not relabel unrelated nodes.

<a id="审计范围与剩余验证"></a>

## Audit scope and remaining validation

Kernel UIO open/close, removal ordering, reset leases, suspend invalidation and
deferred resource release were also reviewed. No new issue of comparable
severity was established there; this does not prove those paths defect-free.

The [earlier validation record](validation-acpi-spidev-2026-10-04.md) remains a
record of its own checks. This audit added isolated reproductions for missing
system behavior instead of treating another run of the same simulators as
independent confirmation.

The audited working tree was not exercised on A1, GPD Pocket 3 or Medion E3224.
A complete Fedora Secure Boot + SELinux enforcing installation, capture,
close and recovery test also remained outstanding.

<a id="后续修复状态"></a>

## Subsequent fixes

The user later authorized correction and publication. The historical evidence
and original line references above are retained. Commit `6e25b20` includes
fixes for all four issues: restart C2 when FW9369 recovery stopped detection,
label exact devices through a restricted helper, obtain CS-control capability
from the kernel, and independently restore surviving nodes in an incomplete
pair. Removal also drains queued udev work and checks for module reload before
restoring labels, preventing an in-flight label worker from overwriting the
restored default.

See [release validation](validation-release-2026-10-05.md) for subsequent
checks and remaining hardware limits. These fixes do not invalidate the
findings about the old simulators, and do not constitute hardware validation.
