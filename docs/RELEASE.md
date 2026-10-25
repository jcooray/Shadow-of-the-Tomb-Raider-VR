# Release engineering

Status of the productionising work (docs/RELEASE_GOALS.md). Batch 1 (2026-10-07): game addresses by pattern and a
release build without the diagnostics.

## Build variants

| Build | Command | What it is |
|---|---|---|
| Development | `cmake -S . -B build` (default `VRMOD_DIAGNOSTICS=ON`) | Everything, including the diagnostic probes and their hotkeys. |
| Release | `cmake -S . -B build-release -DVRMOD_DIAGNOSTICS=OFF` | The 19 probe modules aren't compiled (list in CMakeLists.txt); their headers turn into inline no-ops (`src/build_config.h`), so core code is unchanged. About 360 KB smaller. |

Both: `cmake --build <dir> --config Release --target vrmod_dxgi_proxy` -> `<dir>/bin/Release/dxgi.dll`.

**Hotkeys (2026-10-19, done):** the release build reads only **Home** (first / third person; its Ctrl / Alt variants are
development keys), **K** (head aim), **J** (log mark) and **Ctrl+Alt+O** (the overlay), plus the gamepad combos (Menu +
Guide, LB + stick clicks). Every other hotkey in the core files reads its key through `VRMOD_DEV_KEY` (build_config.h):
GetAsyncKeyState in a development build, "not pressed" in a release build -- their settings are in the overlay. The
development build keeps every key.

## Game addresses

Nothing in the release build uses a fixed game address any more. `src/game_addresses.cpp` finds, at startup:

- **18 code/data addresses by byte pattern** in SOTTR.exe's `.text`: the event-post function, the four DLSS wrapper
  functions, the game-state / menus / movie checks, the camera record-copy function and the six head-aim call sites.
  A pattern must match exactly once, otherwise its address stays 0 and the feature using it logs that and stays off
  (no hook in a wrong place).
- **The event-id globals** (cutscene / traversal events) from the event names: name string in `.rdata` -> the LEA that
  loads it -> the register call after it -> the store of the returned id into its global.

Log at startup: `GAMEADDR: SOTTR.exe linked 0x64AC5C46 ... -- the build the mod was made for` (or `a DIFFERENT build`),
`GAMEADDR: 18 of 18 game addresses found by pattern (...)`, `GAMEADDR: 32 of 32 event ids found from their names`.

Still build-dependent (no pattern possible, checked at use): struct offsets inside game objects (camera component,
controllers, menus object +0x7A8, ...). Each is read with SEH guards and sanity checks; a different layout disables the
feature instead of crashing.

### Making patterns for another game build (Epic / GOG / a patch)

1. Import the new SOTTR.exe into Ghidra (a project of your own) and find the same functions (decompile by name /
   strings; the Ghidra addresses of the Steam build are in `kSigs` and the docs).
2. `analyzeHeadless ... -scriptPath tools/ghidra_sigs -postScript DumpForSigs.java tools/sig_input.txt code:<addr> ... data:<addr>`
   (Ghidra needs JAVA_HOME pointing at a JDK 21).
3. `python tools/make_sigs.py` -- builds each pattern (relative branch targets and RIP displacements wildcarded from
   Ghidra's references), grows it until it is unique in `.text`, and checks the match lands on the target.
4. `python tools/aob_check.py [SOTTR.exe]` -- checks the event-name resolver against the table in cinematic_watch.cpp.
5. Build `vrmod_gameaddr_test` and run it with the path to SOTTR.exe: the C++ resolver on the exe mapped as an image (nothing runs) -- every address
   and event id must be found.

Usually the existing patterns already match a new build (they avoid absolute addresses); steps 1-3 are only for those
reported missing.

## Verified offline (2026-10-07)

- `tools/make_sigs.py`: all 16 patterns unique in `.text`, each landing on its known address.
- `tools/aob_check.py`: all 32 event names resolve to the globals cinematic_watch.cpp used before.
- `vrmod_gameaddr_test`: the C++ resolver finds 18/18 (16 until 2026-10-18, + the two body-turn addresses) and the event ids at their known RVAs.
- Release and development builds compile.

Needs an in-game run: that the hooks installed through the resolver behave exactly as before (cutscene switching,
DLSS per-eye, menus, head aim), and a session on the release build.

## New-user defaults (2026-10-18)

A new install (no `vrmod_settings.ini`) works without editing anything:

- **First start** writes `vrmod_settings.ini` with every setting at its default and its comment (`SettingsFirstRun`,
  called from `dllmain` before anything reads the file), and shows a welcome message in the headset ("Hold Guide and
  press Menu for the settings") once the image is up.
- **Defaults:** render mode `aer` (alternate eye), render resolution 100 % (`resolution_pct`), overscan 1.08 (the
  standard preset), world at life size (`world_scale` 1 / 1.8 = World size 100 %, docs/OVERLAY.md "World size"), frame warp on, head aim and head drive on, Lara's head hidden
  in first person, the HUD smaller and centred in its own layer, view tones off.
- **The game's own settings** are set at startup, before the game reads them (`desktop_spoof.cpp`): its Stereoscopic 3D
  is switched off for alternate-eye / mono rendering (logged; for `sbs` the log asks the user to turn it on), and its
  fullscreen resolution follows Render resolution.
- **Render resolution on a first install:** the headset's target size is learned during the first start (the PHASE6
  line, saved as `render.target_w` / `target_h`), and the game only takes a resolution at startup -- so the first start
  runs at the game's own resolution. When the game's image doesn't match the chosen resolution, the headset shows
  "Restart the game once to render at W x H ..." (also after a SteamVR resolution change alters the target).
- `vrmod_fakedesktop.txt` is no longer needed; it is only used with `resolution_pct=0` ("as set in the game").

Messages in the headset: `VrOverlayNotify(text, seconds)` (vr_overlay.cpp) shows a banner in front of you with the
panel closed (in its help line when open); submitted through the panel's quad layer.

## For the user guide (collected as they come up)

- **SteamVR's render resolution setting doesn't change the game** (since 2026-10-26). SteamVR applies it (global, manual
  or Auto -- 150 % on an RTX 5090 -- times SOTTR's per-application value) to the size it recommends; the mod reads it
  from `Steam\config\steamvr.vrsettings` (steamvr_settings.cpp) and divides it back out of the recommended size, so the
  target is SteamVR's 100 % whatever SteamVR is set to, and the mod's Render resolution alone decides (before, the two
  multiplied: SteamVR 150 % with the mod at 150 % rendered ~225 %). The overlay's Status page shows SteamVR's value as
  "not used here". Other runtimes (VDXR, Oculus): no such setting; a quality preset that is part of the runtime's 100 %
  (Virtual Desktop's) stays in the target.

## Body turn by pattern (2026-10-18)

Body turn's two fixed addresses -- the world-object pool (RVA 0x301D0C0) and the player slot index (RVA 0x16B99C8) --
are now patterns 17 and 18 (`GameAddr::InstancePool` / `PlayerIndexGlobal`), made with the same tools
(`DumpForSigs.java` with `data:14301d0c0 data:1416b99c8` added; `make_sigs.py`): a LEA of the pool while the game walks
its entries (+0x110 / +0xD0 / +0xD8), and the index read right before `*(*(inst + 0xD8) + index * 8) + 0x2A8` -- the
same chain body turn uses to recognise Lara. The build-stamp check is gone; a missing pattern switches body turn off
(logged), and the struct offsets stay guarded by the instance <-> Player back-pointer check. Offline: 18/18 found at the
known RVAs (`vrmod_gameaddr_test`: ALL FOUND). Nothing in the release build uses a fixed game address.
