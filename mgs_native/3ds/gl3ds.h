#pragma once

// OpenGL ES 1.1 on citro3d (gl3ds.cpp, README.md §6.28): the subset gles.cpp and the overlay use, as plain gl*
// functions, so gles.cpp compiles against <GLES/gl.h> unchanged. Draws are recorded during a frame and replayed
// into the top screen at EndFrame: once, or once per eye with the 3D slider up (stereoscopic 3D: draws with a
// perspective projection are shifted per eye, flat 2D draws such as the HUD stay at screen depth).
namespace GL3DS {
bool Init();                     // citro3d, the top-screen render targets, the shader (once; true when ready)
void SetStereo(bool on);         // before Init: false = one eye only (no right-eye target; Old 3DS / 2DS)
void* Proc(const char* name);    // gl* function by name (GLES::Host::get_proc)
void EndFrame();                 // GLES::Host::swap: hand the frame to the replay (render thread), start the next
void Shutdown();                 // before gfxExit: the render thread finishes and stops
// The bottom screen (the F1 overlay): SetBottomScreen(true) shows our render target there instead of the console
// (game thread; the caller re-inits the console after false). GL calls between BeginBottom and EndBottom draw on
// it (a 320x240 screen) instead of the top screen.
void SetBottomScreen(bool on);
void BeginBottom();
void EndBottom();
// Stereo strength from the 3D slider is scaled by this (Settings: 0..200, 100 = default).
void SetStereoStrength(int percent);
// Milliseconds since the last call: recording draws, waiting for the GPU (in EndFrame), all of EndFrame.
void TakeStats(double& record_ms, double& wait_ms, double& end_ms);
}
