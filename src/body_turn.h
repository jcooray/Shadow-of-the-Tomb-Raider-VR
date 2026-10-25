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

}  // namespace vrmod
