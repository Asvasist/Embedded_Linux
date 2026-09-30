#!/bin/sh
# Called by Buildroot from its top directory, BINARIES_DIR is exported.
set -e

BOARD_DIR="$(dirname "$0")"

for arg in "$@"; do
    if [ "$arg" = --zybo-secure ]; then
        exec /bin/sh "$BOARD_DIR/secure/post-image.sh"
    fi
done

install -m 0644 "$BOARD_DIR/extlinux.conf" "$BINARIES_DIR/extlinux.conf"

support/scripts/genimage.sh -c "$BOARD_DIR/genimage.cfg"
