# Workstream D — DLSS under AER (Brief 2)

**Problem.** AER alternates eyes frame by frame, so the upscaler's temporal history mixes left into right, and the
motion vectors point at the previous frame, which is the other eye (about 3 cm of IPD plus a frame of head motion).
The Phase 3 ghost test failed with DLSS Quality.

## D0 — Audit (this build)

### The game's DLSS integration (Ghidra; decompiles in `dlss_helpers_decompiled.txt`, `dlss_create_decompiled.txt`)

NGX is linked statically: the exe looks up the core functions by name (`FUN_141035da0`: `NVSDK_NGX_D3D12_CreateFeature`,
`..._EvaluateFeature`, ...) and **exports the `NVSDK_NGX_*` functions itself** (the references from 0x141457D28 are the
export table) — the natural interception point for OptiScaler. The game's own wrapper is a `DLSSImpl` object (NxDLSS
log channel) with a vtable at 0x1412B3048:

| function | role |
|---|---|
| `FUN_14101d140` ensure(this, cmdList, renderW, renderH, outW, outH, flag, node) | once per frame: creates the feature for GPU node `node` if its slot is empty, or re-creates all slots when the size, quality or flag changed |
| `FUN_14101b9c0` create(same args) | NGX CreateFeature via `FUN_14101c3d0` with CreationNodeMask `1 << node`, into slot `node` |
| `FUN_14101b860` evaluate(this, cmdList, color, output, depth, motionVectors, exposure, renderW, renderH, jitterX, jitterY, reset, node) | fills the evaluate block and calls `FUN_14101c4d0` with the handle in slot `node` |
| `FUN_14101c4d0` | the NGX helper: `Color`, `Output`, `Depth`, `MotionVectors`, `Jitter.Offset.X/Y` (Y sign-flipped by the wrapper), `Reset`, `MV.Scale.X/Y` = 1.0, `ExposureTexture`, render subrect, then `NVSDK_NGX_D3D12_EvaluateFeature_C` |

The DLSS object holds **four feature slots** at +0x28 (stride 0x10: handle, then an "initialised" int at +0x30), one
per GPU node (multi-GPU; the registry's `MaxMultiGPUCount=4`). On one GPU only slot 0 is used. **That's the D1 lever:**
a second feature for the same GPU (CreationNodeMask 1) in slot 1, created with the game's own create parameters, and
the evaluate's slot index steered by eye — the game's own code does the rest, and OptiScaler (at the NGX layer) just
sees two features.

### OptiScaler

OptiScaler-NR 0.8.3 as `dbghelp.dll`; finds `nvngx_dlss.dll` in the game folder; `UpscaleRatioOverride = 1.0` (DLSS
input = output size, i.e. DLAA); the DLSS-NR denoise pass runs before SR (`WorkingScale 0.4`, toggle key **R** — a
gameplay key; to revisit). DLSS is currently off in the game (`DLSS=0`, last used Quality), so none of it runs.

### Runtime probe (`src/dlss_probe.cpp`, logging only)

Byte-checked hooks on ensure, create and evaluate:
- `DLSS create:` each create — node, render and output size, flag, the quality index, the slot handle;
- `DLSS ensure:` whenever its arguments change;
- `DLSS eval:` the first 40 evaluates in full — node, render size, jitter, reset, the resources, the thread, the latest
  game frame and its AER eye, the Present count; then the eye sequence over 64 evaluates;
- `DLSS (5 s):` evaluates per Presents, resets, nodes used, render size, jitter range.

### Test procedure (D0)

1. In the game's graphics options switch **DLSS on (Quality)**. (OptiScaler then runs at its 1.0 ratio; fine for this.)
2. Play about a minute of normal gameplay (look around, walk), then quit.
3. Optional: switch DLSS back off afterwards if performance suffers.

**Accept:** the call graph above confirmed by the log (one evaluate per frame, node 0, jitter moving, resets at cuts),
and the eye sequence showing how evaluates line up with AER's eyes.

## D0 results (2026-09-27 14:29, DLSS Quality selected; DLAA in effect)

- `DLSS create: node 0 render 6368x4592 -> output 6368x4592 flag 1 -> ok; quality index 0` — render = output, i.e. DLAA
  (OptiScaler's `UpscaleRatioOverride 1.0` and/or an Inspector override).
- **Exactly one evaluate per game Present** (120 over 120, 180 over 180 …), always **node 0**, always on one render
  thread; jitter a ±0.5 px sequence (0 in the pause menu); **reset only at cuts** (the load, one cut).
- **Cost:** 72 fps → 24–36 fps and ~20 GB private memory with DLAA at 6368×4592 plus OptiScaler's NR pass; PACE 27.6 %
  headset frames missed. D3 matters: real upscaling ratios and the NR cost.
- **Eye alignment: not measured** — the 64-evaluate eye sequence was taken during the load, before AER ran ("????").

**Probe update (this build):** the eye sequence starts once AER runs, and every 10 s a `DLSS align` line compares each
evaluate with the Present that follows it: the paired game frame minus the evaluate's latest game frame (histogram),
and whether the eyes agree taking the evaluate's frame as latest − 0, − 1 or − 2. That fixes D1's routing rule.

## D0 result 2 (2026-09-27 14:44, gameplay from 14:44:20)

`DLSS align`: the Present after an evaluate shows the game frame 2 behind the evaluate's latest game frame for a
while (335 of 360), then 3 behind (360 of 360) — the pipeline depth drifts, so the latest game frame can't pick the
eye. The eye sequence alternates cleanly once AER runs (…LRLRLR…). **Routing rule:** an evaluate renders the frame the
next Present shows, and AER pairs Presents one game frame at a time, so its eye is the eye of *AER's last paired
frame + 1*. (The first 3½ minutes were the main menu, where head tracking is off; the game runs DLSS behind the menu.)

## D1 — per-eye history (this build)

- **Second feature:** right after the game's ensure (node 0, D3D12), if slot 0 exists and slot 1 is empty, the game's
  own create helper `FUN_14101c3d0` makes a feature in **slot 1** with CreationNodeMask 1 (the same GPU), the game's
  render / output size, quality (object +0x1C) and flags (`10 + flag`), under the object's lock. When the game
  re-creates (size or quality change) it releases all four slots itself; slot 1 is then created again. A failed
  create switches D1 off for the session (logged).
- **Routing:** each node-0 evaluate goes to slot `eye` = the eye of AER's last paired frame + 1 (0 left, 1 right).
  Unknown eye (menus, head tracking off, before AER) → slot 0, and both histories reset when the eye is known again.
- **Resets:** the game's reset (cuts) is passed on and also applied to the other eye's next evaluate; if the Present
  after an evaluate wasn't paired with the predicted frame (an AER resync), both histories reset; toggling and
  creating reset both.
- `[render] dlss_per_eye=1` (default); **Ctrl+Alt+D** toggles for A/B. Log: `DLSS D1: second feature ... created`,
  and every 10 s `DLSS D1 (10 s, per-eye): evaluates to left N, right N, eye unknown N | AER resyncs seen N, history
  resets N`.
- Motion vectors still point at the other eye's previous frame — that's D2; D1 alone should pass the ghost test on
  still scenes.

### Test procedure (D1)

1. DLSS on as before. In gameplay, standing still with a detailed view: **Ctrl+G** (ghost test; the headset goes black
   while it runs). Three rising tones = pass, falling tones = fail.
2. **Ctrl+Alt+D** (low tone: per-eye off), wait a second, **Ctrl+G** again — expected to fail (the old behaviour).
3. **Ctrl+Alt+D** (rising tone: on again). Look around and strafe: note any difference in shimmer or ghosting between
   on and off (movement still has the D2 motion-vector problem).
4. Resize test: in the graphics options change the DLSS mode once (e.g. Balanced) and back — no crash, and the log shows
   the second feature created again.

**Accept:** the ghost test passes with per-eye on and fails with it off; the D1 line shows left ≈ right with few
resyncs; the mode change re-creates slot 1 without trouble.

## D1 results (2026-09-27 14:59–15:04, DLSS Performance then Balanced)

- **Correction (user, same day): the shimmer drops with the *falling* tone — per-eye OFF.** The tones were right (the log
  pairs each press with "off"/"ON"). Reason: the game's motion vectors describe the change from the previous frame's
  camera, which under AER is the *other* eye (our write). With one shared history, history and motion vectors agree —
  DLSS treats the eye switch as a small camera move and reprojects it (only parallax disocclusion and view-dependent
  effects are lost). With per-eye history the history is the same eye two frames back while the motion vectors still
  say one frame, other eye: wrong reprojection, rejected history, shimmer. **D1 needs D2.** Default now
  `dlss_per_eye=0`; D2 will be compared as single history vs D1 + D2.
- *(first reading, superseded)* shimmering stops with per-eye history; "great image quality even with DLSS Performance" — unlike an earlier
  mod attempt that needed DLAA and a negative LOD bias and still shimmered. Moving objects still ghost (D2).
- `DLSS D1: second feature (right eye) in slot 1 for 3184x2296 -> 6368x4592: created`; routing left ≈ right
  (357 / 356, 346 / 346, 359 / 359 per 10 s), 0–2 AER resyncs per 10 s, resets as designed.
- **Ghost test: INVALID ×4** ("too few still areas — the scene moved", 0–7 still tiles of ~100, camera drift ~3 cm) —
  no verdict either way. The test was built without an upscaler; DLSS still converging after each ±60° flip may count
  as motion. To revisit (static indoor spot, or longer settle under DLSS).
- **Resize not yet exercised with D1 on:** at the mode change (15:04:15, Performance → Balanced) per-eye was off, so slot 1
  wasn't re-created (by design). The game's own re-create worked.

## D1 comparison (2026-09-27 15:13, Balanced)

Ensure runs every frame (361 ensures per 361 evaluates), so toggling per-eye on mid-game creates slot 1 at once
(15:15:40) — the comparison is valid. **User:** per-eye off = no shimmer; per-eye on = shimmer *and* heavy smearing;
blur on moving objects about the same in both. **Conclusion:** the single history with the game's own (cross-eye)
motion vectors is the best configuration now (default). Per-eye history needs D2 to be consistent; D2 is only worth
building if a tool needs separate handles per eye (the VR OptiScaler fork; possibly foveated DLSS) or eye-specific
artifacts appear with the single history. Next: check how CheekyFoveatedDLSS / ReallyCheekyFoveatedNR handle
alternating eyes before deciding on D2.

## CheekyFoveatedDLSS upstream (v0.5.2, 2026-09-26; source read 2026-09-27)

Cloned (scratchpad, GPL-3.0). Findings:
- **Alternating-eye mode ("AFW", UEVR's alternating-frame plugin) keeps ONE DLSS stream across both eyes** — its
  history test alternates `afw_source_eye = frame % 2` on one view and one feature with
  `preserve_history_on_crop_move = true`; release notes: "preserves temporal history across alternate-eye crop
  positions", coverage modes "cover both eyes", crop size fixed while the fovea moves. The same configuration that
  looked best here. **D2 is not a prerequisite for Cheeky.**
- **Motion vectors are passed through unchanged** (`docs/nr-motion-vectors.md`).
- **Eye identity** comes from the host: UEVR's AFW plugin passes `source_eye` in its warp callback
  (`AfwWarpPrefix` at +64; `afw_note_warp_call`, read during evaluation by `afw_current_source_eye()`), which places
  the foveal crop in that eye's projection (`publish_afw_stereo_projection`). Its automatic marker calibration (DLSS
  output → submitted eye) tolerates alternate-frame eyes only on D3D11 OpenXR ("OpenVR and D3D12 retain their same-frame
  policy") and treats one output shared by both eyes as mono ("cannot force a mapping") — SOTTR under AER is D3D12
  with one DLSS output alternating eyes, so it can't identify our eyes by itself.
- **Loading next to our `dxgi.dll`:** the standalone `version.dll` loader, or Cheeky as `dxgi.dll` chaining another
  mod renamed `dxgi2.dll`, or as an **ASI plugin in an existing OptiScaler** (here `dbghelp.dll`).
- **Integration for the user's own setup (a small personal fork, possibly an upstream PR):** a host path besides UEVR —
  Cheeky asks our mod for the source eye of the current evaluate (our mod knows it exactly: the eye of AER's last
  paired frame + 1, see D1) and for the two eye projections, and runs its AFW single-stream foveation. Gaze from the
  Dream Air through Cheeky's OpenXR layer (`CheekyOpenXRSetup.exe`, system-wide — user's go-ahead needed).
- The ReallyCheekyFoveatedNR fork is superseded: R.E.A.L. VR support is upstream since v0.5.0.

## Cheeky with the VR mod (2026-09-27 16:18, fork github.com/jcooray/CheekyFoveatedDLSS; released builds use tag `sottr-vr-1.0.0`)

- Loading: Cheeky's `version.dll` loader took the name `dxgi.dll` (its host imports System32 DXGI) before SOTTR loaded
  DXGI (SOTTR imports none), so the VR mod never loaded — first flat, then SteamVR theatre. Fork change 1 in
  `bootstrap/loader.cpp` loads the proxy beside the game first. The documented alternative that needs no fork:
  Cheeky as `dxgi.dll`, this mod renamed `dxgi2.dll` (for the release docs; to verify).
- Per game DLSS feature Cheeky runs two passes: a full-frame periphery pass (DLAA on a 75 % copy, 3184×2296, MV scale
  0.75) and a centre pass (2335×1377 → 3503×2066, a fixed crop at 1432,1263 of 6368×4592). With per-eye history the
  second game feature gets its own pair (per-eye Cheeky histories).
- 72 fps, PACE 0.22 % missed (DLSS Balanced, no NR).
- **User: with Cheeky, the single history smears and per-eye history removes it** — the reverse of stock DLSS. Cause
  not identified yet (the periphery resampling or the crop composite may expose the eye switch). The user's ini set to
  `dlss_per_eye=1`; the default stays 0.
- **Next fork step:** tell Cheeky which of its views is which eye (our slot 0 = left, slot 1 = right), so each eye's
  foveal crop sits at that eye's own centre (its lens centre now, the gaze later) instead of one fixed centre crop.

## D2 — same-eye motion vectors: plan and D2a (this build)

**Decision (2026-09-27):** Cheeky parked (its periphery shimmers under AER); the user's everyday setup is the game's DLSS
with the single history (`dlss_per_eye=0`, OptiScaler restored). D2 makes per-eye history consistent for every route
(stock DLSS, Cheeky's centre and periphery, other per-eye tools).

**Design:** leave the game's "previous camera" alone — its other temporal effects (AO, SSR, volumetrics) keep one
history and are consistent with it now. Instead, just before the DLSS evaluate, a compute pass rewrites only the
motion vectors DLSS receives: for each pixel, reconstruct the world point from depth with the frame's own camera and
re-project it with the *same eye's* camera two frames back instead of the other eye's one frame back
(`mv' = mv + (p_same - p_other)`; object motion stays the game's one-frame estimate).

**D2a — prove the model first.** Our cameras per game frame are recorded (`CameraFrameRecord`: final position,
rotation, near/far; `AerFinalProjectionOf`: the game's final vertical FOV and aspect). **Ctrl+Alt+Q** arms one capture:
at the next evaluate a compute pass (recorded into the game's command list before DLSS; shader compiled at run time with
d3dcompiler_47) samples the game's motion vectors and depth on a 32×18 grid; 8 Presents later the samples are compared
with the prediction from the frame's camera and the previous frame's (the other eye) under each depth convention
(reverse-Z infinite / finite, forward-Z), sign and unit. The log ranks them (median / 90 % error in pixels) and gives the
size of the D2 correction.

### Test procedure (D2a)

1. DLSS on (any mode; per-eye history either way). In gameplay, somewhere with near and far geometry, standing still
   in the world while moving the head gently.
2. **Ctrl+Alt+Q** — rising tone, then the success tones a moment later. Repeat 2–3 times in different spots (close to
   a wall, looking into the distance).
3. Quit.

**Accept:** one convention matches with a median error well under a pixel, consistently across the captures.

## D2a results (2026-09-27 17:17, three captures) and D2b (this build)

- **Model confirmed:** `mv = previous - current` in **pixels at render resolution**, depth **reverse-Z** (finite and
  infinite indistinguishable at far 550000 cm; finite with the recorded near/far is used). Our recorded cameras
  reproduce the game's vectors to a median **0.002–0.010 px** (90 %: 0.02–0.07 px) over 434–576 samples per capture.
  MV format 16 (R16G16_FLOAT), depth format 44 (R24G8_TYPELESS, read as R24_UNORM_X8).
- **The correction is large:** median **3–27 px**, 90 % **24–40 px** — the gap between "other eye, one frame back" and
  "same eye, two frames back"; it explains the per-eye smear.

**D2b:** with per-eye history (`dlss_per_eye=1` / Ctrl+Alt+D), every routed evaluate first runs a compute pass over the
render rectangle (8×8 groups): depth → point (the frame's camera, all maths relative to its position), projected with
the other eye's camera one frame back (`pO`) and the same eye's two frames back (`pS`); the output vector is
`(pS - pc) + 2 · (mv - (pO - pc))` — the camera part becomes the same eye's, the object part is stretched from one frame
to two (constant velocity). Written into our own R16G16_FLOAT texture (the game's size), transitioned to shader-resource,
and passed to DLSS instead of the game's. Descriptors in an 8-frame ring. Skipped (the game's vectors pass through,
counted) when the cameras or projection aren't recorded or the eye alternation isn't L-R-L / R-L-R.
**Ctrl+Alt+F** toggles the correction (on by default) for A/B. Log: `DLSS D2 (10 s): motion vectors corrected N,
skipped N`. Shader compile-checked offline.

### Test procedure (D2b)

1. DLSS on (Performance or Balanced). **Ctrl+Alt+D** (rising tone: per-eye history on; the correction is on with it).
2. Compare in the headset, moving and turning, near and far objects:
   - per-eye + correction (as set in step 1);
   - **Ctrl+Alt+F** (low tone): per-eye without correction (the old shimmer/smear, for reference); Ctrl+Alt+F again;
   - **Ctrl+Alt+D** (low tone): single history (the current best).
3. Change the DLSS mode once in the graphics options with per-eye + correction on (resize check).

**Accept:** per-eye + correction free of the per-eye shimmer and smear, at least as clean as the single history;
corrected ≈ all evaluates in the log; the mode change survives.

## D2b result and D2c (2026-09-27)

**User:** per-eye history + D2 is as good as the shared history; per-object blur remains (likely inherent to AER at
this frame rate). **But** leaving the pause menu broke it until per-eye (and maybe the correction) was cycled.

Log (17:26): during the menu, routing sent 491 evaluates to one eye and 230 to the other with 261 "resyncs" and a
history reset every frame — head tracking is off in menus, game frames stop, and the stale prediction kept picking the
same eye. The routing statistics recovered right after the menu, so the lasting breakage points at the frame
*identity*: D1/D2 predict the evaluated frame as AER's last paired frame + 1, which assumes the Present was handled
before the render thread began the next frame; after a stall the render thread can run a frame ahead, and D2 then
corrects with the cameras of the wrong frames (smear), while D1's histories stay self-consistent.

**D2c (this build):**
- **Frame check:** every second routed evaluate samples 16×9 of the game's motion vectors and depth (the D2a shader,
  an 8-slot ring). Six Presents later the Present thread computes, for offsets −2…+2 from the prediction, how well that
  frame's camera and the one before it reproduce them (D2a: the right frame matches to ~0.01 px). A clear winner
  (< 0.3 px median, next best > 1 px and > 4×) that differs from the current offset twice in a row becomes the offset
  D1 routing and D2 use; both histories reset on a change (`DLSS D2c: the evaluated frame is the prediction +N`).
- **Menus / head tracking off:** no per-eye routing or correction (the shared slot); both histories reset when AER is
  back.
- `DLSS D2 (10 s)` line now also reports frame checks (agree / ambiguous), the offset and the last errors.

### Test procedure (D2c)

1. DLSS on, **Ctrl+Alt+D** (per-eye + correction). Play a minute — it should look like before (as good as shared).
2. Open the **pause menu**, wait a few seconds, resume. Play on: it should stay clean without cycling anything (or
   recover within a second or so).
3. Repeat the menu 2–3 times, and once more after a map screen or an inventory if handy.

**Accept:** no need to cycle per-eye after menus; the log shows `frame checks … agree` most of the time and, if the
offset moves after a menu, a `D2c` line followed by clean frames.

## D2c result (2026-09-27 17:40) — D2 done

The pause menu no longer breaks per-eye history (user). The log confirms the cause and the fix: one second after
resuming (17:40:15) the frame check found the render thread one frame behind the prediction (offset −1, 0.004 px vs
1.83 px next best), switched and reset both histories; back to +0 two seconds later; again at 17:42:31 after a hitch.
Checks otherwise agree (≈ 75–90 %; the rest ambiguous, e.g. distant scenery where offsets predict alike).

**Next: dynamic foveated DLSS** — (1) Cheeky with fixed foveation on top of per-eye history + D2 (Cheeky sits under the
game's DLSS call, so it gets the corrected motion vectors; the periphery shimmer was the mismatch D2 removes);
(2) fork: tell Cheeky which view is which eye; (3) gaze from our own OpenXR session (the runtime offers eye gaze) handed
to Cheeky, instead of Cheeky's system-wide OpenXR layer. Test setup: `version.dll` active, OptiScaler set aside,
`dlss_per_eye=1`.

## Dynamic foveated DLSS (2026-09-27)

- **Cheeky + per-eye history + D2: the periphery shimmer is gone** (user) — Cheeky sits under the game's DLSS call and
  gets the corrected motion vectors.
- The eye mapping alone changes nothing visible: under AER each eye's image is rendered symmetrically around that eye's
  forward direction, so Cheeky's centred fixed crop is already where each eye looks straight ahead. It matters for gaze.
- **Gaze build (this build):**
  - `eye_gaze.cpp`: XR_EXT_eye_gaze_interaction in our own session (a pose action bound to
    `/user/eyes_ext/input/gaze_ext/pose`, attached after session creation), synced after every `xrWaitFrame`, located
    in the VIEW space; `EyeGaze (30 s)` log line.
  - `cheeky_host.cpp`: two exports from our `dxgi.dll` — `CheekyGazeProvider_GetGazeSnapshot` (Cheeky's gaze snapshot
    layout, ABI 4: the head-relative gaze projected with the game's final frustum to (u, v) in each eye's image, the same
    for both eyes; FOV; forward = centre) and `CheekyGazeProvider_GetEyeViews` (slot 0 / slot 1 handles = left / right,
    output size; only while per-eye history runs). Written from Cheeky's documented interface, no GPL source.
  - Fork change 2 (`gaze_foveation.cpp`): without Cheeky's OpenXR layer, read the snapshot from our export, and publish
    our eye views as its calibrated stereo pair (full-image mapping) every 250 ms.

- **Gaze result 1:** no eye data through OpenXR on SteamVR + Dream Air. The session was rarely focused
  (`xrSyncActions` → 8, XR_SESSION_NOT_FOCUSED), and when it was the eye gaze action stayed inactive although the
  eyes profile was bound. Cheeky's snapshot flags showed 0x5F (everything except GAZE_VALID).
- **Gaze build 2 (`openvr_gaze.cpp`):** SteamVR's OpenVR interface, as Cheeky's own OpenVR adapter reads it —
  `IVRSystem::GetEyeTrackedFoveationCenter` (per-eye NDC for the OpenVR projection), turned into a head-relative
  direction per eye with `GetProjectionRaw` and `GetEyeToHeadTransform`, then projected with the game's frustum per eye.
  Loads SteamVR's installed `openvr_api.dll` (path from `openvrpaths.vrpath`), connects as a background application,
  polled after every `xrWaitFrame`, never shut down (it shares the client with the OpenXR runtime). Falls back to the
  OpenXR gaze. `[gaze] openvr=0` turns it off. Log: `OpenVR gaze: connected …` and `OpenVR gaze (10 s): … valid`.
- **Cheeky-only distant smear** (user, 2026-09-27): fine distant detail (a thatched roof) smears only with Cheeky on;
  the game's DLSS alone (single history, or per-eye + D2) is clean. That is Cheeky's own pass (periphery at a lower
  scale, its preset); with the gaze working, the sharp region follows the eyes.

### Test procedure (gaze)

1. DLSS on, per-eye history on (`dlss_per_eye=1`, as set). Eye tracking running for the Dream Air (Pimax software).
2. In Cheeky's window (**F3**): set the foveation centre / eye-tracking mode to the **runtime gaze** option (not
   fixed), foveation on.
3. Look around the image with your eyes (not your head): the sharp region should follow your gaze in both eyes.
4. Quit; the log shows `OpenVR gaze: connected …` and `OpenVR gaze (10 s): N polls, M valid` with M close to N.

## GPU cost by stage (GPUSTAGES, 2026-09-27, gameplay, render 4245x3061 -> 6368x4592, per-eye DLSS + D2)

| Setup | game before DLSS | MV passes | DLSS incl. Cheeky/NR | game after | eye copy | GPU busy | result |
|---|---|---|---|---|---|---|---|
| Cheeky DFR + NR | 5.1 | 0.14 | 11.4 | 0.9 | 0.13 | 17.5-18.3 | half rate, 36 fps (27.8 ms, 37% idle) |
| Cheeky DFR, NR off | 5.3 | 0.14 | 1.6 | 0.8 | 0.12 | 8.0 | 72 fps, 42% idle |
| game DLSS only (Cheeky off) | 5.4 | 0.14 | 2.6 | 0.9 | 0.13 | 9.3 | 72 fps, 34% idle |

- NR (Cheeky's foveated DLSS-NR, region 3568x2576 at NrWidth/NrHeight 0.56) costs ~9.8 ms per frame -- the whole reason
  for the 36 fps lock. Cheeky's DFR saves ~1 ms over the game's own DLSS.
- 72 Hz needs busy <= 13.9 ms: NR must come down to ~5 ms (smaller region / NrWorkingScale). 90 Hz half rate (22.2 ms)
  would hold 45 fps with NR as it is.

## DLSS 5 and the VR upscaler forks (2026-09-27; routes in `docs/DLSS5_ROUTES.md`)

- **SOTTR has native DX12 DLSS**, so the NGX-level routes apply (OptiScaler_DLSSNR forks, RenoDX/ReShade addon); no
  Feeder needed.
- **`tig3rmast3r/OptiScaler_DLSSNR_VR`** (v0.1.1-vr2): per-eye DLSS for alternating-eye VR renderers *that use separate
  DLSS handles per eye*; NR runs at the internal render resolution before SR, per eye (no ray reconstruction on that
  route). Loads as an OptiScaler proxy (here `dbghelp.dll`), so no clash with our `dxgi.dll`. **D1 (one handle per eye
  via the game's spare node slot) is exactly the interface it expects.** Its forwarder is already in the game folder,
  parked as `nvngx.dll_dlssnr.dll.vrfork`.
- **`chronowake/ReallyCheekyFoveatedNR`**: an NR-only fork of CheekyFoveatedDLSS for R.E.A.L. VR (also alternate-eye);
  a ReShade add-on plus an OpenXR layer (`CheekyEyeTrackingSetup.exe`). Foveation (D4) later. ReShade could load as
  `d3d12.dll` (the game loads it) to stay clear of our `dxgi.dll`. The original CheekyFoveatedDLSS is itself a
  `dxgi.dll` proxy — it would need chain-loading.
- **Proxy names the game can use** (its import table): `dbghelp.dll` (OptiScaler now), `WINMM.dll` (free), `dxgi.dll`
  (ours); `d3d12.dll` is loaded dynamically. The exe also imports Tobii's game integration (`tobii_gameintegration_x64`).
- **Runtime build:** the `nvngx_dlssnr.dll` in the game folder is 310.8.SF.0 (unsigned) — the RTX 20/30 community build;
  an RTX 5090 wants 310.8.0 (RTX 50, NVIDIA). Driver 616.56.
- **Order of work unchanged:** D0 audit → D1 per-eye handles → D2 same-eye motion vectors → NR through the VR fork
  (plain, no foveation) → D3 performance → D4 foveation as separate R&D. Resize / resolution-change tests early (the
  worst reported failure mode with ReShade-based layers).

## Eye pairing follows the frame check (2026-10-01)

**User (playthrough 00:27–00:44):** after some first-person switches the view went wrong -- "the floor or nearby
vertical surfaces seem too close and incoherent with the depth of other surfaces" -- and stayed wrong in third person.

**Log:** each time just after a D2c change to offset −1 (00:42:38 after a cutscene and a foreign camera write; the
day before 21:53:18), and it lasted until the offset went back to +0 (the day before: 4.5 min later; here until the end).
Offset −1 happened 22 of 99 D2 windows in this session and 46 of 141 in the previous one. PHASE6 (the frustum) always matched.

**Why it shows each eye the other eye's image:** the Present shows the frame DLSS evaluated just before it. D2c
measures that frame from the game's own motion vectors (~0.005 px). Offset −1 means the evaluate after the Present
paired with S was frame S itself -- so that Present really showed S − 1, and AER sent it to S's eye with S's pose. D1/D2
were re-routed (the DLSS histories stayed right) but the eye pairing wasn't, so the images were swapped between the eyes
for as long as the offset lasted: pseudo-stereo, which is worst on near surfaces and the floor (largest disparity) and
invisible on distant ones. The Phase 4 clamp (S within G − 2 … G − 1) can't catch it -- both values are inside the window.

**Change:** `[render] aer_pair_follow_d2c=1` (default). A confirmed D2c offset now moves the AER pairing (S and the
clamp window) by that many frames, and the offset returns to 0; checks recorded before the move are dropped. Log line
`DLSS D2c: … the Presents were paired with the wrong game frame … eye pairing moved −1 (net …, window lag now …)`.
Guard: two reversals within 10 s each switch the follow off, undo the moves and fall back to re-routing DLSS only.
`aer_pair_follow_d2c=0` restores the old behaviour.

**Head lock after that cutscene:** 64 bone arrays near the camera, none Lara-shaped, for 1.5 minutes, so the offset
camera was used. That's a separate problem (her rig after the cutscene?); the failure line now lists the largest arrays'
bone count, distance and head height.
