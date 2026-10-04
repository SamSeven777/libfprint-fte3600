#!/bin/sh
# SPDX-License-Identifier: MIT
# Print or install a service drop-in for currently present bridge devices.
set -eu

INSTALL=0
REMOVE=0
HELP=0
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
      HELP=1
      ;;
    *)
      echo "Unknown option: $arg" >&2
      exit 1
      ;;
  esac
done

if [ "$INSTALL" = 1 ] && [ "$REMOVE" = 1 ]; then
  echo '--install and --remove are mutually exclusive.' >&2
  exit 1
fi

if [ "$HELP" = 1 ]; then
  echo "Usage: $0 [--install | --remove]"
  echo "  (no args): Print systemd drop-in configuration to stdout"
  echo "  --install: Install drop-in to $TARGET_FILE and run daemon-reload"
  echo "  --remove:  Remove drop-in and run daemon-reload"
  exit 0
fi

if [ "$INSTALL" = 1 ] || [ "$REMOVE" = 1 ]; then
  if [ "$(id -u)" != 0 ]; then
    echo '--install and --remove require root; use no arguments to print configuration.' >&2
    exit 1
  fi
fi

if [ "$REMOVE" = 1 ]; then
  if [ -f "$TARGET_FILE" ] || [ -L "$TARGET_FILE" ]; then
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
  fte_abi=$(cat "$fte_node/fte3600_abi") || continue
  [ "$fte_abi" = 1 ] || continue
  [ -d "$fte_node/device/driver" ] || continue
  fte_driver=$(readlink -f "$fte_node/device/driver") || continue
  [ "${fte_driver##*/}" = fte3600 ] || continue
  fte_name=${fte_node##*/}
  case "$fte_name" in
    *[!a-zA-Z0-9_.:-]*)
      echo 'Invalid FTE3600 bridge node name; refusing to generate permissions.' >&2
      exit 1
      ;;
  esac
  [ -c "/dev/$fte_name" ] || continue
  CONF_CONTENT="${CONF_CONTENT}DeviceAllow=/dev/${fte_name} rw\n"
  fte_found=1
done

if [ "$fte_found" = 0 ]; then
  echo 'No verified FTE3600 bridge node found (ABI 1, fte3600 driver and character device required).' >&2
  echo 'Check that the bridge module is loaded and the SPI device is bound to fte3600; check for old FTE3600 spidev driver_override rules.' >&2
  exit 1
fi

if [ "$INSTALL" = 1 ]; then
  # Publish only a complete configuration. Keep an existing drop-in intact if
  # creating, writing or setting permissions on its replacement fails.
  mkdir -p -m 0755 "$TARGET_DIR"
  umask 077
  TEMP_FILE=$(mktemp "$TARGET_DIR/.10-fte3600-bridge.conf.XXXXXX")
  trap 'rm -f "$TEMP_FILE"' 0
  trap 'exit 1' HUP INT TERM
  printf "%b" "$CONF_CONTENT" > "$TEMP_FILE"
  chmod 0644 "$TEMP_FILE"
  mv -fT "$TEMP_FILE" "$TARGET_FILE"
  trap - 0 HUP INT TERM
  echo "Installed $TARGET_FILE:"
  cat "$TARGET_FILE"
  if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload
    echo "systemd reloaded successfully."
  fi
else
  printf "%b" "$CONF_CONTENT"
fi
