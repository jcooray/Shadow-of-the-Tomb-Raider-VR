# Motion controls — research and plan (2026-10-31)

Goal: play Shadow of the Tomb Raider with VR motion controllers instead of (or alongside) a gamepad, with bow and gun
aiming by pointing. Staged so that each stage ships on its own; nothing here is built yet.

| Stage | What the player gets | Release |
|---|---|---|
| 1 | Motion controllers work as a gamepad: no gamepad needed at all | 1.1 |
| 2 | Bow and guns aim where the right hand points, with a laser dot | 1.1 |
| 3 | Something visible in the hands (hand models / a floating bow, later Lara's arms by IK) | 2.0 |
| 4 | Gestures (draw the bow by pulling back, axe swings) — only if 1–2 play well | later |

## Prior art: fubar's HitmanAbsolutionVR (LGPL-2.1)

[farmerarmor/HitmanAbsolutionVR](https://github.com/farmerarmor/HitmanAbsolutionVR) has motion-controller aiming in a
first/third-person shooter. Read 2026-10-31 (README, `src/ControllerInput.h`, `src/GamepadMapping.h`,
`src/EngineInput.cpp`, `src/AimOriginMath.h`, comments in `src/EngineWeapon.cpp`). Different engine (Glacier, 32-bit
D3D11, native same-frame stereo, an out-of-process OpenXR host), so only the ideas carry over:

- **Controllers as a gamepad:** one OpenXR action set (aim poses, sticks, triggers, grips, A/B/X/Y, stick clicks, menu)
  with suggested bindings for `oculus/touch_controller` (+ `khr/simple_controller` for aim and menu only). A mapper turns
  the actions into an `XINPUT_GAMEPAD` that a hook on the game's `XInputGetState` returns: grips = LB/RB, triggers =
  LT/RT, sticks and clicks as on a pad, left menu tap = Start, held 1.5 s = Back, **right stick click held + left stick
  direction = D-pad**, right stick click + X = recentre, + Y = first/third person; a short right stick click alone is the
  real stick click. Neutral input when the session isn't focused or the game isn't the foreground window.
- **Weapon aiming** is deep engine work there: the held weapon's world transform is replaced by the controller pose on
  the render path, and the fire routine is fed the weapon's own muzzle direction, preserving the game's ammo / cooldown
  / precision checks. A laser dot replaces the hidden crosshair. Body IK is not implemented.
- `AimOriginMath`: the aim origin is learnt from the relaxed (not aiming) rig and blended by crouch, so aiming doesn't
  change eye height. Relevant if stage 2 moves the aim origin to the hand.

## What this mod already has

- **Gamepad interception (1.0.3):** `overlay_pad.cpp` hooks `XInputGetState` (xinput1_4, xinput1_3) and
  Windows.Gaming.Input's `IGamepad::GetCurrentReading` (vtable slot 8), and blanks both while the overlay is open. The
  same hooks can *supply* a gamepad state: the virtual pad of stage 1 goes in here.
- **Aim separate from the view (A3 / A3b, `docs/AIM.md`):** head aim writes a rotation d into the game's aim records
  (+0x450, +0x930; quaternions and matrices kept consistent) and takes it off the view again, and moves the aim records'
  position to the eye. So targeting, shots, rope auto-aim and Lara's aim pose follow d while the view doesn't. Stage 2
  replaces d (the head's turn) with the right controller's direction relative to the view base, and the eye position
  with the hand's.
- **Head drive (`head_drive.cpp`):** drives the game camera's own yaw/pitch from the head; head aim's record injection
  stands down under it except the eye move. Hand aim must coexist: head drive keeps steering the view, hand aim only
  the aim records.
- **OpenXR action set for eye gaze (`eye_gaze.cpp`):** creates `vrmod_gaze`, suggests the eye gaze profile and calls
  `xrAttachSessionActionSets` for it alone. **A session can attach action sets only once**, so the controller action
  set must be attached in the same call: a small input module creates both sets and attaches them together; one
  `xrSyncActions` per frame with both active.
- **Overlay navigation** reads `OverlayPadRead` (XInputGetStateEx plus the game's own reads); the virtual pad feeds it
  directly, so the overlay works with the controllers too.

## Stage 1 — controllers as a gamepad

**Input module** (`motion_input.cpp`, new): action set `vrmod_controllers` with left/right aim and grip poses, both
sticks (or trackpads), triggers, grips, face buttons, stick clicks, menu. Suggested bindings per interaction profile:

| Action | Touch (`oculus/touch_controller`) | Index (`valve/index_controller`) |
|---|---|---|
| Left stick / right stick | thumbsticks | thumbsticks |
| LT / RT | trigger values | trigger values |
| LB / RB | squeeze values (> 0.55) | squeeze **force** (> 0.55; squeeze/value is finger curl, high just holding) |
| A / B | right A / B | right A / B |
| X / Y | left X / Y | left A / B |
| LS / RS click | thumbstick clicks | thumbstick clicks |
| D-pad | right stick click held + left stick direction (the stick centred at the click; a click while walking is a plain RS) | a swipe on the left trackpad (half its width within 0.5 s) |
| Start (pause) | left menu tap | right trackpad press |
| View (Back) | left menu held 1.5 s | left trackpad press (also right trackpad held 1.5 s) |
| Overlay (Guide + Menu today) | left menu + right stick click | both trackpads pressed together |

Plus `khr/simple_controller` (aim pose and menu only) and, if cheap, `htc/vive_controller` and
`microsoft/motion_controller`. Recentre is already on LB + RB together (1.0.4) and works unchanged.

**Virtual pad:** the mapper's `XINPUT_GAMEPAD` is merged into what the game's `XInputGetState` and WGI reading
return (buttons OR-ed, each stick axis the larger magnitude), so a real gamepad keeps working alongside. Neutral when
the OpenXR session isn't focused or the game isn't in the foreground; nothing while the overlay is open (as now).

**Open question — no physical pad (spike first):** with no gamepad plugged in, XInput slot 0 reports "not connected"
and WGI's `Gamepads` list is empty. We know the game calls `XInputGetState` (Steam Input pads reach the overlay through
it), so returning a connected slot 0 from our hook may be enough — but the game may only use XInput as a fallback when
WGI has no pad, or ignore XInput entirely when WGI is present. Faking a WGI `Gamepad` object is far more work. The first
build tests exactly this with the gamepad switched off.

## Stage 2 — aim with the right hand

- **Direction:** while aiming (LT held / bow drawn) and not in a menu / cutscene / third-person moment, d = the right
  controller's aim-pose direction relative to the view base (yaw and pitch; roll dropped), written into the aim records
  exactly as head aim writes the head's turn. Lara's aim animation then turns her bow / gun towards it by itself.
- **Origin:** the aim records' position moves to the controller (as A3b moves it to the eye), so arrows and bullets leave
  from the hand. Check rope arrows and auto-aim still lock (they use the same records).
- **Crosshair:** the game draws its reticle at the screen centre, which no longer matches the shot. Options: hide it
  (find the reticle draw in the HUD layer, or a game HUD option if one exists) and draw our own dot at the hand ray's
  hit point (the depth buffer we already capture for frame warp gives the distance; the dot is a small quad layer).
- **Head aim / head drive:** hand aim takes over the aim records while aiming; head drive keeps steering the view. With
  controllers idle (gamepad play), head aim behaves as today.
- **Open questions:** does the aim animation follow large hand offsets (pointing 60° to the side), or does Lara turn her
  body — and does that fight body turn (`body_turn.cpp`)? Do precision / auto-aim assist pull the shot back to the
  screen centre? First test: log-only (controller ray vs the aim records' direction), then the write.

## First build (stage 1, spike) — what it does and how to test

1. Creates the controller action set alongside eye gaze (one attach), logs the interaction profile in use per hand and,
   every 10 s, the controller state (`MOTION:` lines).
2. Feeds the virtual pad into the XInput and WGI hooks, merged with any real pad.
3. Test on the Index controllers: walk, jump, aim, shoot, open the overlay; then with the gamepad switched off (the
   no-pad question above). Quest Touch bindings are written but need a player to test.

Licence: Hitman is LGPL-2.1; nothing is copied — the mapper is written fresh for this game's layout.

## Stage 1 results (2026-10-31, Index controllers, user)

- **No gamepad:** "the controllers work perfectly" -- the game reads them through XInput slot 0 (connected once used).
- **With the Elite connected:** at first only recentre worked. The game then reads Windows.Gaming.Input and skips a
  reading whose Timestamp didn't change (an idle pad's doesn't); merged readings now get a later Timestamp when they
  change. Works since.
- **Grips:** Index squeeze/value held LB / RB all session (finger curl); squeeze/force fixed it.
- **D-pad:** edges of a trackpad press were awkward -> swipes (user: "the swipes work"); press = View.
- **Touch layout** (no Quest here): `vrmod_motion_mapper_test` feeds scripted input through the mapper -- 19 checks
  (stick click short / held / while walking, D-pad 4 ways, Menu tap / hold, overlay chord, Index swipes and presses,
  grips). Found and fixed: a held right stick click sent nothing, and a click while walking became a D-pad press.
