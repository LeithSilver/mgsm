#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Screenshots (F12 / photo mode): PNG files in screenshots/ next to the exe, written on a worker thread.
namespace Screenshot {
// Saves top-down BGRA pixels (alpha ignored) as a new screenshots\mgs_<date>_<time>.png; done(path or "", error)
// is called on the worker thread.
void SaveAsync(std::vector<uint32_t> bgra, int w, int h, void (*done)(const std::string& path, const std::string& error));
}
