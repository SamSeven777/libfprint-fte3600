#!/bin/sh
# SPDX-License-Identifier: MIT
# Print or install a service drop-in for currently present bridge devices.
set -eu

INSTALL=0
REMOVE=0
TARGET_DIR="/etc/systemd/system/fprintd.service.d"
TARGET_FILE="$TARGET_DIR/10-fte3600-bridge.conf"

for arg in "$@"; do
  case "$arg" in
    --install|-i)
      INSTALL=1
      ;;
    --remove|-r|--uninstall)
      REMOVE=1
      ;;
    --help|-h)
      echo "Usage: $0 [--install | --remove]"
      echo "  (no args): Print systemd drop-in configuration to stdout"
      echo "  --install: Install drop-in to $TARGET_FILE and run daemon-reload"
      echo "  --remove:  Remove drop-in and run daemon-reload"
      exit 0
      ;;
    *)
      echo "Unknown option: $arg" >&2
      exit 1
      ;;
  esac
done

if [ "$REMOVE" = 1 ]; then
  if [ -f "$TARGET_FILE" ]; then
    rm -f "$TARGET_FILE"
    echo "Removed $TARGET_FILE"
    if command -v systemctl >/dev/null 2>&1; then
      systemctl daemon-reload
      echo "systemd reloaded."
    fi
  else
    echo "$TARGET_FILE does not exist, nothing to remove."
  fi
  exit 0
fi

fte_found=0
CONF_CONTENT="[Service]\n"

for fte_node in /sys/class/misc/fte3600-*; do
  [ -r "$fte_node/fte3600_abi" ] || continue
  [ "$(cat "$fte_node/fte3600_abi")" = 1 ] || continue
  [ "$(basename "$(readlink -f "$fte_node/device/driver")")" = fte3600 ] || continue
  fte_name=$(basename "$fte_node")
  case "$fte_name" in *[!a-zA-Z0-9_.:-]*) exit 1 ;; esac
  [ -c "/dev/$fte_name" ] || continue
  CONF_CONTENT="${CONF_CONTENT}DeviceAllow=/dev/${fte_name} rw\n"
  fte_found=1
done

if [ "$fte_found" = 0 ]; then
  if [ -d /sys/bus/acpi/devices/FTE3600:00 ] || [ -c /dev/fte3600-spi-FTE3600:00 ]; then
    CONF_CONTENT="${CONF_CONTENT}DeviceAllow=/dev/fte3600-spi-FTE3600:00 rw\n"
    fte_found=1
  else
    echo 'No FTE3600 bridge node found; load the module first.' >&2
    exit 1
  fi
fi

if [ "$INSTALL" = 1 ]; then
  mkdir -p "$TARGET_DIR"
  printf "%b" "$CONF_CONTENT" > "$TARGET_FILE"
  echo "Installed $TARGET_FILE:"
  cat "$TARGET_FILE"
  if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload
    echo "systemd reloaded successfully."
  fi
else
  printf "%b" "$CONF_CONTENT"
fi
