# Launch

Daily commands and failure fixes are in `docs/usage.md`. Install is in `docs/install.md`.

`omarchy-wslg` is the only supported way to start the desktop.

```bash
omarchy-wslg
omarchy-wslg --fullscreen
OMARCHY_WSLG_FULLSCREEN=1 omarchy-wslg
```

`--fullscreen` asks WSLg to show the toplevel fullscreen. On this PC that is a window at 0,0 matching the primary screen (1536x864 when last measured).

The launcher refuses to start when `WAYLAND_DISPLAY` is unset:

```bash
env -u WAYLAND_DISPLAY omarchy-wslg
```

Exit status is 2.

## Environment on exit

`omarchy-wsl-session-init` imports the Hyprland process environment into the systemd user manager. That includes Hyprland's private `WAYLAND_DISPLAY`. On exit the bridge runs:

```bash
systemctl --user set-environment WAYLAND_DISPLAY=<parent display>
```

The parent display is the value the bridge itself used to connect to WSLg, normally `wayland-0`. Test launches (`--no-child`) do not change the user manager.

## Do not use

- `start-hyprland`. It is a watchdog and can restart Hyprland.
- `uwsm start`. It needs a foreground VT that WSL does not have.
- `pacman -Syu` on `Omarchy-Desktop`.
