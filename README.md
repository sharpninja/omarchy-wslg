# wslg-protocol-bridge

Wayland protocol bridge that lets an unchanged Hyprland talk to WSLg. Hyprland and Aquamarine see a parent with the protocols they require. The bridge copies their dma-buf frames into shared-memory buffers that stock WSLg Weston can show.

Install into the existing `Omarchy-Desktop` distro, then start the desktop from Windows.

## Install

Follow `docs/install.md`. The short form, after the kernel and `/dev/dri/card0` checks in that page:

```powershell
wsl.exe -d Omarchy-Desktop -u omarchy -- bash -lc "cd ~/src/wslg-protocol-bridge && bash scripts/build-and-test.sh && bash scripts/install-into-distro.sh"
```

Clone the repo to `~/src/wslg-protocol-bridge` first if that directory is missing. The build directory must be on ext4. See `docs/build.md`.

## Use

Fullscreen:

```powershell
wsl.exe -d Omarchy-Desktop -u omarchy -- /home/omarchy/.local/bin/omarchy-wslg --fullscreen
```

Windowed: drop `--fullscreen`. Inside an `Omarchy-Desktop` terminal, `omarchy-wslg --fullscreen` is the same command. `OMARCHY_WSLG_FULLSCREEN=1` is the same switch.

Close the desktop window to stop it. Press the Windows key and Enter in that window for a terminal. Details, and the fixes for `no vkms device` and `parent connect failed`, are in `docs/usage.md`.

## Layout

- `src/bridge.c` is the bridge.
- `tests/suite.c` and `tests/mock-parent.c` are the protocol suite.
- `qualify/qualify.c` is the VKMS GBM and EGL probe.
- `scripts/omarchy-wslg` is the launcher.
- `scripts/hyprland-wsl.lua` is the Hyprland entrypoint.
- `scripts/omarchy-wsl-session-init` starts the Omarchy shell without `uwsm start`.
- `scripts/build-and-test.sh` and `scripts/install-into-distro.sh` build, test, and install.
- `docs/install.md` and `docs/usage.md` are the user instructions.
- `docs/kernel.md` is the `.wslconfig` kernel line.
