#pragma once

#include <filesystem>
#include <functional>
#include <string>

struct SDL_Window;
struct SDL_Renderer;

// First-run setup (README.md §6.19): installs the game files from the player's own Metal.Gear.Solid.n-gage
// (the N-Gage 2.0 package) into game\ next to the exe, and extracts the textures to textures\.
namespace Installer {
// exe_dir ends with a backslash. True when game\ holds an installed copy.
bool Installed(const std::string& exe_dir);
// Installs from a package without any UI. progress(fraction 0..1, status) may be called from this thread.
bool Install(const std::filesystem::path& package, const std::string& exe_dir,
             const std::function<void(float, const std::string&)>& progress, std::string* error);
// The setup window (Windows only, setup_window_win32.cpp): drag and drop or browse for the package, or install
// the one found in ROM\ next to the exe.
// Returns true once installed, false if the window was closed first.
bool RunSetupWindow(const std::string& exe_dir);
// The Android setup screen (setup_android.cpp), in the game's window: pick the package with the file picker, or
// copy it into exe_dir. Returns true once installed, false if the app was closed first.
bool RunSetupScreen(const std::string& exe_dir, SDL_Window* window, SDL_Renderer* renderer);
}
