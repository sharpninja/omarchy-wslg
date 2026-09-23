# Hostile validator receipt

TimestampUtc: 2026-09-23T22:22:15Z
ValidatorIdentity: GrokSubagentHostile
add-profile: executed yes. Profile files read: 19. Excluded add-profile.grok.md.
Work class: class 1, project implementation of the WSLg protocol bridge plus live Omarchy desktop verification. Surface C applies.
Requirements source: approved plan text. MCP requirements_list type fr returned zero items. That empty store is not a FAIL.
OverallVerdict: DISAGREE
Accuracy: 85
Completeness: 38
Counts: PASS 22, FAIL 8, UNKNOWN 3
Accuracy formula: 12 surface A claims PASS divided by 14 surface A claims = 85.7, recorded as 85. UNKNOWN claims are A7 and A10.
Completeness formula: 5 of 13 primary gates PASS, recorded as 38. PASS gates: AC-1, AC-4, goal suite, goal distros, goal qualify. Not pass: AC-2, AC-3, AC-5, goal live clean exit, TR-5 status, diagnostic-client test, unset WAYLAND_DISPLAY test, FR-5 docker and foreign WSLg app. Both scores are below 98, so OverallVerdict is DISAGREE.

## FAIL list

- B2: no machine receipt that WM_CLOSE cleared wslg-bridge, Hyprland, and foot.
- C2: AC-2 cursor setup and region add/subtract are not asserted.
- C3: AC-3 invalid bounds and DMA-BUF sync retry/failure are not tested.
- C5: AC-5 compositor crash and bridge failure were not exercised, and live window close is unproven.
- C6: TR-5 non-close transport break returns 0.
- C7: no diagnostic-client disconnect test.
- C8: no test that the launcher refuses when WAYLAND_DISPLAY is unset.
- D3: live-run checklist item is checked without a clean-exit receipt.

## UNKNOWN list

- A7: run 1 WM_CLOSE cleanup was not re-proven.
- A10: run 2 WM_CLOSE cleanup was not re-proven.
- C9: Docker container and other-distro WSLg app checks were not re-run.

## Claims

### A1 PASS surface A

Shipped suite passes twice with FAILURES 0, pixel checks, and super+return on the bridge seat.

Evidence: Re-ran in Omarchy-Desktop as omarchy: cd /home/omarchy/src/wslg-build && ninja && XDG_RUNTIME_DIR=/run/user/1000 ./bridge-suite ./mock-parent ./wslg-bridge twice. ninja: no work to do, exit 0. SUITE1_EXIT 0 and SUITE2_EXIT 0. Both stdout lines are FAILURES 0. ASSERT count 0 in both stderr files. OK count 38 in both. Both stderr files contain OK xrgb pixels, OK super+return on the bridge seat, OK premature reuse errors, OK bad modifier errors, OK bad plane errors. BIND lines are wl_compositor 4, wl_seat 7, xdg_wm_base 1. Historical suite-final-1 and suite-final-2 match that shape. Build uses /mnt/c/Users/kingd/source/wslg-protocol-bridge/src/bridge.c. Installed ~/.local/bin/wslg-bridge sha256 matches the build binary b341affbd9e19b613ccbbbfd30e31aebd760544734bf7f9bc3a47290ddfed7d6.

### A2 PASS surface A

qualify-drm prints PASS qualification for formats 875713112 and 875713089.

Evidence: Re-ran /home/omarchy/src/wslg-build/qualify-drm as omarchy. Exit 0. Line-buffered stdout: PASS readback format 875713112 stride 256, PASS format 875713112, PASS readback format 875713089 stride 256, PASS format 875713089, PASS qualification. A piped run omitted the child PASS readback lines because child_check prints then _exit, which drops a block-buffered stdout. The parent still requires child exit 0, so the readback check ran. Implementer qualify-drm.log matches the piped shape and does print PASS qualification for both formats. That log claim is true. AC-4 readback was confirmed on the line-buffered re-run.

### A3 PASS surface A

SIGUSR1 uses the same forward_key and forward_mods path as a parent chord, in the order key 125 press, mods 64, key 28 press and release, key 125 release, mods 0.

Evidence: bridge.c deliver_super_return calls up_key and up_mods, which are the parent keyboard listeners. up_key pends modifier key 125 until up_mods, then forward_key then forward_mods. Key 28 is not a modifier, so forward_key runs immediately. Resulting order is forward_key(125, press), forward_mods(64), forward_key(28, press), forward_key(28, release), forward_key(125, release), forward_mods(0). on_usr1 sets inject_chord and the loop calls deliver_super_return. This is source evidence, not a live protocol trace.

### A4 PASS surface A

That chord fired the installed SUPER+RETURN binding, which resolves to omarchy-launch-terminal, and foot was spawned.

Evidence: Installed applications.lua line 2 binds SUPER + RETURN to omarchy terminal. helpers.lua command_from prefixes omarchy-launch-. omarchy-launch-terminal execs uwsm-app -- xdg-terminal-exec. It does not exec foot by name. desktop-run processes.txt names foot and says the chord was delivered with SIGUSR1. Screenshots show a terminal prompt. Run 2 hyprland.log does not mention foot or the keybind. Live processes were not re-observed. Scored PASS on the artifact plus screenshots plus the installed binding, with that limit.

### A5 PASS surface A

Run 1 identity: Hyprland pid 5996 WAYLAND_DISPLAY=omarchy-wslg, foot pid 6606 WAYLAND_DISPLAY=wayland-1, signature efb50993780079460b0cbed1363e2166a2de1d9f_1790200592_519658127.

Evidence: desktop-run-1/processes.txt says exactly that, plus bridge pid 5994. The signature directory exists under /run/user/1000/hypr. Its hyprland.log is 0 bytes. ps -p 5994,5996,6606 now prints only the header. Live process list was not re-observed.

### A6 PASS surface A

Run 1 screenshots at 16:56 and 16:57 show a bar and a terminal prompt, and the later file has the later clock.

Evidence: Read both PNGs. Each shows a top bar, a clock, and a tilde prompt. Not blank and not a single color. Clocks Wednesday 16:56 and Wednesday 16:57. Both are 1024x576. UTC mtimes 2026-09-23T21:56:56Z and 2026-09-23T21:57:56Z. SHA256 FADE42C07B061C972D1FC97E25955C7B0301EA61592F6167B6FAAA1E62E89CE4 and 27432768C6363AB5EAD6B077741800FB470901DDE803C80C45056E6CFF01B11B.

### A7 UNKNOWN surface A

Run 1 WM_CLOSE then left no wslg-bridge, Hyprland, or foot.

Evidence: processes.txt is a during-run note, not a post-close listing. No scratch script records WM_CLOSE. /run/user/1000/omarchy-wslg.log is absent. Those three PIDs are absent now, which does not prove WM_CLOSE caused the exit. Live session was not relaunched.

### A8 PASS surface A

Run 2 identity: Hyprland pid 7067 WAYLAND_DISPLAY=omarchy-wslg, foot pid 7706 WAYLAND_DISPLAY=wayland-1, signature efb50993780079460b0cbed1363e2166a2de1d9f_1790200714_1598544106.

Evidence: desktop-run-2/processes.txt says exactly that, plus bridge pid 7065. bridge.out and hyprland.log for that signature both name the same instance. hyprland.log is 4984 bytes and ends during startup. ps -p 7065,7067,7706 now prints only the header. Live process list was not re-observed.

### A9 PASS surface A

Run 2 screenshots at 16:58 and 16:59 show a bar and a terminal prompt, and the later file has the later clock.

Evidence: Read both PNGs. Same kind of bar and tilde prompt, not blank. Clocks Wednesday 16:58 and Wednesday 16:59. Both 1024x576. UTC mtimes 2026-09-23T21:58:51Z and 2026-09-23T21:59:51Z. SHA256 52ED8C7A12674471E80E50DD561E124B9C03D813F4265E89C80C85A9ED745B92 and 4915CA2C49B3FB749A3D416768D19E874341CA455F4CF9FD2889F2D0431366A5.

### A10 UNKNOWN surface A

Run 2 WM_CLOSE then left no wslg-bridge, Hyprland, or foot.

Evidence: Same gap as run 1. The run 2 hyprland.log has no shutdown line. PIDs are absent now. Cause was not re-proven.

### A11 PASS surface A

Pengwin, Ubuntu-24.04, Omarchy, Omarchy-Desktop, and docker-desktop all reported the same uname.

Evidence: Re-ran wsl -d name -- uname -r for all five. Each printed 6.18.40.1-microsoft-standard-WSL2-omarchy-vkms1+ and EXIT 0. Did not run wsl --shutdown and did not change .wslconfig.

### A12 PASS surface A

Packages stayed hyprland 0.56.2-2 and aquamarine 0.15.0-2, and the distro-regression.log hashes still match.

Evidence: pacman -Q printed those versions. sha256 /usr/bin/Hyprland da8fcacf347bcbed83edc40108c6e2298da095e22246bd764e9bb382786cebb2. Both aquamarine libraries 3b418953be482141ddcdc8ae93da7e79bfeebf5ff8e6e8ae136fe8a46a1b7389. Those match distro-regression.log. Did not run pacman -Syu.

### A13 PASS surface A

Hyprland display is omarchy-wslg. foot display is wayland-1, not omarchy-wslg.

Evidence: Both processes.txt files say that. spawn_child sets the Hyprland child WAYLAND_DISPLAY to the listen name, default omarchy-wslg. Run 2 hyprland.log says DRM backend failed, Starting the Wayland backend, Output WAYLAND-1 initialized. Live env was not re-read from /proc.

### A14 PASS surface A

The implementer does not claim a prior hostile AGREE and does not claim the Windows synthetic Win key reaches WSLg.

Evidence: The goal plan deviation says SIGUSR1 is used because Windows drops a synthetic Win key. The HV checklist item is still unchecked. No earlier OverallVerdict AGREE is claimed. This review does not FAIL the absence of inter-phase HV.

### B1 PASS surface B

Suite, qualify, package, and uname numbers in the cited logs match a fresh re-run.

Evidence: No contradicted FAILURES count, hash, package version, or uname string. The piped qualify log omits PASS readback for the stdio reason; the line-buffered re-run printed it.

### B2 FAIL surface B

The live clean-exit claim has no machine receipt.

Evidence: processes.txt does not show a post-close process list. No saved command output shows WM_CLOSE followed by those PIDs gone. Current absence is not that receipt.

### B3 PASS surface B

Implementer scratch automation is shell, not Python.

Evidence: Reviewed scratch scripts are bash. This validator accidentally ran python3 -c print(1) once. That output was discarded and is not evidence. It is a validator mistake, not an implementer FAIL.

### B4 PASS surface B

No MCP storage file edit was observed. The bridge repo has no AGENTS-README-FIRST.yaml.

Evidence: Marker Test-Path was false for the bridge repo and for C:\Users\kingd\AGENTS-README-FIRST.yaml. sessionlog_query and requirements_list accepted this workspace and returned empty collections before this review. Empty MCP FR storage is not a FAIL. The approved plan text is the requirements source.

### B5 PASS surface B

Byrd phase-order is not failed from timestamps, and inter-phase HV was not claimed.

Evidence: red-release.err shows an earlier red run with ASSERT on dma-buf release and a double free. That is not a phase-gate receipt. No timestamp comparison was used as a FAIL.

### C1 PASS surface C

AC-1 holds: upstream binds stay on compositor 4, seat 7, shm 1, and xdg_wm_base 1.

Evidence: mock-parent errors versions above those caps. Fresh suite stderr shows only those versions. Mapping: AC-1 to mock-parent bind checks plus the passing suite.

### C2 FAIL surface C

AC-2 is not fully covered. Cursor setup and region add/subtract are not asserted.

Evidence: The mock client binds the five Aquamarine versions and asserts configure, pointer, one key, and axis_value120. set_input_region NULL is not region add/subtract. No set_cursor call is in the suite.

### C3 FAIL surface C

AC-3 pixel checks pass, but invalid bounds and the sync retry and sync-failure cases are not tested.

Evidence: Suite asserts stride, offset, alpha, resize, consecutive frames, bad modifier, bad plane, and premature reuse. No out-of-bounds expect. sync_buf retries EINTR and EAGAIN, and TR-4 assigns those cases to AC-3. suite.c has no such case.

### C4 PASS surface C

AC-4 live qualification passed, including second-process readback.

Evidence: Line-buffered qualify-drm printed both PASS readback lines and PASS qualification, exit 0, formats 875713112 and 875713089.

### C5 FAIL surface C

AC-5 is not fully exercised. Compositor crash and bridge failure were not run. Live window close is unproven.

Evidence: Screenshots and 60-second spacing support the visible shell. Suite covers parent close and sole-client disconnect. No crash, bridge-failure, or live WM_CLOSE receipt.

### C6 FAIL surface C

TR-5 does not return a documented nonzero status for a non-close transport break.

Evidence: bridge.c breaks when dispatch_pending is negative and then returns G.parent_closed ? 0 : 0. Both branches are 0. Child status and 128+signal return earlier. Init failure returns 2.

### C7 FAIL surface C

No test shows a diagnostic client disconnect leaving the session up.

Evidence: suite.c expects the only client disconnect to stop the bridge. Approved plan phase 4 requires the diagnostic case.

### C8 FAIL surface C

No test shows the launcher refusing when parent WAYLAND_DISPLAY is unset.

Evidence: omarchy-wslg does not check WAYLAND_DISPLAY. The bridge returns 2 if connect fails, but nothing asserts that. Installed launcher matches source.

### C9 UNKNOWN surface C

Docker container and other-distro WSLg app checks from the kernel rollout were not re-run.

Evidence: Goal verification uname and hashes passed. phase1-result.txt claims Pengwin DNS and /mnt/c. This review did not re-run those. Not a false claim and not a pass.

### D1 PASS surface D

Qualify checklist item matches a fresh PASS qualification.

Evidence: Checklist is checked. Re-run printed PASS qualification. This does not complete the goal.

### D2 PASS surface D

Test checklist item matches two fresh suite passes.

Evidence: Both re-runs: FAILURES 0, 0 ASSERT, 38 OK, including pixel and super+return lines.

### D3 FAIL surface D

The checked live-run item overclaims clean exit.

Evidence: Verification step 2 requires a clean exit after the window is closed. Screenshots and during-run notes exist. The clean-exit half does not. The checklist marks the whole item done.

### D4 PASS surface D

Launcher files match source, and the recorded terminal was not on the bridge socket.

Evidence: cmp reported LAUNCHER_MATCH, LUA_MATCH, and INIT_MATCH. processes.txt says foot WAYLAND_DISPLAY=wayland-1. The user bus was not re-queried.

### D5 PASS surface D

The hostile-validator checklist item is still open.

Evidence: goal plan line 39 is unchecked. This receipt is DISAGREE, so that item must stay open.

## Requirement mapping

- FR-1: omarchy-wslg execs the bridge. Screenshots show a window. Not relaunched here.
- FR-2: package versions and hashes match. Run 2 log shows the Wayland backend after DRM failed.
- FR-3: suite asserts pointer enter, motion, button, a modifier key, and axis_value120.
- FR-4: suite asserts parent close and sole-client disconnect. Live WM_CLOSE is UNKNOWN.
- FR-5: five uname checks PASS. Docker and a foreign WSLg app were not re-run.
- AC-1 through AC-5 are claims C1 through C5.
- TR-5 status, diagnostic disconnect, and unset display are C6, C7, and C8.

## Plan

Qualify, tests, and launcher file match are supported. The live checklist item is not fully supported. Hostile validation stays unchecked. The approved plan is not complete.

## Limits

Did not launch omarchy-wslg. Did not send a new SIGUSR1. Did not shut WSL down. Did not edit .wslconfig. Did not upgrade packages. Did not modify product source.
Screenshots are 1024x576, 0.8 times 1280x720. The run 2 log also records a later 1920x1080 swapchain. The images still show the bar and prompt.
