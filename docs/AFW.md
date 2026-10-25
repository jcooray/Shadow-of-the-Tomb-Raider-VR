# AFW -- both eyes every frame under AER

**Problem (user, 2026-10-04):** AER motion clarity is insufficient. Each eye gets a new image every other frame (36 Hz
at 72 Hz). The eye not rendered keeps its last image and pose; the compositor reprojects it for head *rotation* only,
not for the game camera moving (stick turns, walking, Lara's motion), so that eye shows the world a frame late:
36 Hz judder on everything that moves relative to the head.

**v1 (this build): stereo warp from the eye just rendered.** Both AER views share the orientation and the (symmetric
cover) frustum and differ by the eye separation along the camera's x axis, so the other eye at the same moment is the
rendered image with every pixel moved sideways by its disparity, `x' = x - f_px * baseline / z`:
- **Depth:** at the DLSS evaluate of the frame (per-eye routing, so the frame is known exactly -- D2c), the game's
  reversed depth is linearised with that frame's near/far (`z = n f / (d (f - n) + n)`, as D2c) into one of four
  render-resolution slots keyed by frame (`AfwOnEvaluate`).
- **Warp (Present, our copy list):** copy the backbuffer; clear a per-pixel nearest-depth buffer; splat every source
  pixel's depth to its target (InterlockedMin); resolve colours where the depth won; fill disocclusions from the
  farther filled neighbour along the row (background). Written into the other eye's swapchain image, which is declared
  with that eye's pose at the same moment (`FrameRec::poseOther`) and the same FOV.
- Only for ordinary stereo AER frames with depth captured for their frame (`AfwReady`); otherwise plain AER.
- `[render] afw` (default 0), **Ctrl+Alt+I** toggles; `AFW (10 s)` log line (eyes warped, frames without depth).

**Expected artifacts:** fringes at depth edges, largest for near objects (the bow and hands: ~270 px at 50 cm),
filled with background; transparency/particles warp with whatever depth is behind them; the HUD is warped with the
scene depth under it (the crosshair sits at the target's depth in the warped eye, at infinity in the rendered one --
may shimmer). Next steps depend on what's visible: fill from the other eye's last real image, HUD separation, a
temporal warp.
