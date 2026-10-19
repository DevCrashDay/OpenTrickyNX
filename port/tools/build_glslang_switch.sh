#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# glslang (the GLSL -> SPIR-V compiler the Vulkan renderer runs on the
# console) built for the Nintendo Switch, for build_switch.sh VULKAN=1.
#
#   port/tools/build_glslang_switch.sh [prefix]     (default: build/glslang-switch)
#   VULKAN=1 NVK_SDK=... GLSLANG_DIR=build/glslang-switch ./build_switch.sh ...
#
# Static libraries only, without the SPIR-V optimiser (the driver optimises).
# Uses an installed devkitPro (DEVKITPRO=...) or the devkitpro/devkita64
# Docker image, as build_switch.sh does. Needs git.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
PREFIX="${1:-build/glslang-switch}"
TAG="${GLSLANG_TAG:-15.1.0}"
SRC=build/glslang-src
mkdir -p build
[ -d "$SRC/.git" ] || git clone --depth 1 --branch "$TAG" https://github.com/KhronosGroup/glslang.git "$SRC"
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"
REL_PREFIX="${PREFIX#"$ROOT"/}"
OPTS="-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=OFF \
    -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_SPVREMAPPER=OFF \
    -DENABLE_HLSL=OFF -DGLSLANG_ENABLE_INSTALL=ON"
if [ -n "${DEVKITPRO:-}" ]; then
    CMAKE="$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake"
    $CMAKE -S "$SRC" -B build/glslang-build $OPTS -DCMAKE_INSTALL_PREFIX="$PREFIX"
    cmake --build build/glslang-build -j "$(nproc 2>/dev/null || echo 4)"
    cmake --install build/glslang-build
else
    case "$REL_PREFIX" in /*) echo "With Docker the prefix must be inside $ROOT"; exit 1 ;; esac
    docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src devkitpro/devkita64 bash -c "
        set -e
        /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake -S $SRC -B build/glslang-build $OPTS -DCMAKE_INSTALL_PREFIX=/src/$REL_PREFIX
        cmake --build build/glslang-build -j \$(nproc)
        cmake --install build/glslang-build"
fi
echo "glslang for the Switch in $PREFIX"
