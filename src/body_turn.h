#pragma once

#include <string>

namespace vrmod {

// First-person body turn (2026-10-11): Lara's body follows the view the stick turns, so looking round never looks back
// through her own body. Standing still, once the view (stick part, not the real head) is more than [firstperson]
// body_turn_deg from where she faces, her heading is turned towards it at up to body_turn_speed deg/s until within
// 3 deg. Not while she moves (the game turns her then), aims, or outside head drive. Her heading is her instance's
// (the world-object pool entry the Player copies its rotation from each frame; found with the facing probe).
// Game thread, from head drive, once per game frame: viewYaw = the view's body yaw (world, as the camera's).
void BodyTurnGameThread(float viewYaw, bool walkingCamera);
// True while her body is being turned (the head camera holds the eye still meanwhile).
bool BodyTurnTurning();
// One line for the 30 s log.
std::string BodyTurnStats();
// Hand aim (motion controls stage 2b): where the game's aim ray hits (or ends) -- her Player object's targeting point
// (+0x1B10), world units. Her instance is found as for body turn (the patterned world-object pool and player index),
// whether body turn is on or not. Game thread. False until she is known or if it doesn't read.
bool LaraAimHitPoint(float p[3]);

}  // namespace vrmod
