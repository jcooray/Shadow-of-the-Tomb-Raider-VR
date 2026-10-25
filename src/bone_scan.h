#pragma once

#include <cstdint>

namespace vrmod {

// First-person discovery (docs/FIRSTPERSON.md). Ctrl+Z, standing still in first person with the camera inside Lara's
// head: find her skeleton's bone matrices in memory.
//
// Ghidra (helper_decompiled.txt, FUN_140436380): a character instance's +0xE0 points to its skeleton state; the state's
// +0x70 points to an array of 4x4 float bone matrices (0x40 bytes each, translation row at +0x30 with w = 1), +0x78 to
// a second such array, and +0x9C holds the bone count (u16). This scan:
//   1. finds every 4x4 matrix in readable memory whose translation is within kNearCm of the camera (rows 0-2 of
//      plausible length with w = 0, translation w exactly 1.0);
//   2. grows each hit into its whole array (valid matrices at 0x40 steps, translations within 4 m);
//   3. searches memory for pointers to each array's first matrix -- a skeleton state has one at +0x70 or +0x78, and
//      its u16 at +0x9C should equal the array's length (confirms the layout);
//   4. reads every candidate array at each of the next kSamplePresents Presents: does it update every frame, which
//      bone is nearest the in-head point, which moves with head animation.
// Time-sliced (the game keeps running); writes vrmod_bonescan_<time>.txt. Call once per Present (Present thread).
void BoneScanOnPresent();

// Near-plane test (Ctrl+Alt+Z cycles off / 15 / 30 cm): the camera struct's +0x100 looks like the frustum's near plane,
// (0, 0, -1, d) with d = 5 (cm). While set, the camera hook writes d into +0x10C after every game write.
float BoneScanNearOverrideCm();   // 0 = off

// ---- Head-bone first-person camera ([firstperson] in vrmod_settings.ini) ----
// While first person is on, the Present thread locates Lara's skeleton automatically (the scan above, around the offset
// camera's position, published by the hook each write): the pair of equal-length pose buffers whose head bones agree
// and sit nearest. Lost (unreadable for 0.5 s, e.g. a level load) -> located again.
void HeadBonePublishHint(const float pos[3]);   // game thread: where the offset camera put the camera (cm)
// Game thread, camera hook: the eye position (world cm) for this game frame -- the head bone from the freshest pose
// buffer, its offset from the root smoothed, plus the eye offset. False while not located (use the offset camera).
bool HeadBoneEye(uint64_t gameFrame, const float cameraQuat[4], float outPos[3]);
bool HeadBoneActive();                          // enabled and located
void HeadBoneToggle();                          // Ctrl+Alt+Home
// Lara's standing height: the head bone above the skeleton root (cm = game units), the 80th percentile of samples taken
// while she stands still; 0 until 300 samples.
float HeadBoneStandingHeightCm(uint32_t* samples = nullptr);
bool HeadBoneStabiliseOn();
float HeadBoneEyeForwardCm();   // the eye offset in front of the head bone now (HeadBoneAdjustEye moves it)
void HeadBoneToggleStabilise();                 // gamepad LB + left stick click, Ctrl+Alt+S
void HeadBoneForget(const char* why);           // locate again (level loaded)
void HeadBoneAdjustEye(float dFwdCm, float dUpCm, float dSideCm);
// First-person near clip distance (cm), live value ([firstperson] near_cm; Ctrl+Alt+PageUp/PageDown).
float HeadCamNearCm();
void HeadCamAdjustNear(float dCm);

}
