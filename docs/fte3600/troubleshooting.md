# Troubleshooting & Diagnostics

This guide provides targeted diagnostic steps and resolutions for common issues encountered when running `libfprint-fte3600`.

---

## 1. Hardware & Device Discovery

### Symptom: `fprintd` reports no device found, or device probe fails

1. **Verify Device Nodes**:
   Check if the kernel exposes the required SPI and GPIO character devices:
   ```sh
   ls -l /dev/spidev* /dev/gpiochip*
   ```
   If `/dev/spidev*` does not exist:
   - Verify that your kernel has `CONFIG_SPI_SPIDEV` built-in or loaded as a module (`modprobe spidev`).
   - Ensure the udev rules installed with the package or driver have triggered (`sudo udevadm trigger`).

2. **Verify DMI Compatibility**:
   The driver enforces strict fail-closed probe gating against verified DMI profiles:
   ```sh
   cat /sys/class/dmi/id/sys_vendor
   cat /sys/class/dmi/id/product_name
   ```
   Check against the [Hardware Support Matrix](status.md). If your machine matches the hardware but differs in DMI naming strings, probe aborts with `-ENODEV` to prevent driving incorrect GPIO lines.

3. **Check Journal Logs**:
   ```sh
   journalctl -u fprintd.service -b --no-pager
   ```
   Look for messages prefixed with `fte3600:`.

---

## 2. Cold-Boot Recovery & SPI Buffer Size

### Symptom: Journal logs `FT9361 MCU did not return to idle (00 00)`

**Root Cause**:
The FT9361 sensor loses its volatile SRAM microcode during power-off (cold boot). When powered on, its MCU starts in uninitialized bootloader mode (status code `00 00` instead of operational ready code `a5 5a`).

**Resolution**:
1. Ensure the verified firmware payload is installed at `/usr/lib/firmware/fte3600/ft9361.bin`:
   ```sh
   ls -l /usr/lib/firmware/fte3600/ft9361.bin
   sha256sum /usr/lib/firmware/fte3600/ft9361.bin
   ```
   - Expected file size: `10,396` bytes.
   - Expected SHA256: `027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f`.
   - If missing, run `./scripts/install-firmware.sh`.

2. Check SPI buffer size parameter:
   ```sh
   cat /sys/module/spidev/parameters/bufsiz
   ```
   - Must output **`32768`**.
   - If it outputs `4096` or `8192`, the 10,403-byte continuous wire transfer will fail with `EMSGSIZE` / `EINVAL`.
   - Install `/etc/modprobe.d/fte3600-spidev.conf` and reboot:
     ```sh
     sudo install -Dm644 config/modprobe.d/fte3600-spidev.conf /etc/modprobe.d/fte3600-spidev.conf
     sudo reboot
     ```

---

## 3. Permissions, Sandboxing & SELinux

### Symptom: `Failed to claim GPIO line: Permission denied`

Modern Linux distributions confine `fprintd` inside a systemd service sandbox that blocks access to raw device nodes by default.

1. **Systemd DeviceAllow Drop-in**:
   Ensure the fprintd unit includes permission for GPIO character devices:
   ```sh
   systemctl cat fprintd.service | grep -i deviceallow
   ```
   If `DeviceAllow=char-gpiochip rw` is not present, install the service drop-in:
   ```sh
   sudo install -Dm644 config/systemd/10-fte3600-gpio.conf /etc/systemd/system/fprintd.service.d/10-fte3600-gpio.conf
   sudo systemctl daemon-reload
   sudo systemctl restart fprintd.service
   ```

2. **Fedora SELinux Confinement**:
   On Fedora with SELinux in Enforcing mode, the SELinux domain `fprintd_t` is denied read/write access to `gpio_device_t`. Install the provided CIL module:
   ```sh
   sudo semodule -i config/selinux/fte3600-gpio.cil
   sudo systemctl restart fprintd.service
   ```
   Confirm that the module is loaded:
   ```sh
   semodule -l | grep fte3600
   ```

---

## 4. Enrollment & Matching Quality Tuning

### Recommended Enrollment Technique (64 × 80 Sensor)

Because the physical active area is only 3.2 mm × 4.0 mm, successful authentication depends on capturing overlapping spatial features across the 8 enrollment stages:

- **Lift finger completely** between prompts to allow the sensor baseline to settle.
- **Introduce slight variations**: During the 8 stages, tilt the finger slightly, capture the fingertip, center, and left/right margins.
- **Avoid pressing too hard**: Excessive pressure flattens ridges and lowers image contrast, causing the driver's pre-filter to reject the sample.
- **Avoid zero-displacement presses**: Repeatedly pressing the identical contact spot will be rejected by the duplicate detector.

### Incompatible Templates (Upgrading to Policy Version 3)

If upgrading from an earlier build, existing templates in `/var/lib/fprint/` will fail validation because Policy Version 3 uses updated geometric consensus headers.
Purge old templates and re-enroll:
```sh
fprintd-delete "$USER"
fprintd-enroll -f left-index-finger "$USER"
```

---

## 5. Filing an Issue

When reporting hardware or driver issues:
1. Provide distribution name and kernel version (`uname -a`).
2. Provide hardware DMI information (`cat /sys/class/dmi/id/{sys_vendor,product_name}`).
3. Provide system journal logs:
   ```sh
   journalctl -u fprintd.service -b --no-pager
   ```
4. **Privacy Warning**: Never attach raw biometric images, memory dumps, or serialized templates to public bug reports.

