#pragma once

#include <filesystem>
#include <string>

// Small portable helpers (paths are UTF-8 in std::string, std::filesystem::path for file access).
namespace Platform {
// Folder holding the executable (game\, textures\, screenshots\, mgs_mobile.ini live next to it). Android: the
// app's external files folder (Android/data/<package>/files/). Ends with a separator.
std::filesystem::path ExeDir();
// Android: stdout/stderr to logcat and mgs_mobile.log in ExeDir(), and environment variables from mgs_env.txt
// there (debug). Elsewhere nothing (see main.cpp).
void StartLog();
// UTF-8 string <-> path (std::filesystem::u8path; on Windows a plain std::string would be the ANSI code page).
inline std::filesystem::path U8Path(const std::string& s) { return std::filesystem::u8path(s); }
inline std::string U8String(const std::filesystem::path& p) { return p.u8string(); }
}
