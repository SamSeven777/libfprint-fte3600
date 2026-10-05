#!/bin/sh
# SPDX-License-Identifier: MIT
# Print or install exact companion-node permissions after validating the pair.
set -eu

INSTALL=0
REMOVE=0
HELP=0
TARGET_DIR="/etc/systemd/system/fprintd.service.d"
TARGET_FILE="$TARGET_DIR/10-fte3600-acpi-spidev.conf"
PAIR_HELPER="$(dirname -- "$0")/fte3600-pair.py"
[ -f "$PAIR_HELPER" ] || PAIR_HELPER=/usr/libexec/fte3600-pair

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

if ! command -v systemd-escape >/dev/null 2>&1; then
  echo 'systemd-escape is required; install the systemd package before generating device permissions.' >&2
  exit 1
fi

PAIR_DATA=$(python3 "$PAIR_HELPER")
NEWLINE='
'
UNIT_CONTENT="[Unit]${NEWLINE}"
SERVICE_CONTENT="[Service]${NEWLINE}"
while read -r role node alias; do
  case "$role:$alias" in
    spi:/dev/fte3600-spi-FTE3600:*|gpio:/dev/fte3600-gpio-FTE3600:*|irq:/dev/fte3600-irq-FTE3600:*) ;;
    *) echo 'Unexpected validated pair output.' >&2; exit 1 ;;
  esac
  unit=$(systemd-escape --path --suffix=device "$alias")
  UNIT_CONTENT="${UNIT_CONTENT}BindsTo=${unit}${NEWLINE}After=${unit}${NEWLINE}"
  access=rw
  [ "$role" != irq ] || access=r
  SERVICE_CONTENT="${SERVICE_CONTENT}DeviceAllow=${alias} ${access}${NEWLINE}"
done <<EOF
$PAIR_DATA
EOF
CONF_CONTENT="${UNIT_CONTENT}${SERVICE_CONTENT}"

if [ "$INSTALL" = 1 ]; then
  # Publish only a complete configuration. Keep an existing drop-in intact if
  # creating, writing or setting permissions on its replacement fails.
  mkdir -p -m 0755 "$TARGET_DIR"
  umask 077
  TEMP_FILE=$(mktemp "$TARGET_DIR/.10-fte3600-acpi-spidev.conf.XXXXXX")
  trap 'rm -f "$TEMP_FILE"' 0
  trap 'exit 1' HUP INT TERM
  printf "%s" "$CONF_CONTENT" > "$TEMP_FILE"
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
  printf "%s" "$CONF_CONTENT"
fi
