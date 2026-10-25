# Workstream A — Aim where you look

Status: **proposed 2026-09-25** (user decision: "aim where you look, since this is standard in community VR mods").

**Problem.** The game draws its crosshair at the screen centre and fires along its own camera (mouse or stick). The
mod adds head rotation on top of that camera, so the view centre, and the crosshair drawn there, points where the
head looks while shots go along the game camera. There's also a smaller parallax error: the first-person camera sits
at the head bone while the game aims from its third-person camera, about 1° at 10 m for a 25 cm offset.

**Approach.** Head yaw and pitch drive the game's camera controller each frame, and the mouse or stick adds on top. The
mod's own composed rotation then covers only the remainder (prediction, roll, per-eye pose). The game's aim, its
crosshair and its world-anchored prompts then all follow the view.

**Known (Ghidra, `aimvars_decompiled.txt`, `aiminput_decompiled.txt`):**
- `player_cameracomponent_orbityaw` / `orbitpitch`, `player_camera_inputyawrotation` / `inputpitchrotation` and
  `player_aim_yaw` / `aim_pitch` are **animation-graph variable registrations** (`FUN_1407c3e40`, IDs into
  `DAT_14362b6xx`). They're outputs for Lara's animation, not the camera's source.
- The look-input actions (`INPUTACTION_Camera_Aim_LeftRight` / `UpDown` / `Rate` / `Dir`) and the sensitivity
  settings are registered in large tables (`FUN_140be0b30`, `FUN_140b2ae00`). Their users are one more indirection
  away.
- The camera manager's per-camera block (`FUN_1409d6f30`) holds the camera object pointers the controller lives
  behind; our camera struct is at block +0x1E0.

**A0 — discovery (next).** A runtime finder (Ctrl+Alt+Y):
- walks pointers from the camera manager's block around our struct, a few levels deep with a budget;
- snapshots the floats of each object reached while the user turns the camera left, right, up and down;
- reports the values that track the game camera's own yaw and pitch (from the game's pose, before the head is
  added): slope ±1 in radians or degrees, with a constant offset modulo a full turn.

**A1:** verify a candidate by writing it (the camera turns by the written amount, and aiming follows).
**A2:** head-driven yaw and pitch, mouse and stick additive, the composed rotation reduced to the remainder;
recentre (F8) handling; cutscenes and scripted cameras left alone.
## A0 — the finder (this build)

`src/aim_finder.cpp`, **Ctrl+Alt+Y**:
- **Roots:**
  - the gameplay camera object: RCX at the camera read hook, i.e. `FUN_14049a220`'s first parameter; the asm detour
    now saves it to `g_cameraOwnerAddress`;
  - the camera manager block (struct − 0x2B0);
  - the globals `DAT_1416bb1f0` and `DAT_1436aba00` used by the manager's update.
- **Walk:** pointers to depth 3, up to 4,000 objects of 4 KB each. Pointer checks use a sorted table of readable heap
  regions built once per walk: committed, read/write, not image, not GPU-mapped.
- **Snapshots:** the camera hook publishes the game's own camera rotation (before our head delta). A snapshot of all
  objects is taken each time that rotation has been still for 30 frames at a new angle (at least 5° from every
  earlier snapshot), with a tone, up to 6 (90 s limit).
- **Report** `vrmod_aimfind_<time>.txt`: every float that tracks the game camera's yaw or pitch, i.e. value = ±angle
  (radians or degrees) + a constant, allowing a full-turn wrap for yaw, or a forward-vector component. Each hit comes
  with its pointer path from the root and its value per snapshot.
- **Offline** (`scratchpad/aimtest`): a fake camera object →[+0x40]→ controller with yaw in degrees + 90, wrapped to
  0–360 (crossing the seam), and −pitch in radians, driven through six held angles. Both are found, with the right
  paths ("gameplayCamera(RCX) ->[+0x40] +0x118" / "+0x11c"), and the noise value is ignored.

### Test procedure (A0)

In gameplay, standing still, in third or first person:
1. Press **Ctrl+Alt+Y** (one tone).
2. With the **mouse**, turn the camera and **pause for about a second** after each turn; each pause gives a tone (a
   snapshot). Turn left about 45°, right about 90°, look up, look down, then left again. Six tones in total, then the
   success tone (values found) or the failure tones.
3. Don't move Lara during this; only the camera.
## A0 results (2026-09-25 21:49, six snapshots: yaw range 158°, pitch range 67°)

- 4,000 objects walked in 41 ms; 57 matches, all **copies of the final camera orientation**:
  - **−yaw in radians, offset exactly 0**:
    - in our camera struct at **+0xBC** (the 3.13 seen in the early CAMSTRUCT dump);
    - in 0x1A0-strided camera records reached through the camera manager (`cameraManagerBlock ->[+0x450]` +0x36C,
      +0x50C, +0x6AC, +0xE0C) and through `DAT_1416bb1f0 ->[+0x68] ->[+0x0]` (+0xEC, +0x28C, +0x42C);
  - **forward-vector components**: rows and columns of camera matrices in the same records.
- **No pitch angle anywhere**, and **no yaw with an offset**. A controller's own angles usually carry an offset
  (relative to Lara or world north), so these look derived. Either the controller isn't reachable from these roots
  within depth 3, or it doesn't store plain angles.
- Static tracing of the look input: the actions are registered under fixed indices (`Camera_Aim_LeftRight` 0xAA,
  `UpDown` 0xAB, `Rate` 0xAC, `Dir` 0xAD, table `0x1436AE870`). The functions that load both 0xAA and 0xAB are
  registration tables (animation variables and script natives), not the camera. That route is a long chase.

## A0b — write test of the yaw copies (this build)

Some cameras integrate from last frame's stored state. If one of the found values is that state, changing it turns
the camera and the change sticks; a copy is overwritten on the next frame.
- After a finder run, pressing **Ctrl+Alt+Y again** tests each distinct yaw-angle address: wait until the camera is
  still, add +15° of yaw (in the value's own unit and sign), then measure the game camera yaw 15 and 75 frames later.
  The report goes to `vrmod_aimtest_<time>.txt`, with a tone per candidate.
- Single angle values only; nothing like the matrix zeroing that crashed before.
- **Offline:** the source (the camera follows the stored yaw) moved **15.00°**, both at 15 and at 75 frames; a copy
  overwritten from the camera each frame moved **0.00°**.

### Test procedure (A0b)

1. **Ctrl+Alt+Y**, then the six mouse/stick turns with pauses as before; wait for the success tones.
2. **Ctrl+Alt+Y again**, then leave the mouse and stick alone (Lara still). The tool works through the candidates
   (about 1.5 s each, a tone per candidate, the success tones at the end). If the view suddenly turns 15° during
   this, that candidate is the one.
## A0b results (2026-09-25 21:59): all 8 are copies

Every candidate moved the camera **0.00°** at 15 and at 75 frames. The slight movements the user saw were idle
animation (the head-bone camera).

**Why:** the camera director (`DAT_1416bb1f0`, `camdirector_decompiled.txt`) keeps a hash map of camera slots:
- buckets at +0x68, bucket count at +0x80, nodes chained through +0x0 with the slot key at +0x10;
- `FUN_140170cf0` only copies a slot's finished transform from node +0x1D0 (the records with the yaw copies);
- the **active camera object** behind a slot is `*(*(node + 0x520) + 0x20)` (`FUN_140170bd0` → `FUN_140188dc0`),
  depth 4 from the director, just past the first run's depth 3.

## A0c (this build)

- The finder now also roots at the **slot-0 camera object** and the slot-0 node's +0x520 object, walked first.
- Depth 4, budget 6,000 objects.
- It logs `AIMFIND: director ..., slot-0 object ..., slot-0 camera ..., gameplay camera (RCX) ...`.
- Same procedure as A0b: Ctrl+Alt+Y with six turns, then Ctrl+Alt+Y again for the write test.
## A0d–A0f results (2026-09-26): every orientation value is a copy

- A0d rooted the finder at the camera instance's components (handle from the slot, instance table `0x14301D0C0`,
  camera component index at `0x1416B99D4`): Lara's instance has 122 components, the camera component is index 25.
- A0e added forward-vector and quaternion matching; A0f made it NaN-safe (NaN-filled objects had matched every
  float: 4,445 candidates), capped the write test at 60 and made Ctrl+Alt+Y abort it.
- Write test (18:01): 26 candidates — 8 quaternions (camera component records +0x350/+0x4F0/+0x690/+0xDF0 and
  `->[+0xC78]`/`->[+0xD40]` blends), 9 −yaw angles (incl. `instanceComponent ->[+0x2B0]` +0x3C, offset π), 9 forward
  vectors. **All moved the camera 0.00°.** Still no standalone pitch anywhere.

## A0g — write watch (this build)

Values can't find the controller, so follow the data flow instead. After a finder run, **Ctrl+Alt+W** arms hardware
write watchpoints (write-only, 4 bytes, all threads, 1.5 s) on up to four of its matches (one per root, then per
object; angles first). Each writing instruction is logged with its count, registers and XMM0–5 at the first hit,
and the SOTTR.exe return addresses on the stack — `vrmod_aimwatch_<time>.txt`, Ghidra addresses. The writers are
then decompiled to trace where the orientation comes from, back to the controller or the look input.
Offline: a writer thread's store was caught at its exact instruction.

### Test procedure (A0g)

1. In gameplay, **Ctrl+Alt+Y** and the six turns as before; wait for the success tones (skip the write test).
2. **Ctrl+Alt+W** while standing still, then turn the camera slowly with the stick for ~2 s. One tone at the start,
   the success tone after 1.5 s.

## A0g results (2026-09-26 18:14) and A1 — the record matrices

Write watch (`vrmod_aimwatch_20260926_181456.txt`, decompiles in `aimwatch_writers_decompiled.txt`,
`aimwatch_copy_decompiled.txt`):
- The camera component's +0x2B0 record and the manager slot are written by **`FUN_140164300`, a plain 0x1A0-byte
  record copy**. The component's is called from **`FUN_140167df0`, the component's camera blend stack**: it updates
  each active camera mode's controller (entry +0x20 object; vtable +0x28 update, +0x60 active, +0x250 weight), blends
  their poses (entry +0x30; `FUN_140161130` / `FUN_140165690`), post-processes (`FUN_140166230`) and copies the result
  to +0x450 and +0x2B0 (+0x5F0 keeps the previous +0x450). The controller state is behind the virtual update, which
  is why no value matched.
- `instanceComponent ->[+0x2B0]` +0x3C is written by `FUN_1407f9c90`, Lara's movement-heading filter (not the camera).
- **The record layout:** +0x00 a camera->world matrix, +0x40 its inverse, +0x80 position, +0xA0 quaternion, +0xB0 FOV,
  +0xBC −yaw. `FUN_140164300` copies the matrices *before* the block our write hook replaces, so in the slot our
  head-composed quaternion sits next to matrices that still hold the game's own orientation. The renderer follows
  the quaternion (head tracking works); whatever reads the matrices never sees the head.

**A1 (this build):** after the head is composed, the slot's matrices are brought in line with the final pose: their
axes are rotated by the same world rotation that took the game's quaternion to ours, row 3 becomes our position,
and +0x40 is recomputed as the rigid inverse. Every precondition is checked on each write (axes orthonormal, row 3 =
the game position, rows = the game quaternion's axes, +0x40 = the inverse); if one fails nothing is changed.
**Ctrl+Alt+A** toggles it (on by default). The log shows the matrix layout once and `AIMSYNC (30 s)` counts.
If the aim reads these matrices, it now follows the head; if the view drifts or spins, the controller reads them
back — Ctrl+Alt+A turns it off.

## A1 result (2026-09-26 18:30): the aim doesn't read the slot

Layout confirmed (rows +X right, −Z up, +Y forward; row 3 position; +0x40 the inverse) and 1,742 records synced in
30 s with no check failing — but shots didn't follow the head, with the sync on or off. The aim reads something
upstream of the slot. The sync is kept but **off by default** (no key).

Side finding: firing switched first person to third ("game-steered camera"): recoil/shake changes our camera after
the write. Now a **rotation-only** change (> 0.05°, < 1 cm) counts as recoil/shake and is not steering (squeezing
through gaps moves the camera without turning it). `GAMECAM (250 ms)` lines log each burst's size.

## A2 — upstream injection test (this build)

The camera component's record (+0x2B0) is written by the record copy called from the blend stack (return address
0x140168233, verified at install; the copy is a leaf, so the asm hook passes `[rsp]`). **Ctrl+Alt+A** adds a fixed
**+20° yaw** there: quaternion, matrices (if their layout checks out) and the −yaw copy (+0xBC, set in the source).
- If the view jumps 20°, the slot is fed from this record; if shots still land on the crosshair, the aim reads this
  record or something downstream of it — and head yaw/pitch injected here is "aim where you look".
- If the view jumps but shots go 20° off, the aim comes from the controller itself.
- `AIMINJECT:` logs the record-copy callers after 5 s (return addresses, counts).

## A2 result (2026-09-26 18:38): the view follows the component record, the aim doesn't

+20° into component +0x2B0: the view jumped 20°, shots went the original way. The caller table (12 slots) was full.
Decompiles (`aim_copycallers_decompiled.txt`):
- `FUN_14017b3f0` (camera director) copies per slot component +0x450 → slot +0x40 and +0x2B0 → slot +0x1E0 (our
  struct), or +0x930 / +0x790 in another branch.
- `FUN_140168270` is a **second blend stack** (five mode lists at +0xF30..+0xF98): result +0x930 (before the +0xC78
  post-process) and +0x790 (after), previous frame in +0xAD0; its weight goes to +0xC70.

Recoil: a shot re-adjusts our camera on every write for about a second (≤ 0.03°, ≤ 1.4 cm), like a squeeze. The
third-person switch now needs **sustained** steering (6 windows = 1.5 s in a row).

## A2b (this build)

Ctrl+Alt+A (or K alone, for testing with the gamepad) injects +20° into all four current-frame records: +0x2B0, +0x450, +0x930, +0x790 (return addresses
0x140168233 / 0x140168213 / 0x1401687A3 / 0x1401687CC, each verified at install). Caller table 32 slots.
If shots still go the original way, the aim reads the camera controller itself.

## A2b result (2026-09-26): the aim reads +0x450 — but aim modes absorb it

With +0x450 injected (the second stack doesn't run in gameplay), aiming turned Lara and the crosshair 20° and shots
landed on the crosshair: **the aim follows the component's +0x450 record.** But each aim press added another 20°
(she starts from where she last turned): an aim mode starts from the component's last output, so an offset put
there is baked into the game's state. Head yaw injected there would drift.

## A2c (this build)

**K** injects +20° only into the **director's copy of +0x450 in our slot** (the record 0x1A0 before our struct,
`FUN_14017b3f0`), leaving the component alone; the view isn't rotated. Ctrl+Alt+A keeps the component mode. The
first 5 s log line names the slot record's writer.
- Shots 20° off the view centre, the same every aim press → the aim reads the slot copy: inject the head there.
- Shots unchanged → the aim reads the component directly: inject there and undo the offset at aim-mode changes.

## A2c result (19:39): nothing was injected

No record copy writes struct − 0x1A0, so K changed nothing. The log did show the **second blend stack running while
aiming** (+0x930 / +0x790 copies x97 in 5 s) with two extra director copies (0x14017C19C / 0x14017C1B2).

## A2d (this build)

K now injects every copy whose **source** is the component's +0x450 or +0x930 (learned from the blend stacks' own
copies) — the director's copies into its slots — except the component's previous-frame copies (0x140167E42,
0x1401682DA). The caller log (8 s after turning a test on) lists which copies were injected.

## A2d result (19:49): the director's copies aren't what the aim reads

K injected the director's copies of +0x450 / +0x930 (0x14017C05E, 0x14017C19C, 0x140171491; thousands of writes)
and nothing changed. The aim reads the component's records themselves.

## A3 — aim where you look (this build, K)

- Each frame the head's yaw and pitch (`AerHeadAimDelta`: midpoint of the eye views, recentred, roll dropped;
  camera-local yaw about +Z after pitch about +X) go into all four component records (q' = q·d, matrices and −yaw
  kept consistent).
- Our slot hook takes exactly that d off again (q·d̄) straight after the game-frame detection, before first person,
  AER and everything else, so the view is built as before. Undo only when the component's view record (+0x2B0 or
  +0x790) was injected for this frame.
- Suspended in menus, cutscenes and third-person moments. AER only. `HEADAIM (30 s)` log line.
- Open question: an aim mode starting from the component's last output (A2b build-up) would absorb the head offset
  at each aim press — watch for the view jumping when aiming starts.

## A3 result (2026-09-26): aim follows the head

"Aim follows head, although shots land a little to the left of the crosshair."

## A3b — parallax (this build)

First person looks from the head bone while the game aims from its third-person camera position, so the shot ray
runs parallel to the view ray but from beside it. At each slot write the eye's offset from the game camera is taken
in the game camera's axes; with head aim on in first person the aim records (+0x450, +0x930) are moved to the eye
(position and matrix row 3, inverse kept). The view records keep the game position (the first-person head hint
is derived from it). The `HEADAIM (30 s)` line now shows how often the aim records were moved and the eye offset
(right, forward, up; cm).

## A3b result (2026-09-26): shots land on the crosshair

"Working properly this time." Remaining: random first→third switches — the 'game-steered camera' heuristic
(tiny adjustments to our writes, ≤ 0.05°, ≤ 1.5 cm) fires in ordinary play.

## Camera modes (this build) — a direct signal for game-directed cameras

The exe carries RTTI names for its camera controllers: Exploration, Aim, Stealth, Spline, CameraBone, Anchored,
Force, Simple, Iso, Free, Hmd, PhotoMode, MapView, Modelview, Dummy (`...CameraController`). The component's blend
stacks hold one entry per mode (controller at entry +0x20; +0x1C = used in this frame's blend). Every Present the
lists are read (guarded) and a `CAMMODE:` line is logged whenever the set changes: base entry, stack 1 lists,
stack 2 lists, `*` = used, stack-2 weight (+0xC70). Next: map situations (squeeze, climb, interact, cutscene) to
modes and switch to third person from the mode instead of the adjustment heuristic.

## Camera-mode run 1 (2026-09-26 21:04, first 10 minutes: cutscenes, wall climb, ledges, squeezes)

214 `CAMMODE` lines. Cutscenes: base Spline / Simple / Anchored, or stack 2 Dummy. Interactions: stack 2 CameraBone
(or base Spline). Wall crawl: an extra stack-2 Exploration (already covered by its event). Aiming: stack 2 Aim.
The opening's scripted walk: base Anchored. Squeezes: no distinct class (Exploration with other settings?).
The adjustment heuristic switched to third person in ordinary play (21:25:36 while aiming, 21:27:32 walking).

**This build:** third person ('scripted camera') while a scripted class is in use — Spline, Simple, Anchored,
CameraBone, Dummy, Force — as the base entry, or in stack 2 while stack 2 dominates (weight > 0.5); held 0.5 s. The
adjustment heuristic only logs. For squeezes: with event logging on, every unnamed event is logged (`EVENTSEEN`,
first sighting and after 5 s of silence), and **J** stamps `MARK n` in the log with a tone.

## Camera-mode run 2 (2026-09-26 22:39: a combat save, then a new game with J marks)

- No unwanted switches. Every scripted camera (CameraBone after aiming = a takedown-like moment, Simple, Anchored,
  Spline, Dummy) switched to third person at once.
- Squeezes (marks 1–2) are cutscenes to the game (`evCinematicStarted`); mark 3 fell in an interaction. Covered.
- Head lock: at 22:42:19 the camera component was replaced (menu state change: a reload), so Lara's pose buffers
  moved; the re-lock took 23 s because the locator only accepted a standing pose (head ≥ 1 m above the root) while
  she was crouched. Now 0.4–2 m.
- Opening intro: cinematics 22:44:36 (base Exploration + Dummy), 22:45:18 (Simple), 22:46:48 (Anchored → Spline),
  22:47:58, 22:49:15; gameplay from 22:49:56. The user wants the title cards / plane / breakup without head tracking
  (the set's edges show), up to the cut to Lara in the gorge — which cinematic that is still needs a J mark.

## Marked run (2026-09-26 23:13): takedowns, intro, first lock

- **Takedown** (mark 3, 23:15:53): only Exploration cameras — invisible to the mode watch. Events: 0x723A3742 fired
  once at the start (also 23:03:47 in the earlier run, nowhere else); 0x982814C3 at the start and the end (5 s).
  Now third person ('finisher') from the first to the second (8 s timeout).
- **Opening intro** (mark 5 correct, 23:21:38): 42 s + 90 s cutscenes, then the third; the gorge cut comes 21 s into
  it, with no event or camera-class change, identical in both new-game runs. `INTRO`: a load from the main menu
  (`lc2_MainMenu2Game`, within 60 s) straight into a cutscene → head tracking off (re-applied each frame) until 20 s
  into the third cutscene, or 3 s of gameplay. Only after **New Game** (2026-10-10: a checkpoint starting with a
  cutscene, continued from the title, went flat): `evMenuStateChanged`'s payload is `{ phase, from menu, to menu }`, and
  the load (to the loading screen 0x0E) comes from the difficulty screen 0x32 for a new game, from 0x31 for Continue.
  Logged as `INTRO: left the title menu for a NEW GAME / for a saved game`. (Save files can't tell them apart: the game
  reads every save at the title screen and none when Continue is chosen.)
- **First lock** 6–7 s after a load or restart (2 s settle + misses at 150 cm while the camera settles; found at
  100–138 cm): settle 1 s, search radius 200 cm.

## Head aim as a setting (2026-09-27)

`[aim] head_aim=1` (default on) turns aim-where-you-look on at startup in render mode aer; K toggles it live and
Ctrl+Alt+0 saves the state. The takedown, intro and first-lock changes were confirmed in play ("all looks good").

## Rope arrows at range (2026-10-01)

**User:** aiming at a rope-arrow anchor from afar with head aim on, the reticle flicks between the rope-arrow and the
normal reticle while charging, the circle doesn't turn red, and a normal arrow flies. Close up in third person it works.
With head aim off (K) the rope lock works but follows Lara's bow (the game camera), not the crosshair; with head
tracking off it's the flat game.

**Ghidra (`ghidra_scripts/DecompileRttiAndStrings.java`):**
- `AGGetTargetUnderReticle` (animation-graph node, vtable 0x1411060A8, update FUN_140883330) only *reads* the player's
  targeting object T = *(Lara's component [*(u32*)0x1416B99C8] + 0x2A8): target +0xBF8 / +0xC28, ray +0x1B10 / +0x1B20.
- T is written by FUN_1409b9ae0, called every frame while aiming from FUN_1409bf230. Its ray is built from the camera
  component's **aim record +0x450, or +0x930 while stack-2 weight +0xC70 > 0** (FUN_140170e30 on a copy) -- the records
  head aim turns. Raycast FUN_140982fc0 (same record choice).
- Rope target: FUN_1406e7e40(candidate's markup component, T, 5, ray start, ray hit) -- the markup point (flag 0x10)
  closest to the ray start among those whose distance to the ray segment is within the point's radius. The candidate
  object comes from T+0x3418's list (FUN_140984620) or, failing that, T+0x33F0 +0x518 / +0x520.
- A branch in both uses DAT_1416BB1F0 +0x310 / +0x380 instead of the record when FUN_14041ffe0() && FUN_14041f5e0()
  (an override source; not yet identified).

**Probe (this build):** `RETICLE:` lines on every change of "rope target present" while aiming (frame, AER eye, ray
yaw/pitch, hit distance, object, stack-2 weight) and `RETICLE (1 s aiming)` summaries (rope-target frames per eye,
changes, ray yaw range) -- to see what alternates.

## Rope arrows: cause, and a gamepad toggle (2026-10-01)

**User tests:** the crosshair turns into the rope icon over a rope target and Lara's model follows the crosshair, but on
release she snaps to "the true direction the arrow fires from" and a normal arrow flies; aiming with the head at a far
target while standing next to another rope target fires into the adjacent one; aligning her arm with the stick alone
doesn't bring up the icon; near targets work (head and game camera nearly agree).

**Cause:** head aim turns the camera component's records only. The rope auto-target is picked from candidate lists
sorted relative to the player's position and an aim yaw from her aim state (FUN_1409add80 comparator, reference
FUN_14098a3c0 / FUN_14098dc00), with a staggered visibility check (FUN_1409b0fc0 / FUN_1409b0de0) -- and the
projectile goes to that pick, or along the game camera. Those follow the game camera, not the turned records.

**Real fix (next):** the head must turn the game camera itself (the original A2 plan), so every system agrees.

**Stopgap:** gamepad LB + right stick click held 0.6 s toggles head aim (like K); a short click still toggles first
person, now on release.

**User (2026-10-02):** last time only head aim off *and* head tracking off worked -- with head aim off the game aims
along its own camera, but the crosshair stays at the view centre (where the head looks), so the shot can't be lined up.

**Stopgap, part 2 (this build):** while aiming with head aim off, the HUD (its centre is the crosshair) is shifted to
where the game camera's forward falls in the frame's view -- d^-1 applied to forward, projected with the frame's
frustum (OpenXRPredictFrustum), `UiProbeSetAimOffset` (NDC, after the HUD scaling). Head tracking stays on; line the
crosshair up with the right stick as in the flat game. World prompts move with the HUD while aiming.

## The real fix: head drives the game camera (2026-10-02, started)

Goal (the original A2 plan): the head's yaw/pitch turn the game camera itself, the stick adds on top, and the view's
own composed rotation covers only the remainder -- so rope auto-aim, the arrow's direction, Lara's facing and world
prompts all agree with the head. Needs the state the right stick integrates: write it each frame as body + head, and
recover the body from what the game changed since our last write (the user's stick).

**Step 1 -- CAMSTATE probe (this build, Ctrl+Alt+C):** A0 walked from the camera manager and found only copies; it
never looked inside the camera controllers. The probe snapshots the camera component and every controller in its blend
stacks (RTTI: Exploration, Aim, Stealth, ...; entry +0x20) for ~12.5 s and correlates every float with the game
camera's yaw (unwrapped) and pitch -- the game's rotation with head aim's d already undone -- and logs the best
matches with their slope (1 = radians, 57.3 = degrees). Then a write test on the best candidates.

The exe also has an `HmdCameraController` class -- worth a look as a ready-made head-driven controller.

**CAMSTATE result (01:37, 12.5 s of right-stick turns):** `ExplorationCameraController` **+0x12D4 / +0x12D8 = yaw**
(radians, slope 1.000, r 1.0000) and **+0x12EC / +0x12F0 = pitch** (radians, slope -0.992 -- negated), one block
updated by FUN_1408d8010(controller + 0x12D4) in the controller's update (slot 5, FUN_1408c9750). Everything else is
copies in the camera component (+0xE0C, the records' quaternions, ...).

**Write test (this build):** Ctrl+Alt+E adds 20 deg to the yaw pair, Ctrl+Alt+I tilts the pitch pair 10 deg up, once,
on the game thread; `CAMWRITE` lines log the game camera's yaw/pitch before and 0.25 / 1 / 2 s after -- a source keeps
the turn, a copy snaps back.

**Eye pairing:** the follow's "reversed twice -> off" guard tripped 4 s after a load (the offset genuinely flips while
the game settles) and fell back to re-routing DLSS only -- swapped eyes for the session (user, 01:35). Removed;
reversals are only noted in the log line.

**Aim camera (CAMSTATE 02:33, changes-based):** `AimCameraController` **+0x904 / +0x908 = yaw** (slope 0.995; small
values -- relative, not absolute) and **+0x91C / +0x920 = pitch** (negated, 02:29) -- the same layout as Exploration's
block (+0x18 to pitch). The aim camera only dominates while the bow is drawn (stack-2 weight 1.0; holding aim alone is
0.02).

**Head drive, first build (`head_drive.cpp`; `[aim] head_drive`, Ctrl+Alt+E):** first person with the head camera,
AER, no menu / cutscene / third-person moment. Every game frame (once; the game writes the camera twice):
1. per controller in use (base entry + used stack-2 entries; Exploration and Aim known): body += state - lastWritten;
   a controller joining (aim start) gets body = state - the head applied last frame (it starts from the output);
2. body yaw/pitch (world) = the game camera's minus the dominant controller's (state - body) -- dominant = Aim while
   stack-2 weight > 0.5;
3. target = the view centre's direction, body * d (the head's yaw/pitch as AER composes it), as world yaw/pitch; every
   controller gets body + (target - body), both twins, pitch negated;
4. the view base becomes the body rotation, `Rz(-offY) * q_game * Rx(-offP)`; AER adds the full head pose as before.
Head aim's record injection stands down while head drive is active (it re-takes over in third person / suspended).
Released (head part taken off the states) when it stops.

## Aim camera aligned to the view before the bow is drawn (2026-10-09)

**User:** with head drive, the reticle only turns red (NPCs) or into the rope icon with the crosshair off the target,
also with one eye closed (not a stereo effect); on charging the view jumps to where Lara is aiming, and from then on
the crosshair is right; rope mode stays engaged from the offset pick.

**Cause:** the targeting ray uses stack 2's record (+0x930) whenever stack-2 weight > 0, but holding aim the weight is
~0.02: the view (and the crosshair at its centre) is ~98 % stack 1 (Exploration), while the targeting follows the aim
controller. Head drive writes body + head into both, but the aim controller's yaw/pitch are relative to its own
reference, so it points elsewhere; at full draw it dominates and the view jumps to it.

**Fix (head_drive.cpp):** while the aim controller is in use but not dominant (0 < weight <= 0.5), its body is nudged
each frame by half the difference between the view target written last frame and the direction its record +0x930
(quaternion +0xA0) points now (errors over ~35 deg are ignored). The targeting ray then follows the crosshair, and the
hand-over at full draw has nothing to jump. `HEADDRIVE (30 s)` logs the corrections and the largest error.

**Result (17:00):** no difference -- the aim record already pointed within 0.68 deg of the view (19 corrections), so
direction wasn't it. **Real cause:** the ray's *origin*. A3b moved the aim records (+0x450, +0x930) from the game's
third-person camera to the eye, but that lived inside head aim's record injection, which head drive stands down
entirely -- so with head drive the targeting ray started ~3.45 m behind and ~24 cm beside the eye, parallel to the view
(the offset grows up close; one-eye test failed too). **Fix:** the eye move (`HeadAimMoveToEye`) now also runs under
head drive; only the rotation stays off. The alignment above stays as a harmless safety net.

**Result (17:05):** the rope icon lines up with the rope target. Two left: (1) on charging a rope shot the game swaps the
aim camera for a **Simple** base camera (and a few unnamed ones) until the walking camera returns -- read as a cutscene,
so first person flipped to third and back while lining up; (2) the crosshair jumps on charging: head drive only knows
the Exploration / Aim controllers and lets go, while the Simple camera already carries the head turn (it points where
Lara aimed), and the view adds the head again. **(1) fixed:** a scripted camera within 1.5 s of the aim camera is the
rope-shot camera -- first person kept and counted as aiming (bow shown) until the walking camera is back (or 20 s);
`CAMMODE: rope-shot camera` lines. Real cutscenes still switch via their events. (2) next.

**Rope-shot hold (2026-10-09, user agrees with the double-head cause):** while the rope-shot camera is in use, head drive
doesn't let go: it keeps the view correction of the last driven frame (the head offset at the charge, taken off the game
camera -- no jump) and stands down only its writes, so head aim (A3) takes the aim over with a relative head turn:
d' = h0^-1 * d, h0 = the head turn at the charge (camera-local yaw then pitch). Crosshair and targeting then both follow
the head's change since the charge. The controllers are forgotten without writing (their states keep the head part);
when the walking camera returns they re-join with body = state - head (g_hY kept) and driving resumes. A menu / cutscene
/ third-person moment ends the hold. Log: `HEADDRIVE: rope-shot camera -- holding ...` / `... over -- driving again`.
First version never engaged: the rope-shot flag is set on the Present thread a frame or two after the Simple camera
takes over, and that first frame without a known controller released the drive (head part zeroed). Now the hold takes
the head values of the last driven frame if the rope camera shows up within 1 s of it. Confirmed in game 2026-10-09
(12 holds, crosshair aligned at and during the charge).
