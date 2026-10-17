#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include "cpu.h"

// Custom player model (README.md §6.24): a static mesh converted by tools/custommodel/convert.py into Snake's
// skeleton (customModel/snake.mgsm: per vertex a bone and a bone-local position). It follows the game's own
// animation: the end of the CPU skinning callback (0x4A045734) poses it with the bone matrices the game just used,
// and the game's draws with Snake's textures are replaced by it, with the same modelview. Emu thread.
namespace CustomModel {
// Reads the file (once; any thread). False with a reason in error() if it is missing or malformed.
bool Load(const std::filesystem::path& path);
bool Loaded();
// The model's folder (customModel next to the exe) exists: F1 shows the option only then (the model isn't shipped).
bool FolderPresent();
const std::string& Error();
int TriangleCount();

// Hook at 0x4A045734, the end of the skinning callback ([sp+0x84] = the skinned vertex count; Snake: 343).
void OnSkinned(Cpu* uc);
// GLES: a CIwTexture was uploaded into GL texture `name` (its path in textures/originals.txt, or "") / deleted.
// Snake's textures (common/characters/snake/snake) mark his draws: IwGx copies the skinned vertices into its own
// batches, so the vertex pointer doesn't identify them.
void NoteTexture(uint32_t name, const std::string& path);
void ForgetTexture(uint32_t name);
// GLES Draw: true when the model is on, Snake was skinned this frame and the draw uses one of his textures: the
// game's own draw is skipped and Draw() is called instead.
bool Covers();
// Draws the custom model (once per skinning; later calls do nothing), tinted with `color` (the game's lighting).
void Draw(const float color[4]);
}
