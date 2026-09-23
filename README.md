# wslg-protocol-bridge

Wayland protocol bridge that lets an unchanged Hyprland talk to WSLg. Hyprland and Aquamarine see a parent with the protocols they require. The bridge copies their dma-buf frames into shared-memory buffers that stock WSLg Weston can show.

The desktop entry point is `omarchy-wslg`.

## Launch

From the `Omarchy-Desktop` distro, with `WAYLAND_DISPLAY` set to the WSLg socket (`wayland-0`):

```bash
omarchy-wslg
```

Force the Windows window to fill the screen:

```bash
omarchy-wslg --fullscreen
```

The same switch is `OMARCHY_WSLG_FULLSCREEN=1`. The bridge sends `xdg_toplevel.set_fullscreen` to WSLg before the first commit.

`WAYLAND_DISPLAY` must already be set. If it is empty, the launcher exits 2 and does not start Hyprland.

## What the session does

Hyprland's own display is the private socket `omarchy-wslg`. Desktop apps, including the terminal opened by Super+Return, use Hyprland's socket (`wayland-1` on this machine), not the bridge socket. When the session exits, the bridge writes the parent `WAYLAND_DISPLAY` (normally `wayland-0`) back into the systemd user manager.

## Layout

- `src/bridge.c` is the bridge.
- `tests/suite.c` and `tests/mock-parent.c` are the protocol suite.
- `qualify/qualify.c` is the VKMS GBM and EGL probe.
- `scripts/omarchy-wslg` is the launcher.
- `scripts/hyprland-wsl.lua` is the Hyprland entrypoint.
- `scripts/omarchy-wsl-session-init` starts the Omarchy shell without `uwsm start`.
- `scripts/build-and-test.sh` and `scripts/install-into-distro.sh` are the maintenance commands.
- `docs/` explains build, launch, and the kernel constraint.

## Build

Inside `Omarchy-Desktop`, from an ext4 build directory:

```bash
scripts/build-and-test.sh
```

See `docs/build.md`.
