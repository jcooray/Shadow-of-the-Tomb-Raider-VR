# Shadow of the Tomb Raider VR

A VR mod for Shadow of the Tomb Raider on PC. You play the whole game in your headset, in full stereo with your head
tracked: in first person from Lara's eyes, or with the game's own third-person camera when you want it.

It's free and open source. If you enjoy it, you can [support its development](#support-development).

<!-- VIDEO: link to the gameplay video goes here -->

## What it does

- **Full stereo 3D, head tracked.** The game renders one eye per frame and the mod pairs them up for the headset
  (alternate-eye rendering). Frame warp builds the other eye's image from the one just rendered and its depth, so
  both eyes get a new image every frame and movement stays smooth.
- **First person from Lara's eyes,** with her head hidden, her body turning to follow yours, and a neck model.
  Switch to the game's third-person camera and back at any time. Cutscenes switch over by themselves.
- **Aim where you look.** Optional, on by default; the camera stick still works as normal.
- **The world at life size,** and you can make it bigger or smaller in the overlay.
- **HUD where you can see it.** The game's HUD is drawn smaller and centred, on its own layer in front of you.
- **DLSS kept working,** at a resolution worked out for your headset. You set it as a percentage in the overlay, like
  SteamVR's own setting.
- **An overlay in the headset** with all the settings. Hold the Guide (Xbox) button and press Menu. Every change shows
  straight away, and you're asked to keep or undo it when you leave the page. There are presets for slower GPUs.
- **Optional foveated rendering** with [CheekyFoveatedDLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS):
  where you look (with an eye-tracked headset) or the middle of your view kept at full quality and the rest slightly
  softer, for less GPU time. With eye tracking, optionally DLSS 5 neural rendering too (experimental).

## What you need

- **Shadow of the Tomb Raider** on PC, DirectX 12. I made it with the Steam version. Setup also finds Epic and GOG
  installs, but I haven't been able to test those; if your copy's code differs, the mod switches off the features it
  can't find and says so in its log.
- **A PC VR headset and an OpenXR runtime.** I use a Pimax Dream Air with SteamVR. Other headsets and runtimes
  (Quest through Link, Air Link or Virtual Desktop; Windows Mixed Reality; Index; Vive) should work but are untested.
- **An NVIDIA RTX graphics card** for DLSS. It hasn't been tried on AMD or Intel GPUs.
- **A gamepad.** Xbox-style; most of the mod is driven from the gamepad and the overlay.

It's demanding: the game renders at the headset's full resolution, one eye per frame, at the headset's refresh rate.
I developed it on an RTX 5090. The first start picks a render resolution for your graphics card (shown as "auto" in the
overlay). On slower cards, also apply the **minimum spec** preset in the overlay (Rendering page) and lower the game's
own graphics settings.

**Virtual Desktop users:** its quality preset multiplies with the mod's render resolution. *Godlike* renders far more
than *Ultra* or *High*; on mid-range cards use *High* or *Ultra*.

## Installing

1. Download `Shadow-of-the-Tomb-Raider-VR-<version>-Setup.exe` from
   [Releases](https://github.com/jcooray/Shadow-of-the-Tomb-Raider-VR/releases).
2. Close the game and run Setup. It finds the game in your Steam, Epic or GOG library, or you can browse to it.
3. Choose foveated rendering. It keeps part of your view at full quality and the rest slightly softer, and only
   the DLSS pass gets cheaper, not the game's own rendering. Timings on an RTX 5090:
   - **No foveated rendering:** your whole view at full quality. Best without eye tracking, where foveated rendering
     means turning your head, not just your eyes, to see sharply.
   - **Fixed:** the middle stays at full quality, the edges are slightly softer. It saves very little GPU time (about
     2 %), so the automatic resolution stays about the same.
   - **Eye tracked:** wherever you look stays at full quality, freeing about 15 % of GPU time, so the automatic
     resolution is about 30 % higher. Needs eye tracking that reaches SteamVR or OpenXR (e.g. Pimax Crystal / Crystal
     Super / Dream Air, Valve Steam Frame, Varjo, Vive Pro Eye / Focus Vision, Quest Pro).
   - **DLSS 5 neural rendering** (experimental, eye tracked only): a more lifelike image. It uses about as much GPU time
     as eye tracking saves. The mod doesn't include NVIDIA's `nvngx_dlssnr.dll`; Setup looks for one that came with
     another game on your PC (e.g. Crimson Desert, No Man's Sky) and copies it into the game folder. It has to suit your
     graphics card (NVIDIA's current build is for the RTX 50 series).

Setup copies `dxgi.dll` (the mod) and, if chosen, a `CheekyFoveatedDLSS` folder (the mod loads it) and
`nvngx_dlssnr.dll` into the game folder, plus a `SOTTR-VR` folder with the readme and licences. Nothing in the game's own files is changed.

If another mod already uses `dxgi.dll` there (ReShade, OptiScaler, Special K), Setup renames it and puts it back when
you uninstall. The two can't run together yet.

**Updating or uninstalling:** run Setup again. It sees the mod is installed and asks whether to update / change it or
uninstall it. Or uninstall from Windows' Installed apps. You can keep your settings for next time, and uninstalling
also sets the game's fullscreen resolution back to your desktop's.

## First start

1. Start your VR runtime first (e.g. SteamVR), then the game as normal.
2. The first start writes `vrmod_settings.ini` next to the game and works out a render resolution for your headset and
   graphics card. The headset shows the settings it chose, and once a level has loaded it asks to **restart the game
   once**: press **A** (or Enter), confirm the game's quit message, then start the game again. From then on it renders
   at that resolution.
3. In the game's display settings, keep DirectX 12 and DLSS on. The mod turns off the game's own Stereoscopic 3D for
   you.
4. **SteamVR's own render resolution setting doesn't change this game,** whether it's on Auto or set by hand, globally or
   for this game: the mod takes it out and renders at its own Render resolution (overlay, Rendering page).
5. **World size.** The first time you play, the overlay opens at **World size** (View page). If the world looks too
   large or too small, change it with left / right. 100 % is life size, and you can change it there any time.

## Controls

| Gamepad | |
|---|---|
| Hold Guide, press Menu | Open or close the overlay |
| LB + click the right stick | First person / third person |
| LB + hold the right stick click | Aim where you look on / off |
| LB + click the left stick | Camera stabilisation on / off |

| Keyboard | |
|---|---|
| Ctrl+Alt+O | Open or close the overlay (if the Guide button doesn't reach the game) |
| Home | First person / third person |
| K | Aim where you look on / off |
| J | Mark this moment in the log, for bug reports |

In the overlay: D-pad up and down choose a line, left and right change it, **A** switches or runs it, **Y** puts it back
to the default, and **LB** / **RB** change page. The game keeps running and doesn't see the gamepad while it's open.

## Problems and bug reports

- In the overlay's **Support** page, **Save a bug report folder** copies the logs and your settings into a new folder in
  the game folder. Please attach its contents when you
  [report a problem](https://github.com/jcooray/Shadow-of-the-Tomb-Raider-VR/issues).
- `vrmod.log` next to the game says what the mod found and did. If something doesn't work, it's usually the first
  place to look.
- If the overlay won't open with Guide + Menu, Steam Input or the Xbox Game Bar is probably taking the Guide button.
  Use Ctrl+Alt+O instead.
- **Problems with foveated rendering:** please report them here, not to CheekyFoveatedDLSS. The build that comes with
  the mod includes my own changes. Turn on **Foveated rendering log** in the overlay's Support page, restart the game,
  reproduce the problem, then save a bug report folder: it then includes Cheeky's log too.

## Building from source

You need Visual Studio 2022 (C++ desktop workload, with MASM) and CMake 3.21 or later. MinHook and the OpenXR SDK are
fetched by CMake at pinned versions.

```bash
cmake -S . -B build-release -DVRMOD_DIAGNOSTICS=OFF
cmake --build build-release --config Release --target vrmod_dxgi_proxy
```

The mod is `build-release/bin/Release/dxgi.dll`. Leave out `-DVRMOD_DIAGNOSTICS=OFF` for the development build, which
adds the diagnostic probes and their hotkeys. `python tools/make_installer.py` builds the installer with
[Inno Setup](https://jrsoftware.org/isinfo.php). It needs the Cheeky build from
[jcooray/CheekyFoveatedDLSS, tag sottr-vr-1.0.0](https://github.com/jcooray/CheekyFoveatedDLSS/tree/sottr-vr-1.0.0).

How the parts work is in [docs](docs/): the camera and aiming, first person, DLSS, the HUD, frame warp, the overlay,
and how game addresses are found by pattern ([docs/RELEASE.md](docs/RELEASE.md)), including how to update them for a
different build of the game.

## Support development

The mod is free, and always will be. I make VR mods of older games as Hadean VR. If you enjoy this one and want to say
thanks, you can buy me a coffee on **[Ko-fi](https://ko-fi.com/hadeanvr)**. It goes into more hours of modding.

## Credits

- [Frans Bouma's Injectable Generic Camera System](https://github.com/FransBouma/InjectableGenericCameraSystem):
  where the camera hook and its struct offsets started.
- [CheekyFoveatedDLSS](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS) by ClarkCheekyKent: the optional
  foveated rendering.
- [MinHook](https://github.com/TsudaKageyu/minhook), the [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) and
  the [OpenVR](https://github.com/ValveSoftware/openvr) header.

Licences and notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Pimax headset owners: if eye tracking stops reaching SteamVR during a session, my
[Pimax Eye Tracking Monitor](https://github.com/jcooray/PimaxEyeTrackingMonitor) shows where it stops and restarts it.

## Licence

GPL-3.0. See [LICENSE](LICENSE).

Shadow of the Tomb Raider is a trademark of Square Enix. This mod is not affiliated with or endorsed by Square Enix,
Eidos-Montreal, Crystal Dynamics or NVIDIA. You need your own copy of the game.
