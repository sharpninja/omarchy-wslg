# Install

This repository installs the WSLg bridge into an existing `Omarchy-Desktop` distro. It does not build that distro and it does not replace the command-line `Omarchy` distro.

Run the commands below from Windows PowerShell. The Linux user is `omarchy`.

## 1. Confirm the distro

```powershell
wsl.exe -l -v
```

The list must include `Omarchy-Desktop`, version 2. On this PC its files live in `C:\Users\kingd\WSL\Omarchy-Desktop`.

## 2. Confirm the kernel

```powershell
wsl.exe -d Omarchy-Desktop -u omarchy -- uname -r
```

The line must contain `omarchy-vkms1`. The running kernel on this PC is `6.18.40.1-microsoft-standard-WSL2-omarchy-vkms1+`.

If `uname` does not contain `omarchy-vkms1`, `%USERPROFILE%\.wslconfig` needs the kernel line from `docs/kernel.md`. Saving that file is not enough. Run `wsl.exe --shutdown`, then start `Omarchy-Desktop` again. Shutdown stops every WSL distro on the machine.

Then confirm the device:

```powershell
wsl.exe -d Omarchy-Desktop -u omarchy -- ls -l /dev/dri/card0
```

`card0` must exist. The group should be `video`, mode `crw-rw----`. If the group is wrong:

```powershell
wsl.exe -d Omarchy-Desktop -u root -- chgrp video /dev/dri/card0
wsl.exe -d Omarchy-Desktop -u root -- chmod 660 /dev/dri/card0
wsl.exe -d Omarchy-Desktop -u root -- usermod -aG video omarchy
```

`usermod` applies on the next login. `chgrp` applies immediately.

## 3. Get the source

Inside the distro, on ext4:

```powershell
wsl.exe -d Omarchy-Desktop -u omarchy -- bash -lc "mkdir -p ~/src && git clone https://github.com/sharpninja/wslg-protocol-bridge.git ~/src/wslg-protocol-bridge"
```

A checkout that already exists at `C:\Users\kingd\source\wslg-protocol-bridge` is visible in the distro as `/mnt/c/Users/kingd/source/wslg-protocol-bridge`. Building on `/mnt/c` is slow. The build directory itself must stay on ext4. The scripts default to `/home/omarchy/src/wslg-build`.

## 4. Build and test

```powershell
wsl.exe -d Omarchy-Desktop -u omarchy -- bash -lc "cd ~/src/wslg-protocol-bridge && bash scripts/build-and-test.sh"
```

Use `cd /mnt/c/Users/kingd/source/wslg-protocol-bridge` instead when that is the checkout you are building.

`bridge-suite` must print `FAILURES 0`. `qualify-drm` must print `PASS qualification`. If `qualify-drm` prints `no vkms device`, go back to step 2.

## 5. Install the launcher

Still as `omarchy`, not root. The script writes to that user's home.

```powershell
wsl.exe -d Omarchy-Desktop -u omarchy -- bash -lc "cd ~/src/wslg-protocol-bridge && bash scripts/install-into-distro.sh"
```

That copies:

- `~/.local/bin/wslg-bridge`
- `~/.local/bin/omarchy-wslg`
- `~/.local/bin/omarchy-wsl-session-init`
- `~/.config/hypr/hyprland-wsl.lua`

## 6. Start it

```powershell
wsl.exe -d Omarchy-Desktop -u omarchy -- /home/omarchy/.local/bin/omarchy-wslg --fullscreen
```

A Windows window opens. See `docs/usage.md` for the daily commands and the failures `no vkms device` and `parent connect failed`.
