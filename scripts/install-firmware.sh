#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Automated firmware installer for FocalTech FT9361 (FTE3600).
# Downloads the signed vendor driver package from Microsoft Update Catalog,
# extracts the verified 10,396-byte firmware binary, validates its SHA256,
# and installs it to /usr/lib/firmware/fte3600/ft9361.bin.

set -eu

EXPECTED_SHA256="027d776b0f4da0857037bbfe6bd114f52394061c67e8459528f9b2e30114e64f"
EXPECTED_SIZE=10396
TARGET_PATH="/usr/lib/firmware/fte3600/ft9361.bin"

CAB_URL="https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2026/08/e684f740-91ac-4458-9097-09850eaedf9d_4f80a6cb0c9e453d4619667af7c92eadeb165e0f.cab"

find_extractor() {
  if command -v cabextract >/dev/null 2>&1; then
    echo "cabextract"
  elif command -v 7z >/dev/null 2>&1; then
    echo "7z"
  else
    echo ""
  fi
}

require_extractor() {
  EXTRACTOR=$(find_extractor)
  if [ -z "$EXTRACTOR" ]; then
    echo "Error: Neither 'cabextract' nor '7z' was found." >&2
    echo "Please install cabextract:" >&2
    echo "  Fedora: sudo dnf install -y cabextract" >&2
    echo "  Arch:   sudo pacman -S --needed cabextract" >&2
    echo "  Ubuntu: sudo apt install -y cabextract" >&2
    exit 1
  fi
}

require_downloader() {
  if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
    echo "Error: 'curl' or 'wget' is required to download the driver package." >&2
    exit 1
  fi
}

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

echo "=== FTE3600 Firmware Installer ==="

# Check if an existing local file was passed as argument
DLL_PATH=""
if [ $# -ge 1 ] && [ -f "$1" ]; then
  INPUT_FILE="$1"
  case "$INPUT_FILE" in
    *.dll)
      DLL_PATH="$INPUT_FILE"
      ;;
    *.cab)
      require_extractor
      echo "Extracting DLL from provided CAB file: $INPUT_FILE..."
      if [ "$EXTRACTOR" = "cabextract" ]; then
        cabextract -q -d "$TMP_DIR" -F ftWbioUmdfDriverV2.dll "$INPUT_FILE"
      else
        7z e -y -o"$TMP_DIR" "$INPUT_FILE" ftWbioUmdfDriverV2.dll >/dev/null
      fi
      DLL_PATH="$TMP_DIR/ftWbioUmdfDriverV2.dll"
      ;;
    *)
      ACTUAL_SHA=$(sha256sum "$INPUT_FILE" | cut -d' ' -f1)
      if [ "$ACTUAL_SHA" = "$EXPECTED_SHA256" ]; then
        echo "Provided file is already the valid firmware binary."
        if [ "$(id -u)" -ne 0 ]; then
          echo "Writing to $TARGET_PATH requires root. Using sudo..."
          sudo install -Dm644 "$INPUT_FILE" "$TARGET_PATH"
        else
          install -Dm644 "$INPUT_FILE" "$TARGET_PATH"
        fi
        echo "Successfully installed to $TARGET_PATH"
        exit 0
      fi
      ;;
  esac
fi

if [ -z "$DLL_PATH" ]; then
  require_downloader
  require_extractor
  echo "Downloading official driver package from Microsoft Update Catalog..."
  CAB_FILE="$TMP_DIR/fte3600.cab"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$CAB_FILE" "$CAB_URL"
  else
    wget -q -O "$CAB_FILE" "$CAB_URL"
  fi

  echo "Extracting driver DLL..."
  if [ "$EXTRACTOR" = "cabextract" ]; then
    cabextract -q -d "$TMP_DIR" -F ftWbioUmdfDriverV2.dll "$CAB_FILE"
  else
    7z e -y -o"$TMP_DIR" "$CAB_FILE" ftWbioUmdfDriverV2.dll >/dev/null
  fi
  DLL_PATH="$TMP_DIR/ftWbioUmdfDriverV2.dll"
fi

if [ ! -f "$DLL_PATH" ]; then
  echo "Error: Failed to find ftWbioUmdfDriverV2.dll." >&2
  exit 1
fi

echo "Extracting 10,396-byte FT9361 firmware..."
FW_FILE="$TMP_DIR/ft9361.bin"

# Test known offsets first (offset 489824 in driver v2.0.3.102, offset 141824 in older v2.0.3.100)
FOUND=0
for OFFSET in 489824 141824; do
  dd if="$DLL_PATH" of="$FW_FILE" bs=1 skip="$OFFSET" count="$EXPECTED_SIZE" status=none 2>/dev/null || true
  ACTUAL_SHA=$(sha256sum "$FW_FILE" | cut -d' ' -f1)
  if [ "$ACTUAL_SHA" = "$EXPECTED_SHA256" ]; then
    echo "Found verified firmware at offset $OFFSET (SHA256 matches)."
    FOUND=1
    break
  fi
done

if [ "$FOUND" -ne 1 ]; then
  if command -v python3 >/dev/null 2>&1; then
    echo "Scanning DLL for firmware payload..."
    OFFSET=$(python3 - "$DLL_PATH" "$EXPECTED_SHA256" "$EXPECTED_SIZE" <<'EOF'
import hashlib, sys

dll_path = sys.argv[1]
target = sys.argv[2]
size = int(sys.argv[3])

try:
    with open(dll_path, 'rb') as f:
        data = f.read()
except Exception:
    sys.exit(1)

if len(data) < size:
    print("-1")
    sys.exit(0)

found = -1
for i in range(len(data) - size + 1):
    if hashlib.sha256(data[i:i+size]).hexdigest() == target:
        found = i
        break

print(found)
EOF
) || OFFSET="-1"

    if [ "$OFFSET" -ge 0 ] 2>/dev/null; then
      dd if="$DLL_PATH" of="$FW_FILE" bs=1 skip="$OFFSET" count="$EXPECTED_SIZE" status=none
      FOUND=1
      echo "Found verified firmware at offset $OFFSET (SHA256 matches)."
    fi
  else
    echo "Warning: python3 not found, skipping full binary scan fallback." >&2
  fi
fi

if [ "$FOUND" -ne 1 ]; then
  echo "Error: Could not extract valid firmware from $DLL_PATH (SHA256 mismatch)." >&2
  exit 1
fi

echo "Installing verified firmware to $TARGET_PATH..."
if [ "$(id -u)" -ne 0 ]; then
  echo "Writing to $TARGET_PATH requires root. Using sudo..."
  sudo install -Dm644 "$FW_FILE" "$TARGET_PATH"
else
  install -Dm644 "$FW_FILE" "$TARGET_PATH"
fi

echo "=== Firmware installation complete! ==="
echo "Path:   $TARGET_PATH"
echo "SHA256: $(sha256sum "$TARGET_PATH" | cut -d' ' -f1)"
