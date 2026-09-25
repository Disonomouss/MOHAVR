# Decisions

Each entry records why we went this way and not another. A **Proposed** decision is only
recorded here until evidence confirms or rejects it; then mark it **Decided** or **Rejected**,
with the reason and the date.

---

### D1. The mod targets the original, SteamStub-wrapped exe — Decided 2026-09-25
We never ship or rely on an unwrapped exe. Shipping one would mean distributing and replacing a
game binary, which breaks the rule "never modify game files" and Steam's verify-files step.
Because the stub doesn't encrypt code (ENGINE-NOTES §3), hooks at fixed VAs work in the wrapped
exe as well. The unwrapped copy in `work/` is for analysis and debugging only.

### D2. Everything that loads into the game is x86 — Decided 2026-09-25
MOHA.exe is 32-bit (ENGINE-NOTES §2). That covers the mod DLL, the hooking library and the
OpenXR loader, all built for `x86-windows` (vcpkg is at `C:\dev\vcpkg`). The OpenXR runtime for
tests is the x86 simulator build.

### D3. Test against a per-process OpenXR runtime, not the machine's — Decided 2026-09-25
`tools/run-with-openxr-sim.ps1` sets `XR_RUNTIME_JSON` for one process and picks the x86 or x64
build from the exe's PE header. The machine-wide runtime (Virtual Desktop, which also registers
a 32-bit manifest) stays untouched, and the real headset uses it.

### D4. Addresses are hard-coded, with a build check — Decided 2026-09-25
The exe has no ASLR and loads at `0x10900000` (ENGINE-NOTES §2), so fixed VAs are simpler and
more reliable than pattern scans. They are safe only because the mod checks the exe's SHA-256
(or size plus timestamp) at start-up, stands down on a mismatch, and verifies each hook's
prologue bytes.

### D5. Load through a `dinput8.dll` proxy in the game's `Binaries` folder — Decided 2026-09-25
*Confirmed in M1:* the proxy loads under the wrapped exe. Its `DllMain` runs on the main thread
at about 1 ms, and the game's first `Direct3DCreate9` call arrives about 730 ms later, through
our IAT hook. `tools/deploy.ps1` adds only MOHAVR's own files and restores the folder to its
baseline.

*Original reasoning:*
It adds a file rather than modifying one. MOHA imports `dinput8` (ENGINE-NOTES §5), so the
Windows loader maps the proxy before the SteamStub entry and before `WinMain`. That is early
enough to hook `Direct3DCreate9` and change the device parameters, which lessons §2 requires.
Alternatives: a `d3d9.dll` proxy (conflicts with D5a below), or an ASI loader.
To confirm: the proxy loads under the wrapped exe and its init runs before the first
`Direct3DCreate9` call.

### D5a. D3D9 → OpenXR through D3D9On12, with zero copies — Proposed
This follows the path proven in lessons §2: create the device with `Direct3DCreate9On12`, copy
each eye into a shared D3D12 texture, and signal a shared fence that the XR thread's D3D11 device
consumes. There is no D3D10 path to worry about (ENGINE-NOTES §5). Keep a system-memory readback
fallback. Risk: the 2 GB address-space limit (the exe is not large-address-aware). Measure how
much address space D3D9On12 and OpenXR use early.

### D7. The harness runs the game windowed from the command line — Decided 2026-09-25
It passes `-windowed ResX=… ResY=…` (and `-log`) rather than setting `StartupFullscreen` in an
ini, so the player's ini is never edited (standing rule 6). Windowed mode also gives an
IMMEDIATE presentation interval, so no vsync (ENGINE-NOTES §5b). Confirmed 2026-09-25: a
1920×1080 windowed client area, and nothing persisted to the user folder.

### D9. The harness detects progress from the window and screenshots, then from the mod's log — Decided 2026-09-25
Lessons §1 says to detect progress from log lines. MOHA's shipping build writes no log, even with
`-log` (ENGINE-NOTES §5c). Until M1 gives us our own log, the harness uses the process and window
state plus screenshot checks, for example the main-menu highlight (brightness 255 on the
selected item). Once the mod exists, it logs its own state transitions (menu, loading, in
gameplay), and the harness waits on those, as lessons §1 intends.

### D8. The harness backs up the player's whole user folder — Decided 2026-09-25
Before every test the harness copies `...\EA Games\Medal of Honor Airborne(tm)\Config\` and
`Saved\` (the save game and profile) into the project's `logs\backup\<timestamp>\`, and restores
them afterwards. The backup goes into the project, not beside the originals, because that folder
is under OneDrive and a copy there would sync. The game rewrites its user ini files on exit, so
restoring after the run, not only before it, is what keeps the player's settings intact
(lessons §1).

### D6. Ghidra runs headless by default — Decided 2026-09-25
`tools/start-ghidra-headless.ps1` serves the analyzed project to the `ghidra` MCP server with no
GUI, so analysis doesn't depend on someone opening Ghidra.
