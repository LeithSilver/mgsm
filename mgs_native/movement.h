#pragma once

class Cpu;

// Player movement enhancements (README.md §6.22): 8-way movement with the d-pad / keys, and analog movement
// (any angle) with a stick or the touch D-pad. Both work through the game's own movement direction (the
// input object at [0x4A0F1BB8]+0xC): the keys still decide whether Snake moves at all and everything else
// they do; only the direction the game builds from them changes.
namespace Movement {

// Emu thread: the code hooks (S3ERuntime::Init).
void InstallHooks(Cpu* uc);

// Any thread. A stick in screen space (x right, y down, -1..1); 0, 0 = centred / let go. Sources: the
// gamepad's left stick and the touch D-pad; the one moved last wins.
enum Source { PAD, TOUCH, SOURCE_COUNT };
void SetStick(Source s, float x, float y);

// The direction keys for a stick (gamepad.cpp, left stick as d-pad): radial dead zone with hysteresis
// (latched across calls through *held), then 8 sectors with diagonal movement on (two keys on the
// diagonals) or 4 without. Writes up / down / left / right.
void StickKeys(float x, float y, bool* held, bool& up, bool& down, bool& left, bool& right);

} // namespace Movement
