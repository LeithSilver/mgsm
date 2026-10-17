#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <memory>
#include <list>
#include <functional>
#include <cstdio>
#include "cpu.h"

// Virtual file system backing the s3eFile* API.
// Two kinds of handle share one namespace:
//  - disk files  (s3eFileOpen)           -> std::fstream on the host, or a stream over the RAM copy (PreloadAll)
//  - memory files (s3eFileOpenFromMemory) -> a window [addr, addr+size) of guest memory
class VirtualFileSystem {
public:
    static VirtualFileSystem& Instance();

    void SetAssetRoot(const std::string& root);
    void SetEngine(Cpu* uc) { uc_ = uc; }
    std::string ResolvePath(const std::string& rel_path);
    const std::string& AssetRoot() const { return asset_root_; }

    bool CheckExists(const std::string& path);
    uint32_t Open(const std::string& path, const std::string& mode);
    uint32_t OpenFromMemory(uint32_t guest_addr, uint32_t size);
    void Close(uint32_t handle);
    uint32_t Read(uint32_t handle, void* buffer, uint32_t elem_size, uint32_t count);
    uint32_t Write(uint32_t handle, const void* buffer, uint32_t elem_size, uint32_t count);
    int Seek(uint32_t handle, int32_t offset, int origin);
    int32_t Tell(uint32_t handle);
    uint32_t GetSize(uint32_t handle);
    bool EOF_Reached(uint32_t handle);
    int32_t GetChar(uint32_t handle);
    int32_t PutChar(uint32_t handle, uint8_t c);
    void Flush(uint32_t handle);
    bool Delete(const std::string& path);
    bool MakeDirectory(const std::string& path);
    // Reads every file under the asset root into memory (3DS: README.md section 6.28; the SD card made each room
    // change slow). Read-only opens and existence checks are then served from RAM and the index, without touching
    // the card; files the game writes or deletes drop out of the cache. Returns the bytes loaded.
    // load_data = false (Old 3DS / 2DS: no memory for it): the index only; each read-only open then reads the
    // whole file in one go and serves it from memory until it is closed (stdio's 1 KB reads were slow there).
    // pack_path (3DS): the game's data files in one file (data.pak beside the data folder), so loading takes one
    // seek and read per file instead of an SD-card open (15-40 ms each on an Old 3DS). Built from the folder when
    // missing or out of date (on_build is called first: it takes a while); only files in subfolders go in (the
    // game writes its saves and settings at the top), and one written or deleted anyway retires the pack.
    size_t PreloadAll(bool load_data = true, const std::string& pack_path = "", const std::function<void()>& on_build = nullptr);
    // With the index only: the files read most recently stay in memory up to this many bytes (the game opens
    // each group file twice in a row, and an SD-card open costs 100-200 ms on an Old 3DS).
    void SetRecentCacheBytes(size_t bytes) { recent_budget_ = bytes; }

private:
    VirtualFileSystem() = default;

    std::string NormalisePath(const std::string& rel_path) const;
    std::string WritablePath(const std::string& rel_path);
    // Case-insensitive lookup under the asset root (case-sensitive file systems: Linux, Android). The game asks for
    // some files with other capitals than they were packed with ("SingleBone" vs singlebone.group.bin).
    std::string FindNoCase(const std::string& rel_path);

    struct MemFile {
        uint32_t addr;
        uint32_t size;
        uint32_t pos;
        bool eof;
    };

    Cpu* uc_ = nullptr;
    std::string asset_root_;
    uint32_t next_handle_ = 0x60000000;
    std::unordered_map<uint32_t, std::unique_ptr<std::iostream>> open_files_;
    // PreloadAll: real path -> contents, and every file's path known (lookups need no disk access)
    std::unordered_map<std::string, std::shared_ptr<const std::string>> ram_;
    bool preloaded_ = false;
    // SetRecentCacheBytes: real path -> contents, most recently read first
    std::list<std::pair<std::string, std::shared_ptr<const std::string>>> recent_;
    size_t recent_bytes_ = 0, recent_budget_ = 0;
    std::shared_ptr<const std::string> TakeRecent(const std::string& real);   // (moves it to the front)
    void AddRecent(const std::string& real, std::shared_ptr<const std::string> data);
    void DropRecent(const std::string& real);
    // The pack (PreloadAll's pack_path): real path -> {offset, size} in it, read through one open FILE.
    struct PackEntry { uint32_t offset, size; };
    std::unordered_map<std::string, PackEntry> pack_entries_;
    FILE* pack_ = nullptr;
    std::string pack_path_;
    bool OpenPack(const std::vector<std::pair<std::string, std::string>>& files);   // (real, rel): all must be in it
    bool BuildPack(const std::vector<std::pair<std::string, std::string>>& files);
    void RetirePack(const std::string& real);   // a packed file written or deleted: the pack is rebuilt next start
    std::shared_ptr<std::string> ReadWhole(const std::string& real);   // from the pack, else the file (null: neither)
    std::unordered_map<uint32_t, MemFile> mem_files_;
    // stdio-style EOF flag for disk handles. The fstream itself is kept in a good state so
    // tell/seek keep working after a short read, as with fread/ftell.
    std::unordered_set<uint32_t> disk_eof_;
    // lower-case relative path -> real path, built on the first miss of an exact lookup
    std::unordered_map<std::string, std::string> nocase_;
    bool nocase_built_ = false;
};
