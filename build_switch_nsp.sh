#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# OpenTricky for the Nintendo Switch -- a home-menu icon for the game: an
# installable .nsp (a "forwarder") that starts
# sdmc:/switch/opentricky/SSX Tricky.nro, made from your own keys and picture.
#
#   ./build_switch_nsp.sh path/to/prod.keys path/to/cover.png
#
# prod.keys is your console's key file (Lockpick_RCM). The cover is any
# picture; a square one fills the icon. Needs Python 3 with Pillow and Docker
# (or DEVKITPRO), as build_switch.sh. Result: dist/switch/"SSX Tricky.nsp".
# Install it with your usual installer (DBI, Tinfoil, ...); the console needs
# custom firmware with signature patches, as for any unsigned .nsp. Keep the
# .nsp to yourself: it is made with your console's keys.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

KEYS="${1:-}"; COVER="${2:-}"
if [ -z "$KEYS" ] || [ -z "$COVER" ]; then
    echo "usage: ./build_switch_nsp.sh path/to/prod.keys path/to/cover.png"
    exit 2
fi
[ -f "$KEYS" ] || { echo "no key file at $KEYS"; exit 1; }
[ -f "$COVER" ] || { echo "no picture at $COVER"; exit 1; }
ON_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) ON_WINDOWS=1 ;; esac
PY="${PYTHON:-}"
if [ -z "$PY" ] && [ "$ON_WINDOWS" = 1 ]; then
    PY="${MINGW_PREFIX:-/ucrt64}/bin/python.exe"
elif [ -z "$PY" ]; then
    PY=python3
    command -v python3 >/dev/null 2>&1 || PY=python
fi
"$PY" -c "import PIL" </dev/null 2>/dev/null || { echo "Python needs Pillow (see build_switch.sh)"; exit 1; }

echo "== [1/2] the loader (port/switch/forwarder)"
OUT=build/forwarder
rm -rf "$OUT"; mkdir -p "$OUT"
SWITCH_RULES="${DEVKITPRO:-/nonexistent}/libnx/switch_rules"
if [ -n "${DEVKITPRO:-}" ] && [ -f "$SWITCH_RULES" ]; then
    cp -r port/switch/forwarder "$OUT/src"
    make -C "$OUT/src" RELEASE=1 -j4
    cp "$OUT/src/forwarder.nso" "$OUT/src/forwarder.npdm" "$OUT/"
else
    command -v docker >/dev/null 2>&1 || { echo "Needs Docker (for the devkitpro/devkita64 image) or an installed devkitPro (DEVKITPRO=...)."; exit 1; }
    docker info >/dev/null 2>&1 || { echo "Docker is not running: start Docker (Docker Desktop on Windows) and try again."; exit 1; }
    if [ "$ON_WINDOWS" = 1 ]; then
        MOUNT="$(cygpath -m "$ROOT")"; USER_ARGS=()
        export MSYS2_ARG_CONV_EXCL='*'
    else
        MOUNT="$ROOT"; USER_ARGS=(-u "$(id -u):$(id -g)")
    fi
    docker run --rm ${USER_ARGS[@]+"${USER_ARGS[@]}"} -v "$MOUNT:/src" devkitpro/devkita64 bash -c "
        set -e
        cp -r /src/port/switch/forwarder /tmp/f
        make -C /tmp/f RELEASE=1 -j\$(nproc)
        cp /tmp/f/forwarder.nso /tmp/f/forwarder.npdm /src/$OUT/"
fi

echo "== [2/2] the .nsp"
mkdir -p dist/switch
"$PY" port/tools/switch/make_forwarder.py --keys "$KEYS" --cover "$COVER" \
    --nso "$OUT/forwarder.nso" --npdm "$OUT/forwarder.npdm" --out "dist/switch/SSX Tricky.nsp"
echo
echo "Done: dist/switch/SSX Tricky.nsp. Install it on the console; the icon starts"
echo "sdmc:/switch/opentricky/SSX Tricky.nro, with the game's full memory (no hold-R)."
