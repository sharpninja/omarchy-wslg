#!/bin/bash
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${WSLG_BUILD_DIR:-/home/omarchy/src/wslg-build}
if [ ! -f "$build/build.ninja" ]; then
  meson setup "$build" "$root"
fi
meson compile -C "$build"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
mkdir -p "$XDG_RUNTIME_DIR"
"$build/bridge-suite" "$build/mock-parent" "$build/wslg-bridge"
"$build/qualify-drm"
