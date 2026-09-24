# Kernel

Every WSL distro on this PC uses the custom kernel:

```
6.18.40.1-microsoft-standard-WSL2-omarchy-vkms1+
```

`%USERPROFILE%\.wslconfig` contains only:

```
[wsl2]
kernel=C:\\Users\\kingd\\source\\wsl-kernel\\vmlinux
```

`kernelModules` stays unset. The generated `modules.vhdx` makes WSL 2.9.12 fail while configuring networking. VKMS is built into the kernel, so the modules image is not required.

The uncompressed ELF is `C:\Users\kingd\source\wsl-kernel\vmlinux`. Paths in `.wslconfig` use doubled backslashes.

Rollback is deleting `.wslconfig` and running `wsl --shutdown`. That returns every distro to the stock Microsoft kernel and removes `/dev/dri/card0`.

After any boot, confirm:

```bash
uname -r
ls -l /dev/dri/card0
```

Pengwin, Ubuntu-24.04, the CLI distro `Omarchy`, `Omarchy-Desktop`, and docker-desktop must still boot after the kernel line is in place. Confirm each with `wsl.exe -d <name> -- uname -r`.
