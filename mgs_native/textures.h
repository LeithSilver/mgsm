#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Texture pack: every CIwTexture in the game's *.group.bin files, extracted as PNG to textures\<group>\<hash>.png,
// and custom replacements loaded from the same folder (README.md §6.19).
//
// A texture is named by its IwGx resource hash (CIwTexture+4, the hashed texture name). Extract() also writes
// originals.txt with the size and CRC of every PNG it wrote; Init() treats a PNG that still matches as the
// original (the game uploads its own copy) and any other <8 hex digits>.png anywhere under the folder as a
// replacement. Replacements may be any size: texture coordinates are normalised.
namespace TexturePack {
// Extracts every texture under asset_root to out_dir. progress(done, total) is called as files are written.
// Returns the number of PNGs written, or -1 with *error set.
int Extract(const std::string& asset_root, const std::filesystem::path& out_dir,
            const std::function<void(int, int)>& progress, std::string* error);
// True when out_dir holds an extraction (its originals.txt).
bool Extracted(const std::filesystem::path& dir);
// Scans dir for replacements. Call once at startup (any thread).
void Init(const std::filesystem::path& dir);
int ReplacementCount();
void SetEnabled(bool on);
bool Enabled();
// The replacement for a texture hash as top-down RGBA bytes, or false (none, disabled, or unreadable).
bool Replacement(uint32_t hash, std::vector<uint32_t>& rgba, int& w, int& h);
// The texture's PNG as listed in originals.txt (e.g. "common\items\m9\0059788b.png", relative to the folder),
// or "" when it isn't one of the game's group textures.
std::string OriginalPath(uint32_t hash);
bool HasReplacement(uint32_t hash);
}
