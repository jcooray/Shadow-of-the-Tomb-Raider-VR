# Developer notes

How the mod works, written while building it, for anyone who wants to change or extend it. They're working notes:
dated, and sometimes describing an approach that was later replaced (the code is the final word).

| File | About |
|---|---|
| [AIM.md](AIM.md) | The game camera, aiming and the camera hooks |
| [FIRSTPERSON.md](FIRSTPERSON.md) | First person: the head camera, hiding Lara's head, body turn, near plane |
| [COORDS.md](COORDS.md) | Coordinate systems: game, OpenXR and the head pose |
| [DLSS.md](DLSS.md) | DLSS per eye, motion vectors and CheekyFoveatedDLSS |
| [AFW.md](AFW.md) | Frame warp: building the other eye's image every frame |
| [HUD.md](HUD.md) | The HUD in its own layer |
| [OVERLAY.md](OVERLAY.md) | The in-headset overlay |
| [RELEASE.md](RELEASE.md) | Release builds, finding game addresses by pattern, and updating the patterns for another build of the game |

Game function names like `FUN_1409d6f30` are Ghidra's names for addresses in the Steam build of SOTTR.exe. Files named
`*_decompiled.txt` that the notes mention were local Ghidra output and aren't included: the repository contains none of
the game's code. To follow along, load your own SOTTR.exe into [Ghidra](https://ghidra-sre.org/).
