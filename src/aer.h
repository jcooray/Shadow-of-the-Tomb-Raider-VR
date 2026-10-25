#pragma once

#include <cstdint>

namespace vrmod {

// Phase 5 -- alternate-eye rendering: per-eye camera poses (see docs/PHASE5.md, docs/COORDS.md).
//
// Each game frame renders one eye (alternating by game frame number). Its camera is the game's own camera (the
// base: follow/orbit camera, mouse/stick yaw, plus the first-person offset) with that eye's OpenXR view composed on
// top, relative to a recentre reference:
//   rotation = base * map(q_ref^-1 * q_eye)
//   position = base position + base rotation * map(q_ref^-1 * (p_eye - p_ref)) * 100 cm/m * world scale
// map() takes OpenXR LOCAL axes (x right, y up, z back) to the game camera's local axes (X right, Y forward, Z up):
// (x, y, z) -> (x, -z, y), the same for a quaternion's vector part. q_ref is yaw-only (recentring keeps gravity);
// p_ref is the midpoint between the eyes at recentre, so the eyes sit at +-IPD/2 plus head movement.
// Each eye's OWN view pose is used (orientation and position), never head orientation + IPD.

// A pose in OpenXR LOCAL space: orientation x y z w, position in metres.
struct PoseF {
    float q[4];
    float p[3];
};

// Present thread, once per frame with valid views (index 0 = left, 1 = right), located for the display time
// targetTimeNs (XrTime) at which a frame rendered with them is expected to be shown (Phase 7: pose prediction).
void AerPublishViews(const PoseF views[2], int64_t targetTimeNs);

// Present thread, once per frame: F8 recentre (shared with head tracking), Ctrl+[ / Ctrl+] world scale -/+ 5 %,
// Ctrl+H hold the game's base camera still (stereo check), Ctrl+X 20-second axis log (docs/COORDS.md).
void AerPollHotkeys();

// Recentre at the next published views, as F8 does (any thread). Called automatically when an in-engine cutscene
// starts or ends: the game cuts to a new camera, so "forward" is re-taken from where the head points now.
void AerRequestRecenter(const char* why);

// Game thread, camera hook, every write. Returns false until views have been published. outDelta is the rotation to
// compose onto the base quaternion (game camera-local), outOffsetWorld the position offset in game world units,
// outFov the FOV value to write into the struct (Phase 6: covers the headset's view, see AerSetGameFov). Both writes
// of a game frame get the same eye, pose and FOV.
bool AerComputeCamera(uint64_t gameFrame, const float baseQuat[4], float outDelta[4], float outOffsetWorld[3], float* outFov);

// A3 (docs/AIM.md): the recentred head's yaw and pitch (midpoint of the latest eye views; roll dropped) as a
// camera-local rotation for game * delta: yaw about +Z (up) applied after pitch about +X (right). False before the
// first views.
bool AerHeadAimDelta(float outDelta[4]);

// The eye (0 left, 1 right) chosen for a game frame, or -1 if that frame isn't in the ring (DLSS probe).
int AerEyeOfFrame(uint64_t gameFrame);
uint64_t AerLastPairedFrame();
// The game's final projection recorded for a game frame (vertical FOV in radians, aspect); false if not recorded.
bool AerFinalProjectionOf(uint64_t gameFrame, float* fovV, float* aspect);   // the game frame the latest Present was paired with (0 = none yet)

// Present thread: the FOV value to write into the game camera from now on (Phase 6: derived from the runtime's views).
void AerSetGameFov(float fov);

// Projection probe (Phase 6): while frozen, every frame renders the LEFT eye from the views captured when the freeze
// started (head motion ignored), with an extra camera-local yaw (about +Z, left positive) and pitch (about +X, up
// positive) in radians.
void AerProbeSet(bool freeze, float yaw, float pitch);

// Hold the base camera (Ctrl+H): the hook keeps rewriting the game's base position/rotation captured when it was
// switched on, so only head motion moves the view.
bool AerHoldBase();
void AerSetHoldBase(bool on);

// Present thread: which eye and pose the image being presented was rendered with, by the Phase 4 pairing rule
// S = clamp(previous S + Presents elapsed, G - 2, G - 1) (G = latest write's game frame, `present` = Present counter).
// False when the frame is unknown (no AER write for it, e.g. menus) -- the caller then shows the image to both eyes.
struct AerPresented {
    int eye;
    PoseF pose;
    PoseF poseOther;        // the other eye's pose at the same moment (AFW)
    uint64_t gameFrame;
    float fov;              // the FOV value written into the game camera for that frame
    int64_t targetTimeNs;   // the display time its pose was predicted for
    bool corrected;         // the pairing rule's clamp had to move S this Present (a hitch): treat as uncertain
    bool mono;              // rendered from the head centre (AerSetMonoRender): show it to both eyes
    bool haveFinal;         // the game's own final projection for that frame is known (AerRecordFinalProjection)
    float finalFov;         // its vertical FOV (radians) -- after the game's FOV correction and 16:9 pinning
    float finalAspect;      // its aspect (width / height)
};
bool AerPairPresent(uint64_t latestGameFrame, uint64_t present, AerPresented* out);
// D2c (dlss_mv.cpp) found the evaluated frame `frames` away from the prediction: the Presents were paired with the wrong
// game frame -- the other eye's image, shown with this eye's pose (2026-10-01: near surfaces at the wrong depth after a
// cutscene). The next AerPairPresent moves S by `frames` and the clamp window with it. AerPairExtraLag: frames the
// window sits beyond the Phase 4 rule (G - 2 .. G - 1), for the log.
void AerRequestPairShift(int frames);
int AerPairExtraLag();

float AerWorldScale();
void AerSetWorldScale(float scale);   // in-headset overlay

// Phase 8 mono fallback while writes are honoured (cutscenes with aer_cutscenes=mono, Ctrl+V, the frames after a
// fallback ends): new game frames render from the head centre -- the midpoint of the two eye views, their mean
// orientation -- instead of alternating eyes; AerPresented::mono marks them.
void AerSetMonoRender(bool on);

// Game thread, camera hook, first write of each game frame: the projection the camera manager actually used for the
// PREVIOUS game frame -- its vertical FOV and aspect, stored 0x268 / 0x264 bytes before the struct (FUN_1409d6f30
// [-0x22] / [-0x21]: our FOV + the game's FOV correction, pinned-horizontal below 16:9). The declared frustum uses it,
// so game-side FOV corrections (after the examine scene of run 8 one blended back over ~10 s) no longer warp the view.
void AerRecordFinalProjection(uint64_t gameFrame, float fovV, float aspect);

} // namespace vrmod
