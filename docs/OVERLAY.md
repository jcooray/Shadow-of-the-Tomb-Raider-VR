# In-headset overlay

A panel in front of you with the mod's status and settings. **Menu + Guide** on the gamepad opens and closes it: hold
the Guide (Xbox) button, then press Menu. **Ctrl+Alt+O** on the keyboard does the same, for when the Guide button
doesn't reach the game (Steam Input or the Xbox Game Bar can take it). The game keeps running; it doesn't see the
gamepad while the panel is open, or until every button pressed in it has been released.

## Using it

| Button | In the overlay |
|---|---|
| D-pad up / down (or left stick) | choose a line |
| D-pad left / right (or left stick) | change the value (held: repeats) |
| A | switch a toggle, run an action |
| Y | back to the default |
| LB / RB | previous / next page |
| B, or Menu + Guide | close |

Changes apply at once where the mod can, as a preview. Leaving a page (LB / RB) or closing the panel with changes on
it opens a confirmation listing them ("Render resolution: as set in the game -> 80 %"): **A** keeps them (written into
`vrmod_settings.ini` in place: comments, order and other keys stay as they are), **B** undoes them (every value put
back, including keys a preset changed that no line shows), **X** stays on the page. Only lines changed from the panel
count: a value that moves by itself (first person switched off by a cutscene) isn't listed or undone. Lines marked
"(after a restart)" are saved but take effect at the next start.

## Pages

- **Status** (read only): mod build, game build and how many game addresses / event ids were found, the OpenXR runtime,
  headset, refresh rate and eye image size, render resolution, SteamVR's resolution setting (not used), render mode, frame rate (over the last second), first / third person and
  whether head tracking is paused, eye tracking, gamepad, settings saved.
- **View:** first person, recentre, aim where you look (aer), head turns the game camera, stick turn speed limit,
  look-down limit, body turn angle and speed, hide body when looking back, neck model, eye position forward, camera
  stabilisation, hide Lara's head, world scale (aer; as a percentage of the true scale, below).
- **Rendering:** apply minimum-spec settings / apply standard settings (presets, below), render resolution (restart,
  below), foveated rendering off / fixed / eye tracked (restart; each mode writes its sharp region size to Cheeky's ini,
  and a new install's automatic render resolution follows it), foveated region size (restart; Cheeky's Width / Height,
  in 5 % steps keeping the mode's shape), DLSS 5 (restart; eye tracked only, with `nvngx_dlssnr.dll` beside the game),
  eye tracking through SteamVR (restart), render mode (restart), frame warp (aer), in-engine cutscenes stereo / mono,
  headset refresh rate (the rates the runtime offers through XR_FB_display_refresh_rate), overscan, head pose prediction,
  flat screen mode, and last DLSS history per eye (troubleshooting; on by default and in both presets).
- **HUD:** game HUD smaller and centred, HUD size, HUD height, HUD in its own layer, overlay distance and size.
- **Controls:** the gamepad and keyboard controls; view change tones.
- **Support:** mark this moment in the log, save a bug report folder (`vrmod_report_<date>_<time>` in the game folder
  with vrmod.log, the two previous logs and the settings), save settings now, version, log file, licences.

## Presets

| Setting | Minimum spec | Standard |
|---|---|---|
| Render mode | alternate eye | alternate eye |
| Render resolution | 70 % (or lower if already lower) | 100 % |
| Overscan | 1.00 | 1.08 |
| Frame warp | on | on |
| DLSS history per eye | on | on |
| In-engine cutscenes | stereo | stereo |
| Head pose prediction | 3 frames | 3 frames |
| Headset refresh rate | the lowest offered of at least 72 Hz | unchanged |

Minimum spec is for GPUs well below the dev system (25-50 % of an RTX 5090). The game's own graphics settings (quality
preset, DLSS mode) still apply on top; lower them as well.

## Render resolution

`[render] resolution_pct`: a percentage of the PIXELS the headset needs for full sharpness at the centre (SteamVR's
convention: 150 % = 1.22 x each side) -- of the target
render size the mod works out from the runtime's recommended size and field of view (the PHASE6 log line; kept in
`render.target_w` / `target_h`). At startup, before the game reads its settings, the mod sets both the fake desktop
size and the game's saved fullscreen resolution (registry `HKCU\Software\Eidos Montreal\Shadow of the Tomb
Raider\Graphics`, FullscreenWidth / FullscreenHeight, Fullscreen = 1) to that share of pixels, sides rounded to multiples of 8.
0 (default) leaves both as they are (`vrmod_fakedesktop.txt` and the game's own setting). The target size is known
after one start in alternate-eye mode.

## World size

The World size line shows how big the world looks, as a percentage of life size: (1 / 1.8) / `world_scale` (head and
eye movement shrink as the world grows). Life size -- 100 %, and the default -- is world_scale 1 / 1.8 (0.556). The
game's units are centimetres (the executable holds -980 / -981, gravity in cm/s^2; Lara's own falls are tuned to 900
units/s^2 and her jumps to 720, measured with the development build's FALLLOG), but at one unit per real centimetre the
world looks far too small in the headset -- the game seems built for its third-person camera. The developer found 1.8
times that true to life, standing and seated alike (2026-10-27); the opening cutscene (Lara in a ravine, unable to look
down at herself) has to look right from the start, so it is the default. Y on the line sets 100 %.

Earlier: real size from Lara's height assuming 1.70 m (world_scale 0.87-0.91), then centimetres (1.0). An automatic size
from the player's eye height above the floor (seated, a bigger world) was tried and removed: the size that looked right
was the same standing and seated.

The first time play starts (once per install, `[overlay] world_asked`), the overlay opens at World size with a notice
over it: change it if the world looks too large or too small. A or B dismisses the notice; the overlay stays open.

## How it works

- `src/overlay_pad.cpp`: reads the pads with `XInputGetStateEx` (xinput1_4 ordinal 100 -- the only call that reports
  Guide), all connected slots combined. Hooks `XInputGetState` in each XInput DLL the game has loaded (xinput1_4 /
  1_3 / 9_1_0, checked once a second): while blocked the game gets a connected pad with nothing pressed, and Start is
  always removed while Guide is held on that pad, so Menu + Guide never opens the game's pause menu.
  `OverlayPadGameReads()` counts the game's calls -- the Status page shows whether the game reads XInput at all (it
  also has a Windows.Gaming.Input path; if it used that, blocking would need a hook there).
- `src/vr_overlay.cpp`: the pages (items with value / change / activate / reset), gamepad navigation, and drawing: GDI
  into a 1280 x 800 BGRA bitmap (CPU only -- no GPU device of its own), only when the shown text changes (the Status
  page at most twice a second); the panel's rounded corners go into the alpha channel.
- `src/openxr_integration.cpp` (OverlayEnsure / OverlayUpload / OverlayPlace): a swapchain of its own (8-bit sRGB,
  B8G8R8A8 preferred), the bitmap copied through a mapped upload buffer into the acquired image on the game's queue
  (own allocator, list and fence; render target -> copy dest -> render target), submitted as a quad layer after the
  projection layer, with source alpha. It is placed overlay.distance_m in front of the head's level forward direction,
  12 % of that below eye height, facing you, when it opens.
- `src/settings.cpp`: the getters read live values (`SettingGet` / `SettingSet` / `SettingsSaveChanged` /
  `SettingsWriteKeys`), so a change is seen by everything that reads its setting per frame. Modules that keep their
  own switch (AFW, HUD layer, head drive, head aim, head hiding, stabilisation, per-eye DLSS, world scale, eye offset,
  HUD placement) are set through small accessors. `render.mode` keeps its startup value for the session.
- `tools/overlay_preview.cpp` (`vrmod_overlay_preview <folder>`): renders every page to BMP with stand-ins for the game,
  and checks that a change is saved -- for layout work without the headset.

New settings: `[overlay] distance_m` (default 1.0) and `width_m` (0.9). Defaults for a new install, the welcome message and the restart message: docs/RELEASE.md, "New-user defaults".
