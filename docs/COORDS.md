# Coordinate conventions

## OpenXR (reference space LOCAL)

Right-handed: **+X right, +Y up, −Z forward** (+Z back). Positions are in metres. Each `XrView` from
`xrLocateViews` has its own pose (orientation and position; the position includes the IPD offset).

## Foundation engine (Shadow of the Tomb Raider) camera struct

From FransBouma's IGCS SOTTR camera (see `camera_interceptor.asm`), confirmed in this project as noted.

| Field | Offset | Meaning |
|---|---|---|
| position | +0x80 | world position, 3 floats, **centimetres** |
| rotation | +0xA0 | quaternion x y z w, camera-local → world |
| FOV | +0xB0 | vertical FOV, radians |

**Camera-local axes: +X right, +Y forward, +Z up.** The world is **Z-up**. Evidence:
- The first-person offset (`camera_interceptor.cpp`) moves the camera along rotate(q, (0,1,0)) and puts it at
  Lara's head, so local +Y is forward.
- The head-tracking rotation mapping below has been in use in SBS play since before the AER work: looking, tilting
  and turning all track.
- The Phase 5 axis log (Ctrl+X) records the game's base forward vector while the camera is turned with the mouse.
  If the world is Z-up, only its X/Y components change. See "Empirical check" below.

## Mapping OpenXR → game camera-local

    v_game = (x, −z, y)          for a vector v_xr = (x, y, z)
    q_game = (qx, −qz, qy, qw)   for a quaternion (same mapping of the vector part)

- This is a proper rotation (determinant +1), so it preserves handedness. The offline test (`scratchpad/aertest`)
  checks map(rotate(q, v)) = rotate(map(q), map(v)) on 1,000 random cases (worst error 0).
- OpenXR +X (right) → game +X (right).
- OpenXR +Y (up) → game +Z (up).
- OpenXR −Z (forward) → game +Y (forward).

## Per-eye camera (AER, `src/aer.cpp`)

With the recentre reference q_ref (yaw only, keeping gravity) and p_ref (midpoint between the eyes at recentre, F8):

    rotation = base * map(q_ref⁻¹ * q_eye)
    position = base_position + base * ( map(q_ref⁻¹ * (p_eye − p_ref)) * 100 cm/m * world_scale )

- *base* is the game's own camera: follow/orbit camera plus mouse/stick yaw, plus the first-person offset when on.
  The HMD is applied relative to it (the brief's third-person policy).
- Each eye's **own** view orientation is used, never head orientation + IPD.

Offline checks (`aertest`), with identity base unless noted:

| Head motion (OpenXR) | Expected in game | Result |
|---|---|---|
| none | left eye −3.2 cm right, right eye +3.2 cm | ok |
| yaw +30° about +Y (look left) | forward → (−sin 30, cos 30, 0) | ok |
| pitch +20° about +X (look up) | forward → (0, cos 20, sin 20) | ok |
| roll +15° about +Z (right ear up) | right → (cos 15, 0, sin 15) | ok |
| move +0.1 m x, +0.05 m y, −0.2 m z | +10 cm right, +5 cm up, +20 cm forward (± eye) | ok |
| base yawed 90° left, local right offset | world +Y | ok |
| head turned 90° left | right eye 3.2 cm toward the old forward | ok |

## Empirical check (in the headset)

The Ctrl+X axis log records 20 s at 20 Hz:
- the head's OpenXR yaw/pitch/roll and position relative to the recentre reference;
- the same after mapping (game yaw/pitch/roll, and offset in cm right/forward/up);
- the game's base forward vector in world space;
- the eye separation (IPD) and the angle between the two eye orientations (are the views parallel?).

**Result, 2026-09-24 17:02** (`vrmod_axes_20260924_170212`, 361 samples; world scale 0.481 at the time):

| Motion (peak) | OpenXR | Game camera |
|---|---|---|
| look left | yaw 66.9° (pitch 4.4, roll −5.6) | yaw 66.9° (pitch 4.4, roll −5.6) |
| look up | pitch 56.1° | pitch 56.1° |
| tilt head right | roll 31.7° | roll 31.7° |
| head position (e.g. during the look-left) | x −0.119, y +0.050, z +0.106 m | right −5.74, up +2.42, forward −5.10 cm (= m × 100 × 0.481) |

- Every rotation comes through with the same sign and size.
- Positions come through as (x, −z, y) × 100 × world scale.
- **Eye views:** separation 63.0 mm (the user's IPD). The angle between the two eye orientations is 0.000–0.056°:
  the views are parallel on this headset (Pimax parallel projection).
- The game's base forward vector didn't change during the log (the camera wasn't turned with the mouse), so the
  world-up axis wasn't re-measured there. Z-up rests on the first-person offset evidence above.

**In the headset** (user, 2026-09-24):
- tint: red left, green right;
- held-camera stereo check: depth correct;
- head tilt and translation track correctly.

"As good as native SBS in camera movement and 3D perception, with the doubling of horizontal resolution evident."
