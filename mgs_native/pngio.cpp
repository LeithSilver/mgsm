#include "pngio.h"

#include <cstring>
#include <fstream>
#include <iterator>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

// Files go through std::filesystem::path streams (wide paths on Windows), so stb's own stdio is off.
std::string PngIO::Write(const std::filesystem::path& path, const uint32_t* px, int w, int h) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        const uint32_t p = px[i];
        rgba[4 * i] = static_cast<uint8_t>(p >> 16);
        rgba[4 * i + 1] = static_cast<uint8_t>(p >> 8);
        rgba[4 * i + 2] = static_cast<uint8_t>(p);
        rgba[4 * i + 3] = static_cast<uint8_t>(p >> 24);
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return "cannot create the file";
    const int ok = stbi_write_png_to_func([](void* ctx, void* data, int size) {
        static_cast<std::ofstream*>(ctx)->write(static_cast<const char*>(data), size);
    }, &f, w, h, 4, rgba.data(), w * 4);
    f.close();
    if (!ok) return "PNG encoder error";
    if (!f) return "write error";
    return "";
}

bool PngIO::ReadRGBA(const std::filesystem::path& path, std::vector<uint32_t>& rgba, int& w, int& h) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return DecodeRGBA(reinterpret_cast<const uint8_t*>(data.data()), data.size(), rgba, w, h);
}

bool PngIO::DecodeRGBA(const uint8_t* data, size_t size, std::vector<uint32_t>& rgba, int& w, int& h) {
    int comp = 0;
    stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
    if (!px) return false;
    const bool ok = w > 0 && h > 0 && w <= 8192 && h <= 8192;
    if (ok) {
        rgba.assign(static_cast<size_t>(w) * h, 0);
        memcpy(rgba.data(), px, rgba.size() * 4);
    }
    stbi_image_free(px);
    return ok;
}
