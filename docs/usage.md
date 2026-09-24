# Usage

Start and stop the desktop from Windows. The Linux user is `omarchy`.

## Start

Fullscreen, from PowerShell:

```powershell
wsl.exe -d Omarchy-Desktop -u omarchy -- /home/omarchy/.local/bin/omarchy-wslg --fullscreen
```

Windowed:

```powershell
wsl.exe -d Omarchy-Desktop -u omarchy -- /home/omarchy/.local/bin/omarchy-wslg
```

The same switch inside the distro is `omarchy-wslg --fullscreen`, or `OMARCHY_WSLG_FULLSCREEN=1` with either command. `--fullscreen` asks WSLg to fill the primary screen. On this PC that is 1536x864 at 0,0.

Windows Terminal has a profile named `Omarchy-Desktop`. Open it and run:

```bash
omarchy-wslg --fullscreen
```

That profile is a shell. It does not start the desktop by itself.

## What you should see

One Windows window. Its title looks like `aquamarine - WAYLAND-1 (Omarchy-Desktop)`. The Omarchy bar is at the top. Press the Windows key and Enter in that window to open a terminal. The terminal belongs to Hyprland. Hyprland's own display is `wayland-1` on this PC. The bridge socket, `omarchy-wslg`, is only the parent Hyprland connects to.

`WAYLAND_DISPLAY` in the shell that launches the desktop must be `wayland-0`. A normal `wsl.exe -d Omarchy-Desktop` session sets that. When the desktop exits, the bridge writes `wayland-0` back into the systemd user manager.

## Stop

Close the desktop window. The bridge exits 0, and Hyprland and the terminal exit with it.

From a second PowerShell window:

```powershell
wsl.exe -d Omarchy-Desktop -u omarchy -- pkill -x wslg-bridge
```

## Do not use

- `start-hyprland`. It is a watchdog and can restart Hyprland.
- `uwsm start`. It needs a foreground VT. WSL does not have one. Super+Return still uses `uwsm-app`, which is a different program.
- `pacman -Syu` in `Omarchy-Desktop`.

## When it fails

`no vkms device`

The kernel has no `/dev/dri/card0`. Check `uname -r` for `omarchy-vkms1` and `ls -l /dev/dri/card0`. Fix `.wslconfig` as in `docs/kernel.md`, run `wsl.exe --shutdown`, and start the distro again. Shutdown stops every WSL distro.

`parent connect failed`, or the launcher exits 2 with `WAYLAND_DISPLAY is unset`

The launcher has no WSLg socket. Inside the distro, `echo $WAYLAND_DISPLAY` must print `wayland-0`. Start from `wsl.exe -d Omarchy-Desktop` or from the `Omarchy-Desktop` terminal profile. Do not unset `WAYLAND_DISPLAY` before `omarchy-wslg`.

`omarchy-wslg: command not found`

The non-interactive `wsl.exe` command does not search `~/.local/bin`. Use the full path `/home/omarchy/.local/bin/omarchy-wslg`, or run `bash scripts/install-into-distro.sh` again as `omarchy`.

`card0` exists but the desktop or the test suite dies immediately

The device group is not `video`, or `omarchy` is not in `video`. As root:

```powershell
wsl.exe -d Omarchy-Desktop -u root -- chgrp video /dev/dri/card0
wsl.exe -d Omarchy-Desktop -u root -- chmod 660 /dev/dri/card0
wsl.exe -d Omarchy-Desktop -u root -- usermod -aG video omarchy
```

Open a new distro session after `usermod`.
