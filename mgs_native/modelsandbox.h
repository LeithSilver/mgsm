#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Loads a resource group the running game hasn't loaded, through the game's own code, without touching the game
// (README.md §6.23): the guest's memory is copied into a separate Unicorn, the copy loads the group
// (CIwResManager), renders each of its CIwModels (IwGx) and flushes; its GL calls are recorded on the CPU instead
// of being drawn. Emu thread only, while the game is paused (the F1 overlay is open).
namespace ModelSandbox {

struct Texture {
    uint32_t hash = 0;            // CIwTexture hash (0: not uploaded through CIwTexture)
    int w = 0, h = 0;
    std::vector<uint32_t> rgba;   // top row first, R in the low byte
};

struct Draw {
    uint32_t mode = 0;               // GL primitive
    std::vector<float> pos;          // xyz per vertex, as given to GL
    std::vector<float> uv;           // st per vertex, texture matrix applied (empty: none)
    std::vector<float> col;          // rgba per vertex (empty: `color`)
    std::vector<uint16_t> indices;   // rebased to vertex 0 (empty: vertices in order)
    uint32_t count = 0;              // vertices / indices drawn
    float mv[16] = {}, proj[16] = {};
    int texture = -1;                // Result::textures index, -1 untextured
    bool blend = false, alpha_test = false;
    float color[4] = {1, 1, 1, 1};
};

struct Model {
    uint32_t hash = 0;               // CIwModel resource hash
    uint32_t skeleton = 0;           // its skin's skeleton in the copy (0: not skinned)
    std::vector<Draw> draws;
};

struct Anim {
    uint32_t hash = 0;               // CIwAnim resource hash
    std::string name;                // from the scripts or the code, when a string hashes to it ("" otherwise)
    uint32_t skeleton = 0;           // the skeleton it animates (matches Model::skeleton)
    uint32_t length = 0;             // in the game's animation time (4096 = 1 s)
    int keyframes = 0;
};

struct Result {
    std::vector<Model> models;
    std::vector<Texture> textures;
    std::vector<Anim> anims;         // the animations of its skinned models
    std::vector<std::string> files;  // files the game read while loading
    std::string error;
};

// group: the path the game uses, e.g. "common/items/debugcube.group" (".bin" is added by the game).
// The copy stays alive after a load (until the next load or Release) for Pose.
bool Load(const std::string& group, Result& out);
// Renders Result::models[model] again, posed by Result::anims[anim] at `time` (4096 = 1 s) with the game's own
// animation player; anim -1 = the bind pose. Texture indices as in the Result.
bool Pose(size_t model, int anim, uint32_t time, Model& out, std::string* error);
void Release();

}  // namespace ModelSandbox
