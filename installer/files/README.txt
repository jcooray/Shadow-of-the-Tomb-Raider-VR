Shadow of the Tomb Raider VR
===========================

A VR mod for Shadow of the Tomb Raider: full stereo with your head tracked, a first-person view from Lara's eyes (and
the game's third-person camera when you want it), aiming where you look, and the HUD placed where you can see it.
Free and open source (GPL-3.0). If you enjoy it, you can support development -- see the end of this file.

What you need
-------------
- Shadow of the Tomb Raider on PC (made and tested with the Steam version, DirectX 12).
- A PC VR headset with an OpenXR runtime (made and tested with SteamVR; others should work).
- An NVIDIA RTX graphics card for DLSS. Other GPUs should run without DLSS but haven't been tested yet.
- A gamepad. Most of the mod is driven from the gamepad and an overlay in the headset.

First start
-----------
1. Start your headset's VR software (e.g. SteamVR) first, then the game from Steam as normal.
2. The first start writes vrmod_settings.ini next to the game and works out the best resolution for your headset.
   The headset shows the settings it chose, and once a level has loaded it asks to restart the game once: press A (or
   Enter), confirm the game's quit message, then start the game again.
3. In the game's display settings: keep DirectX 12 and DLSS on. The mod switches the game's own Stereoscopic 3D off
   for you.
4. SteamVR's own render resolution setting doesn't change this game (Auto or manual, global or per game): the mod
   renders at its own Render resolution (overlay, Rendering page).
5. World size: the first time you play, the overlay opens at World size (View page). If the world looks too large
   or too small, change it with left / right. 100 % is life size, and you can change it there any time.

Controls
--------
- Hold Guide (the Xbox button) and press Menu: the mod's overlay -- status, view, rendering and HUD settings.
  Every change is previewed, and you're asked to keep or undo it when you leave a page.
- LB + click the right stick: first person / third person.
- LB + hold the right stick click: aim where you look on / off.
- LB + click the left stick: camera stabilisation on / off.
- Keyboard: Home first person, K aim where you look, J mark a moment in the log, Ctrl+Alt+O the overlay.

Foveated rendering (optional)
-----------------------------
Chosen in Setup: none, fixed or eye tracked. Foveated rendering keeps part of your view at full quality and the rest
slightly softer; only the DLSS pass gets cheaper. Fixed keeps the middle sharp and saves very little GPU time (about
2 %). Eye tracked keeps sharp wherever you look and saves about 15 %, so the automatic render resolution can be about
30 % higher. Change it in the overlay: Rendering > Foveated rendering (after a restart). CheekyFoveatedDLSS's own menu
is on F3.

DLSS 5 neural rendering (experimental, eye tracked only): a more lifelike image, using about as much GPU time as eye
tracking saves. NVIDIA's nvngx_dlssnr.dll is not included with the mod: Setup copies one that came with another game on
your PC into the game folder (and removes that copy again when you uninstall). Switch it in the overlay: Rendering >
DLSS 5.

Problems?
---------
- In the overlay's Support page, "Save a bug report folder" copies the logs and your settings into a new folder next
  to the game. Attach that folder's contents when you report a problem.
- vrmod.log next to the game says what the mod found and did.
- To update or uninstall, run Setup again and choose, or uninstall from Windows' "Installed apps". Your settings can
  be kept for next time.

Source, updates and support
---------------------------
Source and releases: https://github.com/jcooray/Shadow-of-the-Tomb-Raider-VR
Support development (voluntary): https://ko-fi.com/hadeanvr -- thank you!

Shadow of the Tomb Raider is a trademark of Square Enix. This mod is not affiliated with or endorsed by Square Enix,
Eidos-Montreal, Crystal Dynamics or NVIDIA.
