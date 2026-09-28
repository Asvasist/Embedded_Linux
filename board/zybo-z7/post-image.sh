#!/bin/sh
# Called by Buildroot from its top directory, BINARIES_DIR is exported.
set -e

BOARD_DIR="$(dirname "$0")"

install -m 0644 "$BOARD_DIR/extlinux.conf" "$BINARIES_DIR/extlinux.conf"

support/scripts/genimage.sh -c "$BOARD_DIR/genimage.cfg"
