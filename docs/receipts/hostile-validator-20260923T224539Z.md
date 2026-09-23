# Hostile validator receipt

TimestampUtc: 2026-09-23T22:45:39Z
ValidatorIdentity: GrokSubagentHostile
add-profile: executed yes. Profile files read: 19. Excluded add-profile.grok.md.
Work class: class 1, project implementation. Surface C applies. Requirements source remains the approved plan text. Empty MCP FR storage is not a FAIL.
Prior receipt: hostile-validator-20260923T222215Z.md, DISAGREE, accuracy 85, completeness 38. Scores were raised only after the failed and unknown items were re-run or re-read.
OverallVerdict: AGREE
Accuracy: 100
Completeness: 100
Counts: PASS 13, FAIL 0, UNKNOWN 0
Accuracy formula: 13 of 13 re-checked claims PASS, recorded as 100. Completeness formula: the prior incomplete gates (suite coverage, close receipt, crash and SIGTERM, TR-5 status, diagnostic client, unset display, distro WAYLAND_DISPLAY, and the unchanged package and screenshot passes) are 13 of 13 PASS, recorded as 100. Both scores are at least 98.

## FAIL list

None.

## UNKNOWN list

None.

## Claims

### R1 PASS surface A

Fresh bridge-suite exits 0 with FAILURES 0 and the new OK lines.

Evidence: Omarchy-Desktop as omarchy, /home/omarchy/src/wslg-build, ninja no work, exit 0. SUITE_EXIT 0. Stdout FAILURES 0. Stderr ASSERT count 0 and 49 OK lines. Present: OK super+return on the bridge seat, OK cursor and region requests, OK diagnostic disconnect keeps the bridge, OK invalid bounds errors, OK sync failure errors, OK sync retry still publishes, OK window close exits 0, OK parent loss exits 2, OK unset WAYLAND_DISPLAY refuses.

### R2 PASS surface A

suite-final-1 and suite-final-2 match that result.

Evidence: Both .out files are FAILURES 0. Both .err files contain the same OK lines and no line starting with ASSERT.

### R3 PASS surface A

desktop-run-close.txt is a window-close receipt: BEFORE the three processes, EXIT 0, AFTER NONE.

Evidence: File order is BEFORE, bridge 14283, Hyprland 14285, foot 14510, READY, EXIT:0, AFTER, NONE. ps -p 14283,14285,14510 printed only the header. Current absence is consistent and is not the cause proof. The file order is the cause evidence. This reviewer did not send WM_CLOSE.

### R4 PASS surface A

Compositor SIGKILL yields bridge_exit 137 and bridge SIGTERM yields 143, then no leftover processes.

Evidence: ac5-lifecycle.log: bridge 11963 hypr 11965 bridge_exit=137, then bridge 12644 hypr 12646 bridge_exit=143, followup pgrep NO_BRIDGE NO_HYPR NO_FOOT. The indented bash -lc line is the script command matching itself. Current pgrep -x wslg-bridge, Hyprland, and foot printed NO_BRIDGE, NO_HYPR, NO_FOOT. This reviewer did not send those signals.

### R5 PASS surface C

TR-5 no longer returns 0 for every loop exit.

Evidence: bridge.c on_sig stores the signal and clears G.run. After the loop, got_signal returns 128+signal, parent_closed returns 0, a signaled child returns 128+signal from the loop, and the remaining fallthrough returns 2. Suite OK window close exits 0 and OK parent loss exits 2. The old both-branches-0 return is gone.

### R6 PASS surface C

A diagnostic client disconnect keeps the bridge. The destroy hook is first-client only.

Evidence: suite.c connects a second client, disconnects it, and expects waitpid WNOHANG 0. Re-run printed OK diagnostic disconnect keeps the bridge. bind_global sets client_hooked and adds client_gone only for the first client. client_gone sets compositor_gone.

### R7 PASS surface C

omarchy-wslg exits 2 when WAYLAND_DISPLAY is empty, and the suite checks the installed script.

Evidence: scripts/omarchy-wslg lines 13-16 test -z and exit 2. Installed ~/.local/bin/omarchy-wslg cmp MATCH. Suite execl of that path with the variable unset got OK unset WAYLAND_DISPLAY refuses.

### R8 PASS surface C

AC-2 cursor and region requests and AC-3 bounds, sync failure, and sync retry are in the passing suite.

Evidence: suite.c issues set_cursor, region add, and region subtract, then expects no protocol error. Invalid bounds uses stride 4 for a 32x32 buffer. params_ok posts OUT_OF_BOUNDS when stride is below width*4. Sync failure uses a non-dma-buf memfd and expects a protocol error. WSLG_SYNC_EINTR injects one EINTR in sync_buf, then the real ioctl runs, and the suite expects the frame. Re-run printed the matching OK lines, including sync retry pixels.

### R9 PASS surface A

All five distros print the VKMS uname and WAYLAND_DISPLAY=wayland-0.

Evidence: Re-ran Pengwin, Ubuntu-24.04, Omarchy, and Omarchy-Desktop with bash, and docker-desktop with sh. Each printed UNAME 6.18.40.1-microsoft-standard-WSL2-omarchy-vkms1+ and WAYLAND wayland-0, exit 0. This matches the bottom of distro-regression.log. Pengwin also printed the known vgem modprobe warning. Did not shut WSL down or edit .wslconfig.

### R10 PASS surface A

Package versions and hashes are unchanged, and the installed bridge matches the build.

Evidence: pacman -Q hyprland 0.56.2-2 and aquamarine 0.15.0-2. Hyprland sha256 da8fcacf347bcbed83edc40108c6e2298da095e22246bd764e9bb382786cebb2. libaquamarine.so.0.15.0 sha256 3b418953be482141ddcdc8ae93da7e79bfeebf5ff8e6e8ae136fe8a46a1b7389. Both match distro-regression.log. cmp of the build wslg-bridge and ~/.local/bin/wslg-bridge printed BRIDGE_MATCH. Did not run pacman -Syu.

### R11 PASS surface A

Prior screenshot and display-identity passes are not contradicted.

Evidence: The four PNGs were read in the prior review: bar, clock one minute later, tilde prompt, 1024x576, not blank. processes.txt still names Hyprland on omarchy-wslg and foot on wayland-1. This pass did not re-open those files as false. qualify-drm was not re-run this pass; the prior line-buffered PASS qualification still stands and nothing here changed the kernel or packages.

### R12 PASS surface D

The goal checklist live item now has a clean-exit receipt, and the hostile-validator box is still open.

Evidence: desktop-run-close.txt supplies the missing exit receipt. goal/plan.md line 39 is still unchecked. This receipt does not edit that file.

### R13 PASS surface B

No Python was used for this re-check. Receipts were serialized with ConvertTo-Json.

Evidence: Commands were PowerShell and wsl. No python, python3, or py invocation in this pass.

## Limits

Did not send WM_CLOSE, SIGKILL, or SIGTERM. Did not relaunch the desktop. Did not edit product source, the goal checklist, .wslconfig, or packages.
Sync retry is an injected EINTR through WSLG_SYNC_EINTR, then the real ioctl. The suite also checks the published pixels.
Cursor assertion is no protocol error after set_cursor and region add/subtract. It does not prove a cursor image was forwarded.
