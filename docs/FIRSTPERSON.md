# First person: head-bone camera

Goal (user):
- The first-person camera sits at Lara's actual head (eyes), not at a hand-tuned offset from the third-person boom.
- The head is hidden.
- Idle animations can't swing the head into view, and the camera can't clip through walls.
- Scripted play and cutscenes always run in third person.

## Done so far

- **Third person while the game steers the camera** (`ThirdPersonReason::GameCamera`, commit 875451f). Sustained
  adjusted or replaced camera writes (the Phase 8 read-back) switch first person off: squeezing through gaps, the
  scripted gorge climb. It comes back 2 s after the steering stops. In game: `ThirdPerson: switching for
  'game-steered camera'` fired after the gorge cutscene.
- **Recentre at cutscene start and end** (AER), as F8 does: the game cuts to a new camera, so forward is re-taken from
  the head.
- End (the old half-rate toggle) does nothing under AER. Home is the first-person toggle.

## Step 1: discovery build (this build)

What's known (Ghidra, `helper_decompiled.txt`, FUN_140436380 and callers):
- A character instance's +0xE0 points to its skeleton state.
- The state's +0x70 (and +0x78) points to an array of 4Ã—4 float bone matrices, 0x40 bytes per bone, with the
  translation row at +0x30 and w = 1.
- The state's +0x9C holds the bone count (u16).
- Bones have no names (TrRebootModTools), so the head is identified by position and motion.

**Ctrl+Z: bone scan** (`src/bone_scan.cpp`). Stand still in first person with the camera inside the head.
1. Scans writable memory for 4Ã—4 matrices whose translation is within 80 cm of the camera, in 8 ms slices so the
   game keeps running.
2. Grows each hit into its whole array.
3. Searches for pointers to each array. A pointer from a skeleton state whose +0x9C equals the array length is
   marked CONFIRMED.
4. Samples the arrays over 3 s.

Output: `vrmod_bonescan_<time>.txt`, listing per array its update rate, the bone nearest the in-head point, and for
the top arrays every bone's position in camera axes and how far it moved. Success tone if an array is confirmed,
failure tone if not.

Offline (`scratchpad/bonetest`): a synthetic 60-bone skeleton with a state pointer. It is found and confirmed, the
animated head bone is picked as nearest, and the decoys are rejected.

**Ctrl+Alt+Z: near-plane test.** Cycles off / 15 cm / 30 cm. The camera struct's +0x100 holds (0, 0, âˆ’1, 5),
which looks like the view-space near plane at 5 cm; the four side planes are at +0xC0 to +0xF0. While on, the hook
writes the value into +0x10C after every game write, and the struct's +0x100 to +0x13C are logged at each press.
- If this is the near plane the renderer uses, Lara's head (in first person) and walls closer than the value
  disappear.
- If nothing changes, it's a culling-only copy and the projection's near value lives elsewhere (next candidates:
  +0x120 to +0x13C).

## Next steps

- Identify the head bone from the scan, then read it every frame through the state pointer (find a stable path to the
  state: from the player instance, or a byte pattern for the code that uses it).
- Camera = head bone position (plus a small eye offset). Orientation stays game camera plus headset. Smooth the bob,
  since AER turns vertical bob into vertical disparity.
- Hide the head with the near plane (or by shrinking the head bone).

## Discovery results (2026-09-24 21:57â€“22:02, five Ctrl+Z scans)

- **Lara's skeleton was found in every scan.** Two identical arrays, `0x209D8787F00` and `0x209D8789AC0`, 0x1BC0
  (111 slots) apart, sat at the same addresses in all five scans (the level stayed loaded).
  - Each has 94 valid bone matrices from its start; the walk stops at bone 94. The remaining slots are probably
    zero-scaled or non-rigid bones.
  - Each updated on **alternate Presents** (106â€“108 of 215): the pose is **double-buffered**.
- Humanoid layout (the last scan, relative to the in-head camera, cm right / forward / up):
  - bone 0 is the root, 135 cm below, and doesn't move while idle;
  - bones 2â€“10 are the spine up to the neck;
  - bones 11â€“13 are the neck and head: bone 12 at (âˆ’4.7, âˆ’14.2, +0.2), bone 13 at (âˆ’7.4, âˆ’18.5, +0.1);
  - bones 14â€“18 are a chain hanging down behind the head (the ponytail);
  - bone 19 is probably the jaw;
  - bones 20â€“39 and 40â€“55 are the arms with their fingers.

  Idle motion is about 1.5 cm on the head bones.
- **Head bone: 12**, possibly 13. The user's first-person offset put the camera about 14 cm in front of it.
- **The "confirmed" check was the wrong test.**
  - The pointers found to the arrays sit in structures whose +0x9C reads 521â€“527, not 111, and their holders change
    every scan (render-side references, not the skeleton state).
  - The one "CONFIRMED" hit was an unrelated 8-bone object.
  - The skeleton state's own pointer wasn't found: its +0x70 may point before the first valid matrix.

  This doesn't block the camera: the arrays can be located by position when first person is switched on, and the
  fresh buffer picked each game frame by which one changed.
- Near-plane test: pressed (15 / 30 / off / 15 cm); visual result not yet reported.

## Step 2: head-bone camera (this build)

- **Automatic locate** (`bone_scan.cpp`). While first person is on, the hook publishes the offset camera's position
  (which the user placed just in front of the head) as a hint. The Present thread scans around it, in 4 ms slices,
  once per second until found (every 5 s after a miss), and picks the pair of equal-length pose buffers whose bone 12
  agree to within 10 cm and sit nearest the hint.
  - Log: `HEADCAM: skeleton located -- pose buffers ... (N bones), head bone 12 X cm from the offset camera`, with the
    success tone.
  - If both buffers are unreadable for 0.5 s (level load), it locates again.
- **Per game frame** (`HeadBoneEye`, camera hook, once per frame):
  - reads bone 12 and the root (bone 0) from both buffers; the fresh one is the buffer whose head changed since the
    previous frame;
  - eye = root + smoothed(head âˆ’ root) + eye offset (cm, along the game camera's horizontal forward / right, and world
    up).

  Smoothing is `head_smooth_ms` (100). The root carries locomotion unsmoothed, so running doesn't make the camera lag.
  The camera's orientation is unchanged (game camera plus headset). The offset camera is the fallback until located.
- **Keys:**
  - Ctrl+Alt+Home toggles head-bone / offset camera.
  - While it's active, the offset keys move the eye offset, 1 cm per press (repeating while held): PageUp/PageDown
    forward, Alt+PageUp/PageDown or Ctrl+Up/Down up, Ctrl+PageUp/PageDown or Ctrl+Left/Right side.
  - Ctrl+Alt+0 saves them in `[firstperson]`.
- **Defaults:** eye 15 cm forward of bone 12, 0 up, 0 side (about where the user's offset camera was). The head isn't
  hidden yet: the near value at +0x10C had no visible effect (the user saw no change at 15 or 30 cm), so the
  renderer's near plane is elsewhere.
- **Log every 10 s:** `HEADCAM (last N game frames): fresh buffer by change -- exactly one / both / neither;
  unreadable; largest camera step between frames`.
- **Offline** (`scratchpad/bonetest`): double-buffered 94-bone arrays updated on alternate frames while Lara walks
  1 cm per frame. It locates them from the hint; the eye follows the fresh buffer's root exactly (0.000 cm), gets the
  15 cm offset, and the 2 cm head bob is smoothed. All pass.
## Run 1 of the head-bone camera (2026-09-24 22:15â€“22:21)

**User:** "that worked". It locked after ~30 s, then stayed locked; the initial lock was already at eye level.

- Fresh-buffer choice: **exactly one buffer changed in every game frame** (e.g. 720 of 720), 0 unreadable. The
  double-buffer reading is unambiguous.
- Largest camera step between frames 5.4â€“7.3 cm, about running speed at 72 fps (two spikes of 16 and 24 cm, and one
  after the toggle).
- Slow locate: the first tries found no bone 12 within 60 cm. When found, it was **54.6 cm** from the offset camera
  (the offset camera moves with the run push and the orbit). **Now:** the locate scans 150 cm around the hint and
  accepts the head within 150 cm, still choosing the nearest pair.
- The user tried eye offsets from 15 cm down to 1 cm and ended at 6 cm (not saved); 15 cm is kept as the default.

## Step 3: hiding the head (this build)

The near value at +0x10C did nothing, so the first attempt collapses bones instead.
- **Ctrl+Alt+H** cycles through off; head + head end + jaw (12 13 19); + ponytail (12â€“19); + neck (11â€“19).
- Each game frame the chosen bones' rotation rows are zeroed (scale 0, position kept) in the fresh pose buffer, just
  after we read it, so vertices skinned to them should shrink to the bone's position.
- Unknown: whether the renderer consumes the buffer after our write (then it works); whether face bones (eyes, lips)
  are separate bones that would need collapsing too; and what the hair simulation does.
- At each locate, the log now lists bones 8â€“20 and the slots around the end of the valid run (position relative to the
  head, row lengths, w), to see whether face bones live there.
## User changes (2026-09-24)

- **No eye offset by default:** `eye_forward_cm` defaults to 0, so the camera locks to the head bone itself.
- **Recentre on level load**, as F8 does, for both head tracking and AER. A level load is a menu break during which
  the game's playing-state left 5 (3 = loading, 2 = main menu); a pause menu stays at 5. The head camera also
  re-locates Lara's skeleton after a load, because the pose buffers are reallocated. Log:
  `Menu: a level was loaded -- recentred.` and `HEADCAM: locating Lara's skeleton again (level loaded)`.
## Run 3 of the head-bone camera (2026-09-24 22:30): the wrong skeleton

**User:** the camera bounced and got stuck on the ground.

**Log:**
- At the very first gameplay frame after the load, the locate accepted a **361-bone** array pair 137 cm from the
  offset camera. The 150 cm radius from the previous build let it in.
- From then on "exactly one buffer changed" held in only 3â€“23 frames per 10 s window (720 of 720 in the good run),
  with 21â€“325 unreadable frames and camera steps of up to 8.8 m.
- The loss check (36 unreadable frames in a row) never fired, because the reads failed intermittently.

The recentre was not involved. It only resets the headset's reference yaw and position.

**Fixes:**
- **Lara-shaped only:**
  - 80â€“130 bones (Lara has 94);
  - the head 1â€“2 m above the root and within 60 cm of it horizontally (standing);
  - a matching pair of buffers is **required** (no single-array fallback).
- **Wait for 3 s of steady gameplay** (no menu or loading) before locating.
- **Health check while locked:**
  - over each 72-frame window, fewer than 54 frames with exactly one buffer changed, or 3 jumps over 1.5 m, drops
    the lock: `HEADCAM: lock DROPPED -- ...` plus the failure tone. The offset camera takes over and a new locate
    follows;
  - a single frame with a jump over 1.5 m is not used.
- **Offline:** a decoy pair of 361-bone arrays 2 cm from the hint is ignored and Lara's pair chosen. When the buffers
  stop updating, the lock drops within two windows (88 frames here).
## Run 4 (2026-09-24 22:45): head lock solid; bone collapse doesn't hide the head

**User:** "lock to headbone working very well. very immersive as all animations such as ducks, stumbles, ledge grabs etc
keep the camera glued. unfortunately no dice with the head hide".
- Locate: one miss (only one Lara-shaped array matched), then locked 4 s later. Every window since shows exactly one
  buffer changed per frame (716 of 716, 720 of 720, ...), with 0â€“2 unreadable frames.
- **Bone collapse (Ctrl+Alt+H), all three sets: no visible change.** The pose buffers are the game's world-space
  pose, not what the renderer skins from: skinning uses its own palette, built before our write.
- The bones after the 94 valid ones hold no face bones (slot 94 is scaled Ã—36, some sit 13 m away: props or
  attachments). The face is probably a separate rig.

## Step 4: the renderer's near value (this build)

The Phase 4 reader probe listed the code that reads the camera's FOV each frame; decompiled in
`near_plane_decompiled.txt` and `nearfar_decompiled.txt`:
- **`FUN_1409d6f30`** is the camera manager's per-frame update. For each camera slot (stride 0xD50) it calls our
  hooked camera write (`FUN_140164300`) and then stores:
  - `[-0x22]` FOV = struct +0xB0 + a correction, pinned-horizontal below 16:9 (`2Â·atan(tan(fov/2)Â·2Â·0.8889/aspect)`
    when the aspect is under 1.7778): **our FOV model 1, from the code**;
  - `[-0x21]` the aspect;
  - `[-0x1E]` = **`FUN_1409cc3a0`**: a per-area value (level data +0x214, blended across overlapping areas and camera
    modifiers +0x48), default 1.0;
  - `[-0x1D]` = `FUN_1409cc680`: a zoom ratio, tan(fov2/2)/tan(fov/2) clamped to â‰¤ 1.
- **`FUN_1409cc3a0` is the near-plane candidate.** Probe (`src/near_probe.cpp`): MinHook with a prologue check.
  - It logs `NEARPROBE (5 s): calls, last/min/max returned, multiplier | camera +0x100 plane`.
  - **Ctrl+Alt+Z** multiplies its result: off / Ã—2 / Ã—4 / Ã—8 / Ã—16 / Ã—0.5.
  - If it's the near plane (or a scale of it), her head and nearby walls get clipped as the multiplier rises. If
    something else changes (LOD, fog, exposure), the user reports what.
- The old +0x10C test is retired.
## Run 5 (2026-09-24): FUN_1409cc3a0 is a detail/LOD scale

**User:** the multiplier "is changing some sort of object pop-in or detail (could even be tessellation levels)
without hiding the head". So `[-0x1E]` is a per-area LOD/detail scale, not the near plane. The hook is removed.

## Step 5: search the camera manager's memory for the near value (this build)

The camera struct's culling plane says near = 5 cm, and writing it changed nothing, so the renderer takes its 5 from
elsewhere. The camera manager keeps a 0xD50-byte block per camera, and our struct sits inside it (at +0x1E0 from the
block pointer `FUN_1409d6f30` walks).
- **Ctrl+Alt+Z:** dumps [struct âˆ’ 0x800, struct + 0x1000) to `vrmod_camslot_<time>.txt` and lists every float equal
  to exactly 5.0.
- **Ctrl+Alt+X:** steps through those candidates. The current one is set to 30 on every camera write (right after the
  game's) and every Present; the previous one is restored to 5, and after the last one all are off.
  `NEARPROBE: candidate i/N at struct Â±0x... = 30` is logged.
- A candidate that hides her head, and clips walls closer than 30 cm, is the near plane.
## Run 6 (2026-09-24 23:09): the near plane found

- The slot dump found 4 floats equal to 5.0: struct âˆ’0x278, âˆ’0x270, âˆ’0x78 and +0x10C.
- **Candidate 1, struct âˆ’0x278, set to 30: "appears to remove her head and shoulders".**
- Layout in the dump:

  | Offset from the struct | Value | Meaning |
  |---|---|---|
  | âˆ’0x278 | 5 | **near, cm** |
  | âˆ’0x274 | 550000 | **far, cm (5.5 km)** |
  | âˆ’0x270, âˆ’0x26C | 5, 550000 | last frame's copies |
  | âˆ’0x268 | 1.66 | the FOV the game uses |
  | âˆ’0x264 | 1.3868 | the aspect |

  In `FUN_1409d6f30` terms: `[-0x26]`, `[-0x25]` near/far, copied to `[-0x24]`, `[-0x23]` each frame, then
  `[-0x22]` FOV and `[-0x21]` aspect. The culling planes in the struct (+0x100) are derived from these, which is why
  writing +0x10C did nothing.

## First-person near clip (this build)

- While first person is on with the head camera locked, the camera hook writes `[firstperson] near_cm` (default
  **12 cm**) into the near and its last-frame copy after every game write. Otherwise it puts the game's own value (5)
  back: third-person moments, cutscenes, the offset camera.
- Before each write it checks that near is in (0.5, 100) and far > 1000; a different layout is left alone and logged
  once.
- **Ctrl+Alt+PageUp/PageDown** changes it by 1 cm; Ctrl+Alt+0 saves it.
- Why 12 cm: the camera is at the head bone, the centre of the head, and the face reaches about 10â€“12 cm in front of
  it. 30 cm also removed the shoulders. Walls closer than the near distance are cut too, but her head's own collision
  keeps them further away than that.
## Run 7 (2026-09-24 23:18): near clip works; the bow on her back

**User:** "its working good". The near clip hides the head. But **the top of the bow** (on her back, reaching about a
foot above her head) bobs forward into view; "a cleaner solution would be to hide the weapon on her back".
Ctrl+Alt+PageUp seemed not to work.
- Log: only the decreases arrived (12 â†’ 6 cm with Ctrl+Alt+PageDown). No Ctrl+Alt+PageUp press reached the mod, so
  something on the user's system takes that combination.
  **Added:** Ctrl+Alt+] / Ctrl+Alt+[ for near Â±1 cm (the plain `[` `]` FOV keys and the Ctrl-only world-scale keys
  ignore Ctrl+Alt).
- A good lock was dropped (23:20:11) when the game paused Lara's animation for 27 frames ("neither" buffer changed).
  **Fixed:** only unreadable frames and "both changed" count against the lock (12 or more in 72 frames), plus
  repeated 1.5 m jumps. Harness: a 150-frame pause keeps the lock; unreadable data drops it after 37 frames.

## Hiding the bow: attachment-slot experiment (this build)

Collapsing body bones had no visible effect, because the skinned body uses its own palette. A rigid attachment like
the bow may instead be placed from the pose buffer later in the frame. Each buffer has 17 slots after the 94 body
bones (94â€“110; slot 94 is scaled Ã—36, others sit near her back, some 13 m away).
- **Ctrl+Alt+H** cycles: off â†’ collapse slot 94 alone â†’ 95 â†’ â€¦ â†’ 110 â†’ all 94â€“110 â†’ off.
- Both buffers, every game frame.
- Each step logs the slot's position relative to the head bone.
## Run 8 (2026-09-24 23:30): bow slots, a failure loop after an examine, FOV after the examine

**User:**
- The bow didn't disappear on any slot (94â€“110, or all of them): it's positioned independently of the pose buffer.
- Near raised to 35 cm and saved (Ctrl+Alt+] worked); that hides the bow's top, but her arms too.
- After examining an object (a guided camera), the view stayed third person with a wrong FOV that recovered after
  ~10 s. Switching to first person by hand gave repeating failure tones.

**Log and causes:**
- **Failure loop.** First person came back 25 s after the head camera was last used, with Lara elsewhere. Every
  frame was rejected as "a jump over 1.5 m" against the stale position; the lock dropped. The locate found **the
  same buffers again**, so nothing was reset, and it dropped again, about every second.
  **Fixed:**
  - the reader starts fresh on every new lock (a lock generation counter, even for the same buffers) and after any
    250 ms without use;
  - a jump is not shown but becomes the reference, so if she really was moved (a teleport) the next frame agrees and
    is accepted.

  Harness: the same buffers re-locked 3 m away are followed at once (99 of 100 frames; the first is the fresh
  start); a 5 m teleport is accepted within 2 frames without losing the lock.
- **Third person for 25 s.** `OnInteractStart` has a 25 s timeout, and none of its end events fire for an examine.
  **Fixed:** closing a menu ends the interaction's third person, since an examine always ends by closing its menu.
- **FOV after the examine.** The camera manager's final FOV (struct âˆ’0x268) is our FOV **plus a game-side FOV
  correction** (slot +0x144), pinned-horizontal below 16:9. The examine camera left a correction that blended back
  over ~10 s, and our declared frustum assumed zero.
  **Fixed:** at the first camera write of each game frame, the hook reads the final vertical FOV and aspect (struct
  âˆ’0x268 / âˆ’0x264, the previous frame's) into that frame's AER record. The declared frustum is taken straight from
  them: tanV = tan(fov/2), tanH = tanV Ã— aspect. Model 1's prediction stays the fallback; e.g. 1.4123 rad at 1.3868
  gives 1.6594, matching the dump's 1.6599.
  The PHASE6 line now also counts eye-frames where the final projection differed from the model by more than 0.5 %
  (a game FOV correction in action).
## Run 9 (2026-09-24 23:45): examine without a menu; near saved at 24 cm

**User:**
- The re-lock race is fixed; the head lock was instant.
- An examine switched to third person at the right FOV at once (the final-FOV fix; PHASE6 shows 0 corrections
  needed during it).
- It didn't switch back after ~20 s, so first person was re-engaged by hand.
- Near saved at 24 cm. It must clear the back of the body and the quiver when she turns 180Â° with a stick flick, so
  "ideally the bow and quiver could be isolated".

**Log:** this examine posted `OnInteractStart` only. No menu opened (so the new menu-close end didn't apply) and none
of the known end events fired; third person ran to the 25 s timeout.

## This build

- **Interaction end discovery** (`cinematic_watch.cpp`). For 40 s after `OnInteractStart`, every other event posted
  on the game's event bus is logged the first time its id is seen: `EVENTDISCOVER: new event id 0x... args=...`, with
  the time. The one at the end of the examine becomes the end event.
- **Bow / quiver finder** (`bone_scan.cpp`), with the head camera locked:
  - **Ctrl+Alt+B** scans for 4Ã—4 transforms within 70 cm of the head bone and behind it (camera axes), excluding
    Lara's pose buffers. Lone matrices count too. The list goes to `vrmod_attach_<time>.txt` (up to 60, nearest
    first).
  - **Ctrl+Alt+H** steps through them. The active candidate's matrices get their rotation rows zeroed on every game
    frame and every Present; if it's the one the bow or quiver is placed from, that object vanishes.
## Run 10 (2026-09-25 20:10): the finder crashed the game

- Ctrl+Alt+B found only **3** lone transforms behind the head:
  1. 27 cm away, 21 cm below the head;
  2. 50 cm away, 46 cm below;
  3. 59 cm away, to the side.
- Collapsing candidate 1 had no visible effect. **Collapsing candidate 2 crashed the game about 1 s later.** Zeroing
  an arbitrary object's rotation feeds invalid values to game code (physics, matrix inverses) or writes into memory
  the game has since reused. **The collapse write is disabled** (`AttachApply` returns at once); Ctrl+Alt+B still
  lists candidates.
- The bow and quiver evidently aren't placed from simple CPU-side 4Ã—4 transforms near her back (3 candidates in
  total, none of them the bow).
- Strings: `AGInstanceIsVisible` exists, but its evaluate method (`FUN_14081f230`, `visibility_decompiled.txt`) is a
  **line-of-sight test** (camera frustum plus a ray cast), not a render-visibility flag. Other hits:
  `evHide_DFA_Mesh`, `UIHideWeaponSelect`, `INPUTACTION_Combat_WeaponHolster`.

**Open for the bow and quiver:**
- how the game hides gear itself (early in the game she has no bow; some cutscenes hide it), traced from the
  equipment/inventory side; or
- skipping the gear's draw calls at the D3D12 level.

Both are larger pieces of work. Until then, the near clip is the compromise (the user's saved value).
## Interaction timeout (2026-09-25)

The user: an examine finishes after about 5 s. `OnInteractStart` now holds third person for **6 s**, down from 25; a
menu-based examine still ends when its menu closes. Longer interactions are expected to be covered by the
game-steered-camera rule. The event discovery stays in to find a real end event.

## Stabilisation (2026-09-27)

The view's rotation comes only from the headset (head aim), so what makes first person feel unsteady is the head bone's
positional bob: stride bounce, sway and fore-aft lean from Lara's animations.

- `[firstperson] stabilise=1` (default): the head's offset from the root passes a dead-zone follower before the
  smoothing. The followed offset moves only when the head leaves a band around it: `stab_vertical_cm` (6) up/down, and
  `stab_horizontal_cm` (6) in the horizontal plane (fore-aft and sideways together). Stride bounce and sway stay inside
  the band; crouching, climbing, sliding and leaning go past it and come through, trailing by the band width.
- The followed offset is smoothed over `stab_smooth_ms` (150) instead of `head_smooth_ms`. The root stays unsmoothed, so
  locomotion, jumps and falls don't lag.
- Toggle: gamepad LB + left stick click, or Ctrl+Alt+S (live; Ctrl+Alt+0 saves). The HEADCAM line (every 10 s) reports
  the vertical travel of the raw head against the camera over the window: the difference is the bob removed.

## Gear hiding through the game's own switch (G1, 2026-09-28)

- `evHide_DFA_Mesh` is a dead end: its posters (ConcealmentComponent, NpcComponent) hide a Flash UI prompt
  (`UIPromptConcealed` / `UIPromptState` via FUN_140bcb1f0 / FUN_140bcb300), not a mesh.
- The animation graph's **`AGInstanceHideUnhide`** node (vftable 0x1410fec58, execute FUN_14082bb50) hides an instance
  and optionally its children (array at instance +0x90/+0x98, or the attach chain +0xE8 -> +0xB8 first / +0xC0 next)
  through FUN_14085a250, which calls **FUN_1406e2510(instance, visible)** and FUN_140447ae0 (collision/physics flags).
- **FUN_1406e2510:** the parent (handle at instance +0x88) has, at component index `*(u32*)0x1416BB118`, a list of its
  attachments (count +0x20, 0x18-byte entries at +0x28); the entry whose +0 equals the child's +0x68 handle gets its
  **visible byte at +0x16** set. That byte is the whole switch.
- `gear_hide.cpp` reads Lara's list (Lara = the camera director's slot-0 object, as the aim finder reaches her) on the
  game thread each frame. It writes 0 into the visible bytes while first person is on with the head camera locked and
  she isn't aiming (an AimCameraController in the camera blend stacks), and puts the game's values back otherwise,
  always on the instance they were hidden on.
- `[firstperson] hide_gear=1` (default). Ctrl+Alt+G cycles all -> only #1 -> ... -> off; each step and every 30 s logs
  `GEARHIDE` lines with each attachment's handle, raw bytes and the component classes of its instance.

## Gear hiding, take 2: Lara's child instances (G1b, 2026-09-30)

- The attachment-list switch (FUN_1406e2510) doesn't apply to Lara: her instance (PlayerComponent, InventoryComponent,
  CameraComponent, MeshInstanceComponent, 122 components) has no component at index `*(u32*)0x1416BB118` (34).
- What hangs off her: instances whose parent handle (+0x88) is hers. 2026-09-30: five 48-component and one
  121-component and one 35-component drawable objects (weapons / outfit pieces / axe?), plus lights, FX, sound emitters
  and ~35 six-component markers without drawables.
- Hiding one: FUN_140447ae0's visual part on its DrawableComponent (component 8): if not already hidden, MarkDirty
  (FUN_14043ac70: queues its render items for the renderer, under the game's lock); set bit 0 of +0xD8; ClearVisible
  (FUN_140436ac0: clears bit 0x100 on each render item). Showing: MarkDirty, clear bit 0, SetVisible (FUN_14044a1c0).
  Collision and lights (the rest of FUN_140447ae0) are left alone. Prologues checked before use.
- `gear_hide.cpp`: a background thread lists Lara's drawable children every 2 s; the game thread hides them while first
  person is on (head camera locked) and she isn't aiming, and shows what it hid otherwise. Ctrl+Alt+G cycles all ->
  only #1 -> ... -> off; `GEARHIDE:` lines list each child (#, handle, component types, names found near its object and
  mesh components) on every change.
- Probe 2026-09-30: every drawable's visibility map (+0x60/+0x70) is EMPTY (Lara's too), so ClearVisible/SetVisible touch
  nothing; weapons-like children and Lara have 1 item in the dirty map. Drawable-path hiding had no visible effect.
- FUN_14085a250 has a second path: if the instance's +0x68 handle -> entry -> component 9 -> +0x48 is set, it sends the
  instance event 0x3EC04619 (argument = hide) via FUN_1408e6d70(*(0x1436AAF30), instance, event, hide) -- the game's
  send-event-to-instance -- and skips the drawable. gear_hide.cpp now uses that path for such children (listed as
  "hide event" in GEARHIDE lines), the drawable path for the rest, and probes every 5 s while anything is hidden.
- Result 2026-09-30 (hide-event path): all 13 children report hidden (flag set by the game's own handlers) and stay
  hidden, yet nothing visible changed. The bow/quiver on her back are NOT child instances -- most likely parts of Lara's
  own model (like the head). The 13 children are presumably inventory weapons and attached FX/lights/sound emitters.
  hide_gear now defaults to 0. Next: find Lara's per-mesh-part visibility (e.g. what the game toggles when she draws
  the bow), or skip the gear's draw calls.
- GEARWATCH 2026-09-30 (table-wide drawable hidden flags, weapon swaps bow -> pistol -> rifle -> shotgun -> bow): only
  world objects changed (groups of 8 scenery instances toggling together -- culling); nothing parented to Lara. The
  game doesn't hide the holstered bow with the drawable flag -- likely a mesh swap on the weapon slot or a mesh part of
  Lara's model. Four game-switch attempts exhausted (attachment list, drawable flag, hide event, watcher); the watcher
  is no longer run. Remaining options: dynamic near clip (raise it only when the gear can be in view), or draw-call
  skipping.

## Speed-dependent near clip (2026-09-30)

User: the gear on her back comes into view when she runs; standing still only the head needs hiding. So the first-person
near clip is `near_cm` standing still (user's value 11) and `near_run_cm` (48 after the first test: she leans well forward) running, blended between
`near_run_from_mps` (1.5) and `near_run_full_mps` (2.8; her run is ~3.0 m/s, walk ~1.4, sprint peaks ~4.5 -- the first thresholds 2.0-3.5 only reached ~20 cm at a normal run) of her horizontal speed. Speed = skeleton root motion per game
frame, smoothed over 80 ms (she accelerates almost at once), teleports ignored. Ctrl+Alt+[ / ] adjust whichever value is
in use (running when she runs, standing otherwise); Ctrl+Alt+0 saves. The HEADCAM line reports speed now / max and
the near clip in use.

## Head-lock cue and a wider locate (2026-09-30)

Playthrough report: switching into first person sometimes gave no three-tone head-lock cue, and the view looked off
(FOV or depth). The log showed two separate things:

- **Silent but fine.** The three tones only played when the skeleton was freshly located. When the lock from earlier was
  still good (most switches) first person used the head bone without a sound. Now the tones play every time first
  person comes on and the head-bone camera is placing the camera (400 ms after the on tones), or on a fresh locate.
- **A real miss.** Once, right after a scripted camera, four locates in a row found no Lara-shaped skeleton within
  200 cm of the offset camera, so the offset camera (2.85 m along the boom) was used -- the "wrong depth" view. Heads
  were found 71-185 cm from the offset camera this session, so 200 cm is marginal. After a failed locate the radius
  now widens to 300 cm, then 400 cm (the nearest double-buffered pair still wins), and after three failures in a row the
  failure tones play once, so a missed lock is audible.

The FOV was not involved: the PHASE6 frustum check reported 0 mismatching eye-frames in all 104 reports of the session.
The FOV model (how the game's FOV value maps to the frustum at non-16:9 aspects, `\` cycles 0-3) is chosen once at
startup and never changes during play.

## Head hiding by draw skipping (2026-10-07, RenderDoc capture)

RenderDoc 1.46 capture of one flat frame (Photo Mode, close on Lara's face; the VR mod and Cheeky set aside; the Epic
Online Services overlay and the Steam overlay off -- the EOS overlay `EOSOVH-Win64-Shipping.dll` crashed the game under
RenderDoc in ntdll; DLSS and async compute off). 1546 draws, 100 dispatches. Pixel history on the face, eyes, lips,
hair, neck, bow and quiver, then every draw of those models listed (RenderDoc Python scripts, not included):

- **Lara's head is its own model**: every face (71256 indices), mouth (22254), eye (2352), lid/eye (540, 7836, 72),
  hair-strand (4704) and hair-card (13536) draw uses ONE index buffer (capture Buffer 129358). The neck below comes from
  the body model (its own index buffer, 38865 indices); the bow and quiver are separate meshes too.
- Skinning is in the vertex shaders (BLENDWEIGHT/BLENDINDICES inputs); the bone palettes are per-draw constant buffers
  (head 1280 bytes = 20 matrices, current and previous frame).
- Passes: material pre-pass (RGBA8 + RGBA16_UINT), shadow map (1024x1024 depth only), main colour pass
  (R11G11B10 + RGBA8 + RGB10A2), translucent passes (hair cards, eye surfaces). Hair is mesh hair -- no strand system.

**Implementation (this build):** `ui_probe.cpp` hooks IASetIndexBuffer (d3d12.h vtable 43) and tracks per command list
the index buffer and the number of colour targets. The head buffer is learned from the face draw's signature (71256
indices from 0 -- LOD 0, always the case with the camera inside the head); every indexed draw from it into colour
targets (not the depth-only shadow map: her shadow keeps its head) is skipped while first person with the head camera
is active and nothing is suspended. `[firstperson] hide_head` (default 1), **Ctrl+Alt+F5** toggles, `HEADHIDE (10 s)`.
Other outfits/heads may have another face index count -- the log shows whether the buffer was learned.

**Test 1 (2026-10-07 21:36):** head hidden (~12200 head draws skipped / 10 s = all its passes). Hair still seen: the
capture has no strand-hair system (no large non-indexed draws besides HUD quads), so it's in the body/outfit model --
whose index buffer holds five meshes (35694@0, 8088@35694, 14145@43782, 38865@57927, 762@96792; a hair pixel at the
neck came from 38865@57927).

**Earlier build:** hide groups (head, bow, quiver, body-part test), each learned from a signature draw. Bow (7962@1158)
hidden in first person except while aiming (`CameraAimingNow`); quiver (14898@12300) hidden in first person; both
under Ctrl+Alt+F5 with the head. Ctrl+Alt+F6 cycles the body-part test 1..5 (one part hidden) to find the hair.

**Strand hair (2026-10-07):** the hair is TressFX strands in their own 32-bit index buffer (learned from any colour draw
of 150000+ indices from 0; the count varies with LOD). Three things draw it into the picture, all skipped with the head:
the strand colour draws (full and half resolution), the full-screen hair composite after them (first 3-vertex
DrawInstanced in a colour pass after a skipped strand draw -- its hair targets are only discarded, never cleared, so it
re-blends stale hair), and the strands' depth-only pass into the scene depth (left in, fog and lighting showed a
see-through hair imprint). That depth pass is told from the hair shadow map by the depth format (scene D24S8, shadow
map D16; CreateDepthStencilView hooked, device vtable 21) -- matching it by strand count broke after a cutscene.

**Bow and quiver in other levels:** a second level drew neither signature (other gear models). Signatures are now a list
(`kSignatures`, several per group). **Ctrl+Alt+F6 = gear finder:** the first press lists the models drawn in the 32 draws
before and 16 after the head's face draw (Lara's draws come as a block in the G-buffer pass), excluding head, hair and
body; each further press hides the next candidate, the log naming its parts (count@first); after the last, off.

**Scripted-camera debounce:** a scripted base camera (Simple/Spline/Anchored...) must hold 0.4 s before first person
yields to it. With ray-traced shadows on, one area flickered the base camera to Simple every few seconds, flipping
first person off and on; flickers are counted in the log (`CAMMODE: N scripted-camera flicker(s)`).

**Head lock from the face draw (2026-10-07):** after one checkpoint load the lock took ~10 s: the offset camera (the
search centre) jumped 2.8 m between two places while the load settled, and her spawn pose failed the standing/crouching
checks. Now `face_track.cpp` reads her head position from the face draw's own constants (RenderDoc capture: root
parameter 2 is the VS CBV table b0..b13; b1 `CB_Skinning` holds world-space float3x4 skinning matrices, b0
`WorldBuffer` the model's World matrix, b11 `ModelDrawable` the face bounds (-9,-10.8,108)..(9,10,150)). Skinning matrix 0
applied to the face-bounds centre gives the head point, accepted only if both matrices are rotations and the point is
within 2.5 m of the model root. To resolve b0/b1 to memory the mod follows CBV descriptors per heap slot
(CreateDescriptorHeap 14, CreateConstantBufferView 17, CopyDescriptors 23, CopyDescriptorsSimple 24), the table bound at
root parameter 2 per command list (SetGraphicsRootDescriptorTable 32) and every buffer Map (ID3D12Resource vtable 8). First test: every read failed -- the constants live in a GPU-only buffer filled by one CopyBufferRegion a frame from
an upload buffer (capture event 10873), so copies out of mapped buffers are followed too (CopyBufferRegion, list vtable 15).
With a fresh head point the locate searches 80 cm around it with no pose checks and retries every 0.7 s; without one it
falls back to the offset camera as before. Once a second the lock is checked against the drawn head; five misses over 60 cm in a row re-locate. Second test: the copy is recorded AFTER the draws, so the face draw now only queues its two addresses and they are read when the copy filling them is recorded; a reading is published only if the previous one agrees within 50 cm. Log: `FACETRACK (10 s)` (reads, failures, head point, locked bone vs drawn head distance).

## Body turn (2026-10-11)

Turning the view 180 deg with the stick looked back through Lara's own body (her body doesn't turn while she stands).
Two attempts that didn't hold up: a stick limit relative to her facing (`look_limit_deg`, now default 0 -- her facing
shifts with idle and turning animations, so the stop moved around) and hiding her body when looking back
(`hide_body_deg`, default 0 -- it keys on the outfit model's draw signature, which changed with her outfit).

**Finding her facing (facing_probe.cpp, Ctrl+Alt+F12, five rounds):**
1. Scan for her model root's position (face draw World matrix) and test-turn every matching rotation by 15 deg: only
   her render matrices (SceneEntity +0xA30) turned her, for one frame -- rebuilt from gameplay every frame.
2. Record the Player object (RTTI `Player`, her position at +0x550) for 8 s while she turns: a unit direction vector
   at +0x1B20 and heading angles at +0x3C8 / +0x468 follow her facing in the same frame.
3. Writing those: no effect, replaced within a frame -- copies.
4. Hardware write watchpoints (aim_watch.cpp) on them + Ghidra (`tools/ghidra_facing/DecompileFacing.java`,
   `facing_decompiled.txt`): the Player update FUN_1409a77a0(instance) copies the instance's angles (+0x30: about X, Y, Z,
   radians) into the Player through its set-rotation FUN_1409ad290 and the position (+0x10) through FUN_14099ead0.
5. Her instance = Player +0x33E8 = a static world-object pool entry (DAT_14301d0c0, 0x120 each; Lara was entry 3):
   writing its Z angle (+0x38) turned her model 15 deg and the game kept it (8/8 and 79/79 Presents).

**body_turn.cpp:** finds Lara's pool entry (near her model root, whose Player component --
`*(*(*(inst + 0xD8) + DAT_1416b99c8 * 8) + 0x2A8)` -- points back at it), and on the game thread (from head drive),
standing still on the walking camera, eases her Z angle towards the view's stick yaw once it is more than
`body_turn_deg` (35) away, at up to `body_turn_speed` (200 deg/s), until within 3 deg. Her facing = Z + 180 deg (her
model's -Y axis). Not while she moves, aims, or outside head drive; the camera doesn't follow, so the view never moves
by itself. She pivots (no turn animation). Fixed RVAs for build 0x64AC5C46 -- to be turned into patterns
(game_addresses.cpp) for the release. Confirmed in game 2026-10-11 ("It works!").
