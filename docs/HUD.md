# Workstream H — HUD and messages to the centre (Brief 2)

Status: **H0 accepted 2026-09-25**: the UI has its own render target and a stable pipeline signature (see "H0 results"). H1 next.

## H0 — Find the UI pass

**What's known going in:**
- The UI is the game's own Flash implementation (`gfx::flash::GImageLoader`, UiWidget, "Scaleform: movie(%s)").
- The exe has no safe-zone or HUD-scale setting (strings searched: safezone, safe_area, hudscale, uiscale; none
  found).
- The HUD is drawn in screen space into every frame, across the full 6368 × 4592 image (±56° × ±47°).

**The probe** (`src/ui_probe.cpp`, only with `[debug] ui_probe=1`; `vrmod_settings.ini.before_h0` is the previous
ini):
- Hooks `D3D12CreateDevice` at startup. Once the game's device exists, it hooks:
  - `CreateGraphicsPipelineState` (10) and `CreateRenderTargetView` (20), always recorded, since both happen at load;
  - `ExecuteCommandLists` (10);
  - on the command list: Close 9, Reset 10, DrawInstanced 12, DrawIndexedInstanced 13, RSSetViewports 21,
    RSSetScissorRects 22, SetPipelineState 25, OMSetRenderTargets 46, ClearRenderTargetView 48, BeginEvent 57,
    EndEvent 58.

  All slots were counted from the 10.0.26100.0 SDK `d3d12.h` C vtable structs.
- **Always recorded:**
  - each PSO's signature: render-target count and format, depth test/write, blend enable and factors, VS/PS
    bytecode sizes, a hash of the pixel shader, and the input-element count. Stable across sessions, unlike
    pointers;
  - each RTV descriptor's resource, size and format.
- **Ctrl+Alt+U** captures the command lists submitted over the next 3 frames to `vrmod_uiprobe_<time>.txt`. Each
  submitted list, in submission order, is split into passes: render target (size, format), viewport, draw count and
  index-count range, the PSOs used (as #ids, with their signatures listed), clears, and PIX event names if the game
  emits any.
- Outside a capture the hooks cost one atomic load each.
- **Offline smoke test** (`scratchpad/uitest`, a real D3D12 device on this machine): hooks install at device
  creation; an RTV maps to its 256×128 RGBA8 texture; the viewport, clear and PIX event "HUD pass" are recorded; the
  submitted list is reported, and a recorded-but-unsubmitted list is correctly absent.

**What decides the UI pass:** the difference between captures with and without UI on screen. UI draws should come
late in the frame, on a backbuffer-sized target (6368 × 4592), with blending on and no depth, using small draw
counts (quads).

## Test procedure (H0)

The game ini now has `[debug] ui_probe=1`.
1. Launch. The log should show `UIPROBE: hooks installed on the game's D3D12 device`.
2. In gameplay take four captures (**Ctrl+Alt+U**, then wait for the success tone before the next), noting the order:
   1. **quiet exploration**, with as little HUD on screen as possible;
   2. **with HUD elements**: an interaction prompt, a pickup message or objective text, or aiming (reticle);
   3. **during an examine**, with its text on screen;
   4. **the pause menu** open.
3. Quit and tell me the order.

**Pass:** four `UIPROBE: capture written` lines, and the reports show which passes appear only when UI is on screen.
**Fail:** no "hooks installed" line (the device was created before our hook, or another hook got in the way), or a
crash; set `ui_probe=0` to disable.

## H0 results (2026-09-25 20:50–20:57, five captures)

**The UI is drawn into its own render target, then composited.**

| Capture | Draws into the UI target (frame 1) |
|---|---|
| 1, 2, 4: quiet exploration | **0** (the target is cleared every frame, nothing drawn) |
| 3: HUD elements on screen | **32** |
| 5: pause menu | **101** |

- **The UI target:** RGBA8_SRGB at the game's display size (4096 × 2160 in this session; see below), cleared at the
  start of the UI work each frame.
- **UI pipelines, all with one signature:** RGBA8_SRGB, **premultiplied alpha** (blend ONE / INV_SRC_ALPHA), **depth
  off**, small indexed draws (6–78 indices: quads and glyph strips). The pixel-shader hashes are stable across
  sessions: `0F244A4D` (the most common), `4C4E4163`, `A04BD9F2`, `D5027F1D`.
- **Final composite:** 5 non-blended quad draws into the 6368 × 4592 backbuffer with two tiny blit pipelines (VS 568
  bytes; PS 860 and 592 bytes). The scene and UI are combined there or just before.
- **Not the game's:** a 348–386-quad alpha-blended pass on the backbuffer appears after our Present-hook boundary,
  i.e. inside Present after we copy the frame to the eyes. It's an overlay (OptiScaler, Steam or a stats tool) and
  never reaches the headset.
- **PIX events:** none from the game (the only one, "Whole-resource copy", is a copy utility's).

**H1 plan, grounded in this:**
- Identify the UI target at run time: the RGBA8_SRGB target that receives draws from pipelines with the UI signature.
- Rewrite the viewport and scissor rectangles of every draw into it to a centred, scaled rectangle. The whole UI
  (HUD, prompts, subtitles, examine text) then shrinks towards the centre before it's composited, at a single choke
  point.
- Menus and the map are skipped (they already have their own 0.75 view).

### Found along the way: the render resolution fell back to 4096 × 2160

In this session the **scene was rendered at 4096 × 2160** (the G-buffer: 1,673 draws into 4096 × 2160 targets) and
stretched by the final composite to the 6368 × 4592 swapchain. The game's own log says:
`[Validate Settings] The requested display mode (ResX:6368 ResY:4592 ...) was not available ... Changed display mode.
(ResX:4096 ResY:2160 ...)`. Cause: the resolution unlock's single attempt to find a DXGI output, from inside the
game's first CreateDXGIFactory, found none (`ResolutionUnlock: no IDXGIOutput found`), so the unlock gave up for the
session. A moment later an output was there (the game listed its 4096 × 2160 modes, and a test program sees DISPLAY1
on the 5090).

**Fixed:**
- the unlock is retried:
  - a failed export-path attempt leaves it "not done", so the next call tries again, never waiting;
  - the init thread retries every 100 ms for up to 3 s, before the game validates its resolution about 3 s in;
- the UI probe is installed after the unlock, so it can't delay it;
- a new **RESCHECK** about 10 s after start compares the game's display mode (its own log) with
  `vrmod_fakedesktop.txt`, and on a mismatch logs a WARNING and plays the failure tone.
## H1 — Centred UI (this build)

**How** (`src/ui_probe.cpp`; the D3D12 hooks now install when `[hud] enabled=1` or the probe is on):
- A pipeline has the **UI signature** (H0) when it has 1 render target in RGBA8_SRGB, premultiplied alpha
  (ONE / INV_SRC_ALPHA) and no depth. This is decided at pipeline creation.
- Per command list (thread-local state, cheap) the hooks track whether a UI pipeline is bound and whether render
  target 0 is an sRGB RGBA8 target.
- While both are true, every `RSSetViewports` / `RSSetScissorRects` is mapped about the target's centre:
  p' = c + (p − c) × `scale` + `offset_y` × height. Binding a UI pipeline re-issues the current viewport and scissor
  scaled; leaving it re-issues the game's own.
- Menus and the map are skipped (`MenuScreenActive`).
- **Settings** `[hud] enabled=1, scale=0.6, offset_y=0`. **Keys:** Ctrl+Alt+8 / 9 smaller / larger (±0.05),
  Ctrl+Alt+5 / 6 up / down (±0.02), Ctrl+Alt+7 on/off, Ctrl+Alt+0 saves.
- **Log:** `HUD (30 s): N UI viewport rewrites; on, scale, offset_y` every 30 s, and a line per key press.
- **Offline GPU test** (`scratchpad/uitest/hudtest.cpp`, a real device, scale 0.5, all pass):
  - a UI-signature pipeline's full-screen draw fills only the centre half: the centre is white, the corners
    untouched, and the edges are exact (drawn 2 px inside, not drawn 2 px outside);
  - a following non-UI pipeline's full-screen draw covers everything, so the game's viewport was restored.
- `[debug] ui_probe` is set back to 0; the capture isn't needed any more.

**Known limit:** prompts drawn over world objects (button icons at a lever or a ledge) move towards the centre with
everything else, so they sit off their object. Whether that's acceptable decides H1 (see the test).

## Test procedure (H1)

1. Launch. The log shows `UIPROBE/HUD: waiting ... HUD on, scale 0.600000` and `hooks installed`. Watch for the
   RESCHECK line (the render resolution; no failure tone about 10 s in).
2. In gameplay, bring up UI: an interaction prompt, a pickup message, the objective text, aiming (reticle),
   subtitles in a conversation if one comes up, and an examine if you find one.
   - All of it should now sit closer to the centre, readable without moving your eyes to the lens edge.
   - Nothing clipped or cut off.
   - The 3D view is unchanged.
3. Try **Ctrl+Alt+8 / 9** (smaller / larger) and **Ctrl+Alt+5 / 6** (up / down) until it's comfortable, then
   **Ctrl+Alt+0** to save. **Ctrl+Alt+7** toggles it for comparison.
4. Open the pause menu and the map: they should look as before (full size).
5. Report: the scale and offset you settled on, anything clipped, how world-anchored prompts look, and whether any
   UI didn't move.

**Pass:** all HUD, prompts and messages inside the central area and readable; menus and the map unchanged; the
`HUD (30 s)` line shows rewrites when UI is on screen; no rendering glitches outside the UI.
## H1b — World-anchored prompts must stay on their objects (user, 2026-09-25)

The user's requirement: **button prompts attached to objects stay where they are**; only screen HUD and messages move
to the centre.

**Problem.** On the GPU, prompts and HUD are drawn the same way: the same UI pipelines into the same UI layer. The
viewport rewrite can't tell them apart by itself.

**Game side (Ghidra, `scaleform_decompiled.txt`, `scaleform_vt_decompiled.txt`):**
- The UI is made of widgets. Each `UiWidgetScaleform` owns a Scaleform movie instance at **widget +0x78**; the
  instance's **+0x210 is the movie name** (a string); widget +0x80 is the clip name, +0x50 the widget ID, +0x5C
  visible. Source: the widget debug print `FUN_140b67610`.
- Prompts are their own widget classes: `UiWidgetInteractPrompt`, `UiWidgetTextPrompt`, `UiWidgetFinisherPrompt`,
  `UiWidgetRopeCutPrompt`, `UiWidgetISSPrompts`, plus the script types `NtFloatingPromptType` / `NtActionPromptType`.
- World attachment exists as script nodes: `ScaleformMarkerAttachNode`, `ScaleformLocalSpaceAttachNode`,
  `ScaleformCameraAttachNode`.
- The actual per-movie drawing is inside the statically linked Scaleform library. The renderer's vtable
  (`ScaleformRenderer` [4] `FUN_140bdbd40`) sets up the UI render pass, not individual movies.

**Discovery (this build).** During a Ctrl+Alt+U capture, every bind of a UI-signature pipeline records its **call
stack** (`RtlCaptureStackBackTrace`, 24 frames; distinct stacks counted). They're listed at the end of the report as
module+offset. The stacks that appear only with a prompt on screen lead to the game function that draws the prompt
movie. Hook that, read the movie name, and skip the scaling for prompt movies (a thread-local flag read by the
viewport hooks).
- Harness: stacks recorded and named (module+offset) during a forced capture; the GPU test still passes.
- `[debug] ui_probe=1` is set again in the game ini.

### Test procedure (H1b discovery)

1. Take three captures with **Ctrl+Alt+U**, waiting for the success tone before each next one:
   1. standing next to an interactable object with its **button prompt showing**;
   2. **HUD without a prompt**: aiming (reticle), or an objective or pickup message;
   3. the pause menu.
2. Quit and tell me the order if it differs.
## H1b results and a change of plan (2026-09-25 21:24–21:25)

- Captures: 1 = an object's prompt on screen; 2 = HUD without a prompt, but **no UI draws at all** in that capture
  (nothing on screen at that instant).
- **The call stacks don't separate movies.** Every UI-pipeline bind came from the same render-worker stack
  (`SOTTR.exe+0x376DF3 ... +0x3038AF` thread start). The UI is recorded as a command stream and replayed by a render
  worker, so the stack never contains the game function that drew a given movie.
- **The prompt's draws** (capture 1): 20 draws per frame into the UI layer with pipelines #1946 (PS `0F244A4D`) and
  #1905 (PS `4C4E4163`), the same as the HUD. So there's no draw-level marker.
- **User decision: aiming becomes "aim where you look"** (the head drives the game camera, the standard in community
  VR mods). That also changes the prompt problem: once the game camera follows the head, the game places world prompts
  with the same camera the view uses, so unscaled prompts sit on their objects. What remains is "scale the HUD but not
  the prompts", to be solved after aiming.
- **H1 in the meantime:** the HUD scaling works (`HUD (30 s): 9339 UI viewport rewrites` with UI on screen), but it
  moves prompts too, so `[hud]` stays as is until the prompts can be told apart.
- Also confirmed this session: `RESCHECK: the game renders at 6368x4592, matching the fake desktop` (the unlock
  retry works).
## H1 retest with head aim (2026-09-27)

With aim where you look, whatever you interact with sits near the view centre, where the centred-HUD scaling moves
things least (a prompt 10 % of the width off-centre moves 4 % at scale 0.6). The retest decides whether prompts are
close enough to their objects with the HUD scaled, or need separating. `[debug] ui_probe` set back to 0.

## H1 result (2026-09-27)

The user saved a HUD size. Button prompts attached to objects are wrong in first person (the game places them with its
own third-person camera, not the head camera — separate from the HUD scaling). Workaround chosen by the user: a quick
first/third-person toggle on the gamepad — **LB held + right stick click** (XInput; same as Home). Proper prompt
placement in first person stays open.

## HUD layer (2026-10-08): the HUD kept out of the AFW warp

**Problem (user):** world-anchored icons sit in the right place but flicker and ghost. Under AFW the other eye is
warped from the finished image, HUD included, so every HUD pixel moved with the scene depth under it (docs/AFW.md,
expected artifacts).

**The game's composite** (RenderDoc frame 10430, event 20575): `scene_tonemapped * (1 - ui.a) + ui.rgb` in linear light,
the UI layer sampled through an sRGB view (premultiplied); a later pass gamma-encodes into the swapchain.

**How (`src/hud_layer.cpp`, hooks in ui_probe.cpp):**
- The game's HUD layer is learned at run time: the RGBA8 sRGB target a UI-signature pipeline draws into (outside menus).
  A texture of ours of the same size is created (R8G8B8A8_TYPELESS, sRGB RTV and SRV).
- `OMSetRenderTargets` binding the game's layer binds ours instead; `ClearRenderTargetView` on the game's layer also
  clears ours. The game's layer stays empty, so its composite and the backbuffer hold the scene only -- what AFW warps.
- At Present, after both eye images are written (`CopyBackbufferToEyes12`), a full-screen pass blends our layer over
  each eye: with an sRGB view of the eye image when its format allows (typeless / sRGB: the hardware blends in linear,
  like the game), else a UNORM view with the HUD colour gamma-encoded (2.2) in the shader.
- Only while the Present path composites (checked every frame) and no menu is open: menus and the map keep the game's
  own composite. The HUD scaling (H1) still applies -- it rewrites the viewports of the same draws.
- Our composite pipeline has the UI signature itself, so it is marked not-UI (`UiProbeMarkNotUi`).
- `[hud] layer` (default 1), **Ctrl+Alt+L** toggles; `HUDLAYER (30 s)` log line (binds redirected, eye composites).

**Expected:** HUD and world icons steady in both eyes (no ghosting), at zero disparity (at infinity), like the rendered
eye before. A world icon is placed by the rendered eye's camera, so in the other eye it is half an eye-distance off its
object. HUD depth (H2: a horizontal shift per eye; the shader has the constant ready) can follow.

**First test (2026-10-08 23:30-23:41):** active (layer found 6368x4592, ~300 binds redirected/s, 144 eye composites/s,
Ctrl+Alt+L registered) but no visible difference: the flicker isn't the warp. Under AER the game camera alternates
between the eyes and the game places world icons with it, so their screen position jumps by the eye offset every
frame -- in the warped eye before, in both eyes with one shared HUD. **Fix:** a HUD store per eye (2 x 117 MB at
6368x4592): each frame's HUD is copied into the store of the eye it was rendered for (both when mono), and each eye shows
its own store. Icons then sit on their objects with the right disparity in both eyes; the HUD updates at 36 Hz per eye.

**World icons (2026-10-09):** the item / collectible icons did not turn red with the tint test (not in the HUD layer) and
were steady with AFW off. Two Ctrl+Alt+U captures (icon on / off screen) differ by 12 draws in the full-resolution HDR
scene pass (6368x4592 R11G11B10F, depth-tested against the full-resolution depth, after DLSS and right after the HUD
layer's clear) with pipelines whose pixel shaders are the Scaleform UI shaders (`0F244A4D`, `A04BD9F2`): world-space
Scaleform drawn into the scene. AFW's depth (from the DLSS input) doesn't have them, so the warp moved them with the
depth behind. **Now:** at pipeline creation every such pipeline (1 RT R11G11B10F, premultiplied blend, UI pixel shader)
gets a copy for our HUD layer (RGBA8 sRGB, alpha as coverage, same depth test); a draw with one, while its pass targets
an R11G11B10F target, goes into our layer with the copy -- so the icons get the per-eye HUD stores and stay out of the
warp, and walls still hide them. Our target stays bound for the following icon draws; the game's targets and pipeline
are re-bound only if a different draw follows in the same pass (logged in the HEADHIDE line). The icons were HDR scene
values; in the 8-bit layer they clamp to white (a possible look difference on soft edges). Log: `HUDLAYER (30 s)`
counts world-icon draws moved.
