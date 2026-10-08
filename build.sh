#!/bin/bash
# Cross-builds gsdm and LoginWindow.app for the Pi (Darwin arm64) with the
# iokit repo's toolchain; this checkout is built as-is, not the pinned
# third_party/gsdm.
#
#   ./build.sh                 build; output lands in iokit's libc_build/gnustep/root
#   ./build.sh deploy          build, then copy LoginWindow.app and gsdm to the Pi
#
# IOKIT_DIR   the iokit checkout (default ../iokit)
# DEPLOY_HOST ssh target for "deploy" (default root@10.0.0.142; needs ssh access)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd -P)"
IOKIT_DIR="${IOKIT_DIR:-$HERE/../iokit}"
STAGING="$IOKIT_DIR/tools/userland_staging"
[ -x "$STAGING/build_gsdm.sh" ] || { echo "error: $STAGING/build_gsdm.sh not found; set IOKIT_DIR" >&2; exit 1; }

GSDM_SRC="$HERE" "$STAGING/build_gsdm.sh"

[ "${1:-}" = deploy ] || exit 0
HOST="${DEPLOY_HOST:-root@10.0.0.142}"
GSROOT="$(cd "$STAGING/libc_build/gnustep/root/usr/GNUstep/System" && pwd -P)"
tar -C "$GSROOT/Library/CoreServices" -cf - LoginWindow.app |
    ssh "$HOST" 'rm -rf /System/Library/CoreServices/LoginWindow.app && tar -C /System/Library/CoreServices -xf -'
echo "deployed LoginWindow.app to $HOST (restart gsdm to pick it up)"
