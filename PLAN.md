# Plan

The checklist for the current milestone. An unattended session can work through it top to
bottom. Tick items as they're done, adding a one-line result. A blocked item stays unticked
and gets marked **BLOCKED**, with the reason (time-box research at about two hours). Stop and
ask the user before any step that touches the game folder, launches the game for the first
time, or needs the headset.

## M0 (headless test rig), plus the research that M1 and M2 need — DONE

### Needs the user's go-ahead first
- [x] First launch (done by the user). The user config and saves are under OneDrive
      Documents (ENGINE-NOTES §7).
- [x] First `-windowed ResX= ResY= -log` launch. Windowed 1920×1080 works. **No engine log**
      (a console opens but stays empty), so the harness uses screenshots (D9). Main menu reached,
      SendInput works in menus, WM_CLOSE works, and the user folder is unchanged (ENGINE-NOTES §5c).
- [x] Decide how the game runs windowed. The command line (`-windowed ResX= ResY=`) never
      touches the player's ini (D7). Still to confirm on first launch.

### Static research (can run unattended; no game launch)
- [x] Script dump and camera search. All 2,483 classes are in `work/script/`
      (`tools/dump-script.ps1`). The view chain is mapped, and a stock split-screen lead was
      found (ENGINE-NOTES §6).
- [x] Control table recorded in ENGINE-NOTES §7. Gamepad bindings live in script
      (`MOHAPlayerInput.uc`), not ini.
- [x] `Direct3DCreate9`, `CreateDevice` and the presentation parameters: CreateDevice at
      `0x1090339A`, and windowed mode means no vsync (ENGINE-NOTES §5b).
- [x] DirectInput and XInput: mouse and keyboard are DirectInput with buffer sizes set;
      XInput is imported by ordinal (ENGINE-NOTES §5a).
- [ ] ~~In Ghidra: find where the log file is opened~~. Replaced: observe the log location on
      the first launch; that's cheaper and definitive.

### Harness (after the first launch)
- [x] `tools/harness.ps1`: launch, wait, key, shot, mem, ingame, to-gameplay, quit (with a kill
      fallback and Steam cooldown), and restore. The player's Config and Saved are backed up and
      restored every run (D8).
- [x] SendInput reaches the menus **and gameplay** (mouse-look turns the camera).
- [x] Front-end walk to live gameplay from the save, driven by screenshot checks (D9).
- [x] Screenshots: `capture-window.ps1` (PrintWindow) works for the D3D9 window.
- [x] **M0 acceptance: 4 of 4 consecutive `cycle` runs OK, about 30 s each.**

## M1 (the mod loads and stands down safely) — DONE 2026-09-25
- [x] CMake x86 `dinput8.dll` proxy forwarding all six exports (`tools/build.ps1`, static CRT,
      /W4 clean).
- [x] `MOHAVR.log` and `MOHAVR.ini` beside the DLL (module-handle path), with the previous log
      kept as `.prev`.
- [x] Build check from the in-memory PE header plus 4 signatures (D4); `Debug.TestWrongBuild`
      proves stand-down.
- [ ] ~~Hook library~~. Moved to M2: M1 needed only a verified IAT swap (`patch.cpp`).
- [x] `Direct3DCreate9` IAT hook, verified before the swap. Reached about 730 ms after init.
- [x] `tools/deploy.ps1` deploy, undeploy and status (with the user's OK; baseline-verified).
- [x] Harness `wait-log`, and every run's mod log is kept in `logs/modlogs/`.

## Next: M2 (D3D9 → OpenXR bridge, mono)
- [x] **Address-space budget:** `Bridge.D3D9On12=1` works and costs +137 MB, leaving 555 MB
      free with a largest block of 331 MB (ENGINE-NOTES §5e, `tools/vmmap.py`,
      `tools/measure-variant.ps1`). Enough to proceed.
- [x] Device hooks for diagnosis: `IDirect3D9::CreateDevice` (slot 16), `Present` (17),
      `CreateAdditionalSwapChain` (13), all as verified vtable swaps. The game presents via the
      device on its render thread.
- [x] Mod-side backbuffer capture (`frame_capture.cpp`, event `Local\MOHAVR_Capture` →
      `%TEMP%\MOHAVR\capture.bmp`). The harness prefers it, so screen checks work under 9On12.
- [ ] Time-box (≤ 2 h): why the window stays white under 9On12 (ENGINE-NOTES §5e). Not blocking.
- [ ] Hook library for x86 (vcpkg `safetyhook` or `minhook`, x86-windows) with prologue checks.
      Not needed yet: vtable swaps have covered everything so far.
- [ ] `CreateDevice`: force a windowed, headset-sized backbuffer (vsync is already off in
      windowed mode, ENGINE-NOTES §5b).
- [x] OpenXR in the process: static x86 loader (vcpkg manifest, `x86-windows-static`), a D3D11
      device on the runtime's adapter, a session and a test-pattern quad. **Works** against the
      x86 simulator selected by `OpenXR.RuntimeJson` (Steam-launched processes don't inherit our
      environment, so the mod sets `XR_RUNTIME_JSON` itself).
- [x] Measured: 9On12 + XR leaves 297 MB free, largest block 132 MB, which is **too tight**.
      **D10 decided: OpenXR in a 64-bit host process.**
- [x] **Out-of-process bridge** (D10): the game publishes frames into shared D3D12 textures and
      fences; `MOHAVR-host.exe` (x64, `src/host`) runs OpenXR and shows them on a world-locked
      quad. The game's frame is visible in the simulator; the game keeps 505 MB free.
      `tools/build.ps1` builds both, and `tools/deploy.ps1` ships both.
- [x] **[H] headset check PASSED** (HEADSET-TESTS round 1, attempt 2): Quest 3 via Virtual
      Desktop, 90 Hz, stable, smooth, correct colours.
- [ ] Desktop mirror window in the host (the game's own window is white under 9On12).
- [ ] Quad placement: the LOCAL origin puts it low in the simulator. Recenter or use VIEW-based
      placement at session start.

## Current: M3 (head tracking and per-eye projection, mono)
Design (ENGINE-NOTES §5g):
- **Host → game** (shared block v2, seqlock): each XR frame, `xrLocateViews` at the predicted
  display time gives the head pose (VIEW in LOCAL) and both eyes' FOV tangents.
- **Game, game thread:** a safetyhook MidHook at `0x10C19B3C` in `CalcSceneView` gives the final
  ViewRotation = **game yaw ∘ head orientation** (the game's own pitch and roll are dropped;
  lessons §3). ViewLocation += yaw-rotated head position × `Camera.UnitsPerMeter`.
- **Game, projection:** MidHooks at `0x10C19EAB` and `0x10C19DAF` overwrite the matrix at [EAX]
  with an asymmetric perspective. Mono uses the union of both eyes' FOV, widened horizontally to
  the viewport's aspect (so no backbuffer resize is needed yet). Near plane stays at 5.0.
- **Pose/image pairing:** each published frame carries the pose and FOV it was rendered with.
  Render-thread lag is detected (the Present thread differs from the CalcSceneView thread → use
  the previous view).
- **Host:** a projection layer (both eyes get the same image, pose and FOV = the frame's own),
  replacing the quad. The runtime reprojects.
- Every piece behind `[Camera]` switches; the prologue and signature bytes are verified before
  each MidHook (standing rule 4).

Steps:
- [x] RE: CalcSceneView, GetPlayerViewPoint, FPerspectiveMatrix and hook sites (ENGINE-NOTES §5g).
- [x] safetyhook 0.7 (vcpkg, x86-static, C++23). MidHooks installed after the call-target and
      signature checks.
- [x] Shared block v2: views from the host (seqlock), per-slot render pose/FOV from the game.
- [x] Game: the view and projection hooks; pose pairing; translation recentred on the first pose.
- [x] Host: xrLocateSpace(VIEW) + xrLocateViews, projection layer (2-slice swapchain), quad
      fallback.
- [x] [S] Simulator: yaw, pitch and roll drive the camera correctly (ENGINE-NOTES §5h), and the
      projection layer fills each eye at a natural perspective at 60 FPS.
- [ ] [H] Headset round 2: world-lock (no swim), scale, comfort.
- Later in M3: a recentre key; render only the needed FOV (the widened union wastes pixels); the
  weapon and HUD follow in M5/M7.
- [ ] Per-frame copy of the backbuffer into a shared texture, then the XR thread's swapchain,
      shown on a world-locked quad first.
