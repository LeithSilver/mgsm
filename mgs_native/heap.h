#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>

// Allocator for the guest heap (s3eMalloc/s3eRealloc/s3eFree). Bookkeeping lives on the host, so guest
// memory holds only payload. Best-fit over free blocks, neighbours coalesce on free, realloc grows or
// shrinks in place when it can. The game frees and reallocates whole levels on every room change, so
// memory must really be reused (a bump allocator ran out after two rooms).
class GuestHeap {
public:
    void Init(uint32_t base, uint32_t size);

    uint32_t Alloc(uint32_t size);                 // 0 if out of memory
    void Free(uint32_t ptr);                       // ignores 0 and unknown pointers
    // Returns the (possibly new) address; *moved_from/size tell the caller what to copy (0 = nothing).
    uint32_t Realloc(uint32_t ptr, uint32_t size, uint32_t* copy_from, uint32_t* copy_len);
    uint32_t SizeOf(uint32_t ptr) const;           // usable size, 0 if not allocated

    uint32_t BytesInUse() const { return in_use_; }
    uint32_t PeakInUse() const { return peak_; }
    uint32_t FreeBytes() const { return size_ - in_use_; }
    uint32_t LargestFree() const;

private:
    static constexpr uint32_t kAlign = 16;
    static uint32_t RoundUp(uint32_t n) { return n ? (n + kAlign - 1) & ~(kAlign - 1) : kAlign; }

    void InsertFree(uint32_t addr, uint32_t size);
    void EraseFree(std::map<uint32_t, uint32_t>::iterator it);

    uint32_t base_ = 0, size_ = 0, in_use_ = 0, peak_ = 0;
    std::map<uint32_t, uint32_t> free_by_addr_;          // addr -> size
    std::multimap<uint32_t, uint32_t> free_by_size_;     // size -> addr
    std::unordered_map<uint32_t, uint32_t> used_;        // addr -> block size
};
