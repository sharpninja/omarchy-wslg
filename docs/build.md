# Build and test

User install steps are in `docs/install.md`. This page is the build itself.

Build on ext4 inside `Omarchy-Desktop`, as user `omarchy`. The source tree can stay on the Windows drive. This machine uses `/home/omarchy/src/wslg-build` as the Meson build directory. A clone at `~/src/wslg-protocol-bridge` is the preferred source. The Windows checkout is `/mnt/c/Users/kingd/source/wslg-protocol-bridge`.

Packages already present in the desktop image: gcc, meson, ninja, wayland, wayland-protocols, libdrm, Mesa GBM, EGL, and GLES. Do not run `pacman -Syu`.

```bash
bash scripts/build-and-test.sh
```

That configures the build directory if needed, runs `ninja`, then `bridge-suite` and `qualify-drm`.

`bridge-suite` must print `FAILURES 0`. `qualify-drm` must print `PASS qualification`.

The suite needs `/dev/dri/card0` in the `video` group. If a new WSL boot creates the node as another group, restore it as root:

```bash
chgrp video /dev/dri/card0
chmod 660 /dev/dri/card0
```

## Install

```bash
bash scripts/install-into-distro.sh
```

Run that as `omarchy`, not root. It copies `wslg-bridge`, `omarchy-wslg`, `omarchy-wsl-session-init`, and `hyprland-wsl.lua` into that user's `~/.local/bin` and `~/.config/hypr`. From Windows, use the full launcher path in `docs/usage.md`. A non-interactive `wsl.exe` command does not search `~/.local/bin`.
