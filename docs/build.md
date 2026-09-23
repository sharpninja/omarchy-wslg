# Build and test

Build on ext4 inside `Omarchy-Desktop`. The source tree can stay on the Windows drive. This machine uses `/home/omarchy/src/wslg-build` as the Meson build directory and `/mnt/c/Users/kingd/source/wslg-protocol-bridge` as the source.

Packages already present in the desktop image: gcc, meson, ninja, wayland, wayland-protocols, libdrm, Mesa GBM, EGL, and GLES. Do not run `pacman -Syu`.

```bash
scripts/build-and-test.sh
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
scripts/install-into-distro.sh
```

This copies `wslg-bridge`, `omarchy-wslg`, `omarchy-wsl-session-init`, and `hyprland-wsl.lua` into the omarchy user's config and `~/.local/bin`.
