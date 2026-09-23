#!/bin/bash
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${WSLG_BUILD_DIR:-/home/omarchy/src/wslg-build}
if [ ! -x "$build/wslg-bridge" ]; then
  echo "missing $build/wslg-bridge; run scripts/build-and-test.sh first" >&2
  exit 1
fi
install -m 755 "$build/wslg-bridge" "$HOME/.local/bin/wslg-bridge"
install -m 755 "$root/scripts/omarchy-wslg" "$HOME/.local/bin/omarchy-wslg"
install -m 755 "$root/scripts/omarchy-wsl-session-init" "$HOME/.local/bin/omarchy-wsl-session-init"
install -d "$HOME/.config/hypr"
install -m 644 "$root/scripts/hyprland-wsl.lua" "$HOME/.config/hypr/hyprland-wsl.lua"
