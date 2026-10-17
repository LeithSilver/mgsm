#pragma once

#include <cstdint>
#include <string>
#include <vector>

// F1 > Textures and F1 > Models (debug mode). README.md §6.23.
//
// Textures: every texture the game has uploaded and not deleted, read straight from its GL objects, named by the
// CIwTexture hash (and its extracted PNG path, textures\originals.txt) when it came through the CIwTexture upload.
// Models: the 3D draws of one game frame, captured in gles.cpp's Draw (the overlay lets one frame through for it),
// moved into world space and grouped into models by texture folder, then shown in an orbit view and exportable as OBJ.
// Everything here runs on the emu thread (GL calls and the overlay both do).
namespace AssetViewer {

// ---- gles.cpp ----
// A texture's level 0 was (re)defined. hash = the CIwTexture hash, 0 for other uploads (fonts, surfaces).
void OnTexImage(uint32_t name, int w, int h, const char* format, uint32_t hash, bool replaced);
void OnDeleteTexture(uint32_t name);
// A capture is recording the current frame: gles.cpp passes each draw to AddDraw.
bool Recording();
struct DrawIn {
    uint32_t mode = 0;                    // GL primitive
    const std::vector<uint16_t>* indices; // rebased to vertex 0; empty: vertices 0..count-1 in order
    uint32_t count = 0;                   // vertices / indices drawn
    const float* pos = nullptr; int vsize = 3; uint32_t nverts = 0;
    const float* uv = nullptr; int uvsize = 2;   // texture unit 0, or null
    const float* col = nullptr;                  // RGBA floats per vertex, or null
    bool eye_space = false;                      // positions are already in eye space (host-skinned)
    uint32_t vptr = 0;                           // guest vertex array address
};
void AddDraw(const DrawIn& d);
// Present: called first thing (a recorded frame is complete), and when Present goes back to the game
// (a requested capture starts recording the next frame).
void OnPresentBegin();
void OnPresentEnd();
// The pause hook lets the game run while a capture waits for its frame.
bool CaptureWaiting();

// ---- overlay.cpp (inside the ImGui frame) ----
void DrawTexturesTab();
void DrawModelsTab();
// Start a capture (as the button does); `group` is selected once it is done (0 = the whole frame).
void RequestCapture(int group = 0);
// Load a group file in the Models tab next time it is drawn (as picking it in the list does).
void RequestLoad(const std::string& group);
// After the window layout, before ImGui renders: draws the Models preview into its texture.
void RenderPreview();

}  // namespace AssetViewer
