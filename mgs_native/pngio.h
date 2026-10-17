#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// PNG files (screenshots, extracted and custom textures) through stb_image / stb_image_write. Any thread.
namespace PngIO {
// Writes top-down 32-bit pixels in BGRA byte order (0xAARRGGBB words). Returns "" or an error.
std::string Write(const std::filesystem::path& path, const uint32_t* bgra, int w, int h);
// Reads any PNG as top-down RGBA bytes (0xAABBGGRR words, GL_RGBA / GL_UNSIGNED_BYTE order).
bool ReadRGBA(const std::filesystem::path& path, std::vector<uint32_t>& rgba, int& w, int& h);
// The same from a PNG in memory.
bool DecodeRGBA(const uint8_t* data, size_t size, std::vector<uint32_t>& rgba, int& w, int& h);
}
