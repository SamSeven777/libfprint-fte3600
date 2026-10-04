#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Safe, read-only hardware probe script for FTE3600 / FT9361 fingerprint sensors.
# Collects non-biometric hardware topology to diagnose ACPI resource discovery.

set -u

echo "=== FTE3600 Hardware Compatibility Probe ==="
echo "Date: $(date -u '+%Y-%m-%d %H:%M:%SZ')"
echo ""

echo "### 1. System & OS"
echo "- **Kernel:** $(uname -r)"
if [ -f /etc/os-release ]; then
  # shellcheck source=/dev/null
  . /etc/os-release
  echo "- **Distribution:** ${PRETTY_NAME:-Linux}"
fi

echo "### 2. FTE3600 resource bridge"
for node in /sys/class/misc/fte3600-*; do
  [ -r "$node/fte3600_abi" ] || continue
  echo "Node: /dev/$(basename "$node")"
  echo "ABI: $(cat "$node/fte3600_abi")"
  echo "SPI device: $(readlink -f "$node/device")"
done
echo ""

echo "### 3. ACPI FTE3600 Discovery"
echo "\`\`\`"
found_fte=0
for dev in /sys/bus/acpi/devices/*; do
  [ -d "$dev" ] || continue
  hid_file="$dev/hid"
  if [ -f "$hid_file" ]; then
    hid=$(cat "$hid_file" 2>/dev/null || true)
    if [ "$hid" = "FTE3600" ]; then
      found_fte=1
      echo "Device: $(basename "$dev")"
      echo "  HID: $hid"
      [ -f "$dev/path" ] && echo "  ACPI path: $(cat "$dev/path")"
      [ -f "$dev/modalias" ] && echo "  Modalias: $(cat "$dev/modalias")"
      [ -L "$dev/physical_node" ] && echo "  Physical node: $(readlink -f "$dev/physical_node")"
      [ -f "$dev/status" ] && echo "  ACPI status: $(cat "$dev/status")"
    fi
  fi
done

if [ "$found_fte" -eq 0 ]; then
  echo "No ACPI device with HID 'FTE3600' detected in /sys/bus/acpi/devices/"
fi
echo "\`\`\`"
echo ""

echo "### 4. Legacy spidev status (not used by the bridge)"
echo "\`\`\`"
spidevs=$(ls -l /dev/spidev* 2>/dev/null || true)
if [ -n "$spidevs" ]; then
  echo "$spidevs"
else
  echo "No /dev/spidev* character devices found."
fi

if [ -f /sys/module/spidev/parameters/bufsiz ]; then
  echo "spidev bufsiz: $(cat /sys/module/spidev/parameters/bufsiz) bytes"
else
  echo "spidev module not loaded or bufsiz parameter unavailable"
fi
echo "\`\`\`"
echo ""

echo "### 5. GPIO Controllers"
echo "\`\`\`"
seen_chips=""
for chip in /sys/bus/gpio/devices/gpiochip* /sys/class/gpio/gpiochip*; do
  [ -d "$chip" ] || continue
  cname=$(basename "$chip")
  case " $seen_chips " in
    *" $cname "*) continue ;;
  esac
  seen_chips="$seen_chips $cname"
  label=$(cat "$chip/label" 2>/dev/null || echo "unknown")
  base=$(cat "$chip/base" 2>/dev/null || echo "unknown")
  ngpio=$(cat "$chip/ngpio" 2>/dev/null || echo "unknown")
  acpi_path="none"
  if [ -f "$chip/firmware_node/path" ]; then
    acpi_path=$(cat "$chip/firmware_node/path" 2>/dev/null || echo "unknown")
  elif [ -f "$chip/device/firmware_node/path" ]; then
    acpi_path=$(cat "$chip/device/firmware_node/path" 2>/dev/null || echo "unknown")
  fi
  printf "%-12s | label: %-20s | base: %-4s | ngpio: %-4s | acpi: %s\n" \
    "$cname" "$label" "$base" "$ngpio" "$acpi_path"
done
echo "\`\`\`"
echo ""

echo "### 6. FTE3600 ACPI Resource Table (_CRS)"
# Check for iasl and acpidump or tables in sysfs
has_iasl=0
command -v iasl >/dev/null 2>&1 && has_iasl=1

if [ "$has_iasl" -eq 0 ]; then
  echo "> [!NOTE]"
  echo "> \`iasl\` is not installed. To extract the exact GPIO and SPI pin configuration from ACPI, please install acpica tools:"
  echo "> - Fedora: \`sudo dnf install acpica-tools\`"
  echo "> - Arch: \`sudo pacman -S acpica\`"
  echo "> - Ubuntu/Debian: \`sudo apt install acpica-tools\`"
  echo "> and re-run this script with \`sudo\`."
else
  tmpdir=$(mktemp -d)
  trap 'rm -rf "$tmpdir"' EXIT INT TERM

  # Try dumping tables (DSDT and SSDT)
  if [ -r /sys/firmware/acpi/tables/DSDT ]; then
    cp /sys/firmware/acpi/tables/DSDT "$tmpdir/dsdt.dat" 2>/dev/null || true
    cp /sys/firmware/acpi/tables/SSDT* "$tmpdir/" 2>/dev/null || true
    for f in "$tmpdir"/SSDT*; do
      [ -f "$f" ] && mv "$f" "$f.dat" 2>/dev/null || true
    done
  fi

  if [ ! -s "$tmpdir/dsdt.dat" ] && command -v acpidump >/dev/null 2>&1; then
    (cd "$tmpdir" && acpidump -b 2>/dev/null || true)
  fi

  table_count=$(find "$tmpdir" -maxdepth 1 -name '*.dat' 2>/dev/null | wc -l)
  if [ "$table_count" -gt 0 ]; then
    (cd "$tmpdir" && iasl -d ./*.dat >/dev/null 2>&1 || true)

    extracted=""
    if command -v python3 >/dev/null 2>&1; then
      # Extract FTE3600 block from any decompiled .dsl file
      extracted=$(python3 -c '
import glob, os, re, sys

tmp = sys.argv[1]
for dsl_path in glob.glob(os.path.join(tmp, "*.dsl")):
    try:
        with open(dsl_path, "r", errors="ignore") as f:
            lines = f.readlines()
        target = -1
        for i, l in enumerate(lines):
            if "FTE3600" in l:
                target = i
                break
        if target != -1:
            start = target
            for i in range(target, -1, -1):
                if re.search(r"^\s*Device\s*\(", lines[i]):
                    start = i
                    break
            started = False
            depth = 0
            end = start
            for i in range(start, len(lines)):
                depth += lines[i].count("{") - lines[i].count("}")
                if "{" in lines[i]:
                    started = True
                if started and depth <= 0:
                    end = i
                    break
            print("".join(lines[start:end+1]))
            break
    except Exception:
        pass
' "$tmpdir" 2>/dev/null)
    fi

    # Fallback to grep if python3 is unavailable or did not find it
    if [ -z "$extracted" ]; then
      dsl_with_fte=$(grep -l "FTE3600" "$tmpdir"/*.dsl 2>/dev/null | head -n 1 || true)
      if [ -n "$dsl_with_fte" ]; then
        extracted=$(grep -C 20 "FTE3600" "$dsl_with_fte" 2>/dev/null || true)
      fi
    fi

    if [ -n "$extracted" ]; then
      echo "\`\`\`asl"
      echo "$extracted"
      echo "\`\`\`"
    else
      echo "Could not locate 'FTE3600' block in decompiled ACPI tables."
    fi
  else
    echo "Could not read ACPI tables (may need root privileges: try running with \`sudo\`)."
  fi
fi

echo ""
echo "=== End of probe ==="
