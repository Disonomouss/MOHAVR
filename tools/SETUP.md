# RE + VR toolchain

Set up 2026-09-25 by mirroring `C:\Users\j_tom\Projects\RDR2VR` (see its `tools\SETUP.md`).
Every repo is cloned at the **same commit** RDR2VR uses; every Python venv was rebuilt from a
freeze of RDR2VR's venv, so package versions match exactly.

## Target

| | |
|---|---|
| Game dir | `C:\Program Files (x86)\Steam\steamapps\common\Medal of Honor Airborne` (`tools\gamedir.txt`) |
| Executable | `UnrealEngine3\Binaries\MOHA.exe` — 15.2 MB |
| Arch | **32-bit x86** (PE machine 0x14C) — Unreal Engine 3, 2007, Direct3D 9 |

The 32-bit target is the main difference from RDR2VR (x64). It affects:

- **x64dbg**: the target needs **x32dbg**. `.mcp.json` sets `X64DBG_PATH` to `x96dbg.exe`; the
  Automate MCP reads the target's PE header and picks `x32\x32dbg.exe` itself.
- **Ghidra**: imports as `x86:LE:32:default`. RDR2VR's `ghidra_scripts\ApplyPdataFunctions.java`
  was not copied — 32-bit PEs have no `.pdata`.
- **Cheat Engine**: attach normally; the 64-bit CE handles 32-bit processes.

## What is where

Shared installs (one copy, used by both projects):

| Component | Location | Version |
|---|---|---|
| Ghidra | `C:\Users\j_tom\Tools\ghidra_12.1.2_PUBLIC` | 12.1.2 (GhidraMCP extension already deployed in `%APPDATA%\ghidra`) |
| x64dbg / x32dbg | `C:\Users\j_tom\Tools\x64dbg\release` | launcher `x96dbg.exe` |
| Cheat Engine | `C:\Program Files\Cheat Engine` | 7.7 |
| JDK 21 | `C:\Program Files\Eclipse Adoptium\jdk-21.0.12.101-hotspot` | |
| Maven | `C:\Users\j_tom\Tools\apache-maven-3.9.16` | |
| uv, Rust, CMake, VS C++ | on PATH | |
| x64dbg-skills plugin | user scope (`~/.claude/settings.json`) | `/state-snapshot`, `/decompile`, … already available |

Per-project, in `tools\`:

| Component | Commit | Notes |
|---|---|---|
| `cheatengine-mcp-bridge` | 6bd7ce9 | venv: Python 3.13, `mcp` 1.30.0 |
| `ghidra-mcp` | 1e02113 | venv via `uv sync`; `.env` points at this project's Ghidra project |
| `Ghidrust` | d2c42b6 | `cargo build --workspace --release -j 4` — the default parallelism crashed rustc on the `windows` crate (`STATUS_HEAP_CORRUPTION`, not memory); `-j 4` built cleanly in 1m34s. RDR2VR's copy only ever produced `ghidrust-netcap.exe`, probably the same failure |
| `OpenXR-Simulator` | 8de3457 | built → `bin\openxr_simulator.dll`; mcp-server venv `mcp` 1.30.0 (pinned `<2`) |
| `Vulkan-Headers` | ee2ec5f | headers only, for the simulator build |
| `x64dbg-skills` | 0409f53 | source for the user-scope plugin |
| `x64dbg-env\.venv` | — | `x64dbg_automate[mcp]`, angr 9.3.4, yara-python 4.5.4, lief 1.0.0, unicorn 2.1.4 |

`ghidra-mcp\uv.lock` shows as modified after `uv sync` — same as in RDR2VR; harmless.

## MCP servers

Registered in `..\.mcp.json` (project scope), enabled in `..\.claude\settings.local.json`.
Handshake-verified on 2026-09-25:

| Server | Tools | Talks to |
|---|---|---|
| `ghidra` | 35 → ~273 | GhidraMCP plugin in a running Ghidra, `http://127.0.0.1:8089` (full tool set appears once Ghidra is up) |
| `cheatengine` | 175 | Lua bridge in a running Cheat Engine, pipe `CE_MCP_Bridge_v99` |
| `x64dbg` | 48 | x64dbg Automate session (x32dbg for MOHA.exe) |
| `openxr-simulator` | 20 | simulator logs/screenshots in `%LOCALAPPDATA%\OpenXR-Simulator` |

All four are **bridges** — their tools do nothing useful until the app on the other end runs.
Ghidrust is used via its CLI and the `ghidrust` skill.

> **`mcp` must stay `<2`.** mcp 2.x removed `mcp.server.fastmcp`. If you re-run
> `pip install -r requirements.txt` in `OpenXR-Simulator\mcp-server`, re-pin:
> `uv pip install --python .venv/Scripts/python.exe "mcp>=1.28,<2"`

## Using it

### Ghidra
Project: `..\ghidra-projects\MOHAVR.gpr`, with `MOHA.exe` imported and auto-analyzed headless
(log: `..\ghidra-projects\import-MOHA.log`) — 32,865 functions, image base `0x10900000`.

**Headless (no GUI) — the default way to feed the `ghidra` MCP server:**
```powershell
tools\start-ghidra-headless.ps1          # foreground; Ctrl+C to stop
```
Runs `com.xebyte.headless.GhidraMCPHeadlessServer` (bundled in the deployed GhidraMCP jar)
via Ghidra's `support\launch.bat`, opens `MOHAVR.gpr`, loads `/MOHA.exe`, and listens on
127.0.0.1:8089 within a few seconds. The bridge then exposes 260 tools (verified: decompile
works). It holds the project lock and port 8089, so the GUI can't use the project meanwhile.
Edits made through the API are only persisted when `save_program` is called.

**GUI instead:**
1. Launch `C:\Users\j_tom\Tools\ghidra_12.1.2_PUBLIC\ghidraRun.bat`, open `MOHAVR.gpr`, open `MOHA.exe`.
2. Health check: `curl http://127.0.0.1:8089/check_connection`

Only one project can hold the lock — close RDR2VR's project first (both use port 8089).

Headless re-import. Two traps: the `.bat` breaks on `Program Files (x86)` (pass the 8.3 short
path), and the default 2G heap runs out during stack analysis (`OutOfMemoryError: Java heap
space`, nothing saved) — set `GHIDRA_HEADLESS_MAXMEM`. For the GUI on this binary, raise
`MAXMEM` in `ghidraRun.bat` the same way.
```powershell
$short = (New-Object -ComObject Scripting.FileSystemObject).GetFile('<full path to MOHA.exe>').ShortPath
$env:GHIDRA_HEADLESS_MAXMEM = '16G'
& C:\Users\j_tom\Tools\ghidra_12.1.2_PUBLIC\support\analyzeHeadless.bat ..\ghidra-projects MOHAVR -import $short
```

Rebuild the plugin: `tools\rebuild-ghidra-mcp.ps1` (it is shared — this redeploys for RDR2VR too).

### Cheat Engine
1. Launch Cheat Engine, attach to `MOHA.exe`.
2. Ctrl+Alt+L (Lua Engine), paste `tools\load_ce_bridge.lua`, Execute.
3. Confirm: `[MCP v12.0.0] MCP Server Listening on: CE_MCP_Bridge_v99`

Uses `MOHAVR_ROOT` if set, else `C:\Users\j_tom\Projects\MOHAVR`. RDR2VR's bridge uses the
same pipe name — load only one at a time.

### x64dbg
`start_session` with `target_exe` = MOHA.exe; `X64DBG_PATH` resolves to x32dbg automatically.

The **x64dbg Automate plugin** v0.8.1 (compat `ghost_fungus`, matching client 0.9.2) is installed
in `release\x32\plugins` and `release\x64\plugins` (2026-09-25; RDR2VR benefits too — it lacked
it). Verified: x32dbg session on a 32-bit target, compat handshake, `RegDump32`, terminate.
If the client is ever upgraded and reports `Incompatible x64dbg plugin and client versions`,
update the plugin to the release whose tag suffix matches `x64dbg_automate.COMPAT_VERSION`.

## Game-specific tools (installed 2026-09-25)

| Tool | Location | Version | Verified |
|---|---|---|---|
| Steamless | `C:\Users\j_tom\Tools\Steamless` | 3.1.0.5 | identifies MOHA's stub; see below |
| UE Explorer | `C:\Users\j_tom\Tools\UE-Explorer\ue-explorer\UEExplorer.exe` | 1.6.2 (portable, .NET 4.8) | decompiles MOHA script (build `MoHA`) |
| UE Viewer (umodel) | `C:\Users\j_tom\Tools\umodel\umodel_64.exe` | gildor.org build | `-game=moha` lists packages |
| Unreal package decompressor | `C:\Users\j_tom\Tools\umodel\decompress\decompress.exe` | gildor.org | needed by UE Explorer |
| apitrace | `C:\Users\j_tom\Tools\apitrace\apitrace-14.0-win32` (+ `-win64`) | 14.0 | 32-bit `d3d9.dll` wrapper present |

Archives are kept in `C:\Users\j_tom\Tools\_downloads`.

### SteamStub → `work\MOHA.exe.unpacked.exe`
MOHA.exe is wrapped in **SteamStub Variant 2.1 (x86)**; the entry point sits in `.bind`. The code
section is **not** encrypted (flags `0x26`, NoEncryption), so static analysis of the original is
fine — but Ghidra's auto-analysis started from the stub and never found the real startup path.

Steamless 3.1.0.5 crashes on it in step 5 (the stub's "code section VA" is garbage, `0x73E98FEA`,
so it links an invalid section and indexes -1). Workaround, same result as `Steamless --keepbind`:
```powershell
copy '<game>\UnrealEngine3\Binaries\MOHA.exe' work\
C:\Users\j_tom\Tools\Steamless\Steamless.CLI.exe --dumppayload --dumpdrmp work\MOHA.exe   # crashes after dumping
python tools\unwrap_steamstub21.py work\MOHA.exe                                        # -> work\MOHA.exe.unpacked.exe
```
The script uses Steamless's own offset logic to read the OEP from the decoded payload and only
rewrites `AddressOfEntryPoint` (and zeroes the checksum). The game install is never touched.

| | VA |
|---|---|
| Stub entry (`.bind`) | `0x1182D2ED` |
| Original entry (`entry_OEP`) | `0x1112B7EA` — `call __security_init_cookie; jmp __tmainCRTStartup` |
| `__tmainCRTStartup` | `0x1112B529` |
| `WinMain` | `0x10918200` |

These three are created/named in the Ghidra project and saved.

### UnrealScript (UE Explorer)
MOHA's cooked packages (`MOHAGame\CookedPC\*.xxx`, UE3 v421 / licensee 11) are LZO-compressed;
UE Explorer parses the header but loads 0 objects until they are decompressed:
```powershell
C:\Users\j_tom\Tools\umodel\decompress\decompress.exe -game=moha -out=work\decompressed '<CookedPC>\Engine.xxx'
```
Already decompressed into `work\decompressed`: `Core`, `Engine`, `GameFramework`, `MOHAGame`
(85 MB). Open those in UE Explorer. Verified headless via `Eliot.UELib.dll`: Engine.xxx → 20,235
objects / 3,270 functions; `PlayerController.GetPlayerViewPoint` decompiles (→
`PlayerCamera.GetCameraViewPoint`, the VR camera path). Native operators show as `__NFUN_nnn__`.

### Assets (umodel)
```powershell
C:\Users\j_tom\Tools\umodel\umodel_64.exe -game=moha -path='<CookedPC>' -list Engine.xxx
```
Reads compressed packages directly.

### D3D9 frame capture (apitrace)
Use the **win32** build for MOHA. Put `apitrace-14.0-win32\lib\wrappers\d3d9.dll` next to the exe
being traced (or use `bin\apitrace.exe trace`), then inspect with `qapitrace` / replay with
`d3dretrace`. PIX and RenderDoc do not support D3D9.

### Ghidrust
```
tools\Ghidrust\target\release\ghidrust.exe help
tools\Ghidrust\target\release\ghidrust-gui.exe
```

### OpenXR Simulator
Two builds, because a 32-bit process cannot load a 64-bit runtime:

| Build | Runtime | For |
|---|---|---|
| `OpenXR-Simulator\bin` | x64 | 64-bit apps, the upstream tests |
| `OpenXR-Simulator-x86\bin` | **x86** | **MOHA.exe** — separate checkout, same commit (CMake hard-codes output to `<src>\bin`) |

Rebuild: `tools\rebuild-openxr-simulator.ps1` / `tools\rebuild-openxr-simulator.ps1 -Arch x86`.
The x86 build adds a linker `/EXPORT` alias: `XRAPI_CALL` is `__stdcall` on x86, so the entry
point otherwise exports only as `_xrNegotiateLoaderRuntimeInterface@8`, which the OpenXR loader
cannot find by name.

Smoke test, both bitnesses (loads the DLL like the loader, negotiates, `xrCreateInstance`,
`xrGetSystem`): `tools\openxr-sim-smoke\build.ps1` — ALL PASS on 2026-09-25.

Not registered machine-wide (your runtime is Virtual Desktop, which registers both a 64-bit and
a 32-bit manifest). Per-process — the script picks x86/x64 from the exe's PE header:
```powershell
.\tools\run-with-openxr-sim.ps1 -Exe '<path to exe>'
```

Graphics: the simulator implements D3D11, D3D12, OpenGL and Vulkan bindings. OpenXR has no
D3D9 binding, so the mod will have to hand frames from MOHA's D3D9 device to a D3D11 device
(e.g. D3D9Ex shared surfaces) before submitting.

### Game-driving helpers
Copied from RDR2VR and retargeted to `MOHA`: `screenshot.ps1`, `capture-window.ps1`,
`focus-game.ps1`, `sendkey.ps1`, `click.ps1`, `look.ps1`. Their comments still describe what
was measured on RDR2 (menu coordinates, raw-input behaviour); re-verify against MOHA.

## Security notes

- Ghidra's HTTP server binds 127.0.0.1, unauthenticated on loopback. `/run_script` is off unless
  `GHIDRA_MCP_ALLOW_SCRIPTS` is set in `tools\ghidra-mcp\.env`.
- The CE named pipe grants full read/write over the attached process to any local process.
  Load the bridge only while you need it.
- MOHA ships PunkBuster (`MOHAGame\pb`). Keep all debugging and injection to single-player/offline.
