# FTE3600 Troubleshooting and Diagnostic Guide

[Documentation Index](README.md) · [Installation Guide](install.md) · [Protocols](protocols.md)

This guide helps diagnose common issues with kernel modules, device nodes, SPI communication, permissions, and finger detection.

---

## 1. Quick Diagnostic Checklist

Run these commands to verify device and driver state:

```bash
# 1. Check if kernel module is loaded
lsmod | grep fte3600

# 2. Check kernel logs for FTE3600 ACPI attachment
dmesg | grep -i fte3600

# 3. Verify created device nodes
ls -l /dev/fte3600-*

# 4. Check SPI buffer size (must be >= 32768)
cat /sys/module/spidev/parameters/bufsiz

# 5. Test sensor detection with debug output
G_MESSAGES_DEBUG=all FP_DEBUG=all /usr/bin/fprintd-enroll "$USER"
```

---

## 2. Common Issues and Fixes

### Issue 1: Device node `/dev/fte3600-*` not found
- **Cause**: Kernel module `fte3600.ko` not loaded, or ACPI table has non-matching HID.
- **Fix**:
  1. Check ACPI tables: `dmesg | grep -i ACPI | grep -i FTE`
  2. Reload module: `sudo modprobe -r fte3600 && sudo modprobe fte3600`
  3. Re-apply udev rules: `sudo udevadm control --reload-rules && sudo udevadm trigger`

### Issue 2: `spidev.bufsiz` too small (< 32 KB)
- **Symptom**: SPI read errors or truncated image captures.
- **Fix**: Create `/etc/modprobe.d/spidev.conf` with:
  ```text
  options spidev bufsiz=65536
  ```
  Then reboot or reload `spidev`.

### Issue 3: Permission Denied for `fprintd`
- **Symptom**: `fprintd` cannot access `/dev/spidevX.Y` or `/dev/uioX`.
- **Fix**: Ensure the systemd service override is active:
  ```bash
  sudo ./install/setup-fte3600-systemd-device.sh --install
  sudo systemctl restart fprintd
  ```

### Issue 4: Sensor not responding / All-0xFF or All-0x00 replies
- **Cause**: Chip is in deep sleep or CS polarity is inverted.
- **Fix**:
  - The driver automatically retries CS polarity on supported controllers (`fte3600_cs_control=1`).
  - Perform a complete power cycle (cold reboot) to reset sensor SRAM state.

### Issue 5: Missing Firmware Error (`ft93xx.bin not found`)
- **Symptom**: Driver identifies sensor (e.g. FT9338 / FT9348 / FT9361) but fails during initialization.
- **Fix**: Ensure the required `.bin` file is placed in `/usr/lib/firmware/fte3600/` with read permissions (`chmod 644`).
