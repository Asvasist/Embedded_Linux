#!/bin/sh
set -eu
BOARD_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Do this before validating environment inputs so an unsuccessful rebuild
# cannot leave yesterday's deployable SD image looking like a new release.
"$HOST_DIR/bin/python3" "$BOARD_DIR/../../../tools/secure_boot.py" invalidate --images "$BINARIES_DIR"
: "${ZYBO_SIGNING_DIR:?Set ZYBO_SIGNING_DIR to the private signing directory}"
: "${ZYBO_FSBL:?Set ZYBO_FSBL to an RSA-capable Zybo Z7-20 fsbl.elf}"
ZYBO_BOOTGEN=${ZYBO_BOOTGEN:-"$HOST_DIR/bin/bootgen"}

set --
if [ -n "${ZYBO_RECOVERY_FIT:-}" ]; then
    set -- --recovery "$ZYBO_RECOVERY_FIT"
fi

"$HOST_DIR/bin/python3" "$BOARD_DIR/../../../tools/secure_boot.py" assemble \
    --images "$BINARIES_DIR" --keys "$ZYBO_SIGNING_DIR" \
    --fsbl "$ZYBO_FSBL" --bootgen "$ZYBO_BOOTGEN" \
    --fdtget "$HOST_DIR/bin/fdtget" "$@"
if ! support/scripts/genimage.sh -c "$BOARD_DIR/genimage.cfg"; then
    "$HOST_DIR/bin/python3" "$BOARD_DIR/../../../tools/secure_boot.py" invalidate --images "$BINARIES_DIR"
    exit 1
fi
