#pragma once

namespace vrmod {

// World-scale tuning. The game's stereo settings live as three consecutive floats in its settings object:
// StereoSeparation, StereoPopOut, StereoNearSeparationMultiplier (0.022, 0.271, 0.35 with the defaults in use).
// The first key press scans memory for that triple; afterwards
//   Insert            separation +5%   (bigger separation = the world looks smaller, people less giant)
//   Ctrl+Insert       separation -5%
//   Alt+Insert        near-field separation multiplier +5%
//   Ctrl+Alt+Insert   near-field separation multiplier -5%
// Values are logged as "StereoScale:". Call once per frame from the render thread.
void StereoScalePollHotkeys();

}
