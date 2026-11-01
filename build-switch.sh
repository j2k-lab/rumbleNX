#!/usr/bin/env bash
set -euo pipefail

: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
JOBS="${JOBS:-8}"

git config --global --add safe.directory /workspace || true

cmake_args=(
  -S .
  -B build_switch
  -G Ninja
  -DPLATFORM_SWITCH=ON
  -DBUILTIN_NSP=OFF
  -DCMAKE_BUILD_TYPE=Release
  "-DPKG_CONFIG_EXECUTABLE=$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config"
)

if [[ -n "${RUMBLENX_FFMPEG_ROOT:-}" ]]; then
  cmake_args+=("-DRUMBLENX_FFMPEG_ROOT=$RUMBLENX_FFMPEG_ROOT")
fi

if [[ -n "${RUMBLENX_RELEASE_REPOSITORY:-}" ]]; then
  cmake_args+=("-DRUMBLENX_RELEASE_REPOSITORY=$RUMBLENX_RELEASE_REPOSITORY")
fi

cmake "${cmake_args[@]}"
ninja -C build_switch -j"$JOBS" RumbleNX.nro
