#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# OpenTricky for the Nintendo Switch -- build the game from your own SSX Tricky
# (USA) Xbox disc image.
#
#   ./build_switch.sh "path/to/SSX Tricky (USA).iso"
#
# Runs on Linux, macOS or Windows (build_switch.bat, through MSYS2 as for the
# Windows build) with Python 3 (capstone, numpy, pillow) and Docker (Docker
# Desktop on Windows and macOS); the Switch toolchain (devkitPro's devkitA64, libnx, SDL2 and Mesa) is
# the devkitpro/devkita64 image, pulled on first use. Set DEVKITPRO instead to
# use an installed devkitPro and skip Docker.
#
# Result: dist/switch/opentricky/"SSX Tricky.nro". Copy that folder to
# sdmc:/switch/ with your disc image in it -- see docs/switch.md. The build
# contains code translated from your copy of the game: keep it to yourself.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

ISO="${1:-}"
if [ -z "$ISO" ] && [ ! -f game_files/default.xbe ]; then
    echo "usage: ./build_switch.sh \"path/to/SSX Tricky (USA).iso\""
    exit 2
fi
# On Windows (MSYS2): Docker wants a Windows path for the mounted folder, and
# MSYS2 must leave the container's own paths (/src, /opt/...) alone.
ON_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) ON_WINDOWS=1 ;; esac

echo "== [0/4] checking the tools"
PY="${PYTHON:-}"
if [ -z "$PY" ] && [ "$ON_WINDOWS" = 1 ]; then
    # MSYS2's own Python, never the Windows "python3" placeholder (it opens
    # the Microsoft Store and waits).
    PY="${MINGW_PREFIX:-/ucrt64}/bin/python.exe"
    if [ ! -x "$PY" ]; then
        echo "MSYS2's Python is not installed. In the MSYS2 UCRT64 shell:"
        echo "  pacman -S --needed mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone \\"
        echo "            mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow"
        exit 1
    fi
elif [ -z "$PY" ]; then
    PY=python3
    command -v python3 >/dev/null 2>&1 || PY=python
fi
"$PY" -c "import capstone, numpy, PIL" </dev/null 2>/dev/null || {
    if [ "$ON_WINDOWS" = 1 ]; then
        echo "Python needs capstone, numpy and pillow. In the MSYS2 UCRT64 shell:"
        echo "  pacman -S --needed mingw-w64-ucrt-x86_64-python-capstone mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow"
    else
        echo "Python needs capstone, numpy and pillow:  $PY -m pip install capstone numpy pillow"
    fi
    exit 1
}
if [ -z "${DEVKITPRO:-}" ] && ! command -v docker >/dev/null 2>&1; then
    echo "Needs Docker (for the devkitpro/devkita64 image) or an installed devkitPro (DEVKITPRO=...)."
    exit 1
fi

if [ -n "$ISO" ]; then
    echo "== [1/4] reading your disc image"
    "$PY" port/tools/extract_iso.py "$ISO" game_files
else
    echo "== [1/4] game_files/default.xbe already there"
fi

echo "== [2/4] translating the game (about 3 minutes)"
mkdir -p build
PYTHON="$PY" bash port/tools/fork_regen.sh

echo "== [3/4] compiling for the Switch (10 to 20 minutes)"
BUILD=port/build-switch
CONFIGURE="cmake -S port -B $BUILD -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE='-O2 -DNDEBUG' \
    -DCMAKE_C_FLAGS='-ffile-prefix-map=/src/='"
if [ -n "${DEVKITPRO:-}" ]; then
    CMAKE="$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake"
    [ -f "$BUILD/CMakeCache.txt" ] || eval "${CONFIGURE/cmake/$CMAKE}"
    cmake --build "$BUILD" -j "$(nproc 2>/dev/null || echo 4)"
else
    if [ "$ON_WINDOWS" = 1 ]; then
        MOUNT="$(cygpath -m "$ROOT")"; USER_ARGS=()
        export MSYS2_ARG_CONV_EXCL='*'
    else
        MOUNT="$ROOT"; USER_ARGS=(-u "$(id -u):$(id -g)")
    fi
    docker info >/dev/null 2>&1 || { echo "Docker is not running: start Docker (Docker Desktop on Windows) and try again."; exit 1; }
    docker run --rm "${USER_ARGS[@]}" -v "$MOUNT:/src" -w /src devkitpro/devkita64 bash -c "
        set -e
        [ -f $BUILD/CMakeCache.txt ] || ${CONFIGURE/cmake//opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake}
        cmake --build $BUILD -j \$(nproc)"
fi

echo "== [4/4] dist/switch/opentricky"
OUT=dist/switch/opentricky
mkdir -p "$OUT"
cp "$BUILD/SSX Tricky.nro" "$OUT/"
echo
echo "Done: $OUT/SSX Tricky.nro"
echo "Copy $OUT to sdmc:/switch/ on your SD card, put your disc image (.iso) in it,"
echo "and start it from the Homebrew Menu opened over a game (hold R), see docs/switch.md."
