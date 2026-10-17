#include "heap.h"
#include <algorithm>

void GuestHeap::Init(uint32_t base, uint32_t size) {
    base_ = base; size_ = size; in_use_ = peak_ = 0;
    free_by_addr_.clear(); free_by_size_.clear(); used_.clear();
    InsertFree(base, size);
}

void GuestHeap::InsertFree(uint32_t addr, uint32_t size) {
    free_by_addr_[addr] = size;
    free_by_size_.emplace(size, addr);
}

void GuestHeap::EraseFree(std::map<uint32_t, uint32_t>::iterator it) {
    auto range = free_by_size_.equal_range(it->second);
    for (auto s = range.first; s != range.second; ++s)
        if (s->second == it->first) { free_by_size_.erase(s); break; }
    free_by_addr_.erase(it);
}

uint32_t GuestHeap::Alloc(uint32_t size) {
    const uint32_t need = RoundUp(size);
    auto best = free_by_size_.lower_bound(need);
    if (best == free_by_size_.end()) return 0;
    const uint32_t addr = best->second, avail = best->first;
    EraseFree(free_by_addr_.find(addr));
    if (avail > need) InsertFree(addr + need, avail - need);
    used_[addr] = need;
    in_use_ += need;
    peak_ = (std::max)(peak_, in_use_);
    return addr;
}

void GuestHeap::Free(uint32_t ptr) {
    auto u = used_.find(ptr);
    if (u == used_.end()) return;
    uint32_t addr = ptr, size = u->second;
    in_use_ -= size;
    used_.erase(u);

    auto next = free_by_addr_.lower_bound(addr);
    if (next != free_by_addr_.end() && next->first == addr + size) { size += next->second; EraseFree(next); }
    auto prev = free_by_addr_.lower_bound(addr);
    if (prev != free_by_addr_.begin()) {
        --prev;
        if (prev->first + prev->second == addr) { addr = prev->first; size += prev->second; EraseFree(prev); }
    }
    InsertFree(addr, size);
}

uint32_t GuestHeap::LargestFree() const {
    return free_by_size_.empty() ? 0 : free_by_size_.rbegin()->first;
}

uint32_t GuestHeap::SizeOf(uint32_t ptr) const {
    auto u = used_.find(ptr);
    return u == used_.end() ? 0 : u->second;
}

uint32_t GuestHeap::Realloc(uint32_t ptr, uint32_t size, uint32_t* copy_from, uint32_t* copy_len) {
    *copy_from = 0; *copy_len = 0;
    if (!ptr) return Alloc(size);
    if (size == 0) { Free(ptr); return 0; }
    auto u = used_.find(ptr);
    if (u == used_.end()) return Alloc(size);   // not ours (shouldn't happen): fresh block, nothing to copy

    const uint32_t old = u->second, need = RoundUp(size);
    if (need <= old) {                          // shrink in place, return the tail
        if (old - need >= kAlign) {
            u->second = need;
            in_use_ -= old - need;
            used_[ptr + need] = old - need;     // make the tail a used block, then free it (coalesces)
            in_use_ += old - need;
            Free(ptr + need);
        }
        return ptr;
    }
    auto next = free_by_addr_.find(ptr + old);  // grow in place into a free neighbour
    if (next != free_by_addr_.end() && old + next->second >= need) {
        const uint32_t total = old + next->second;
        EraseFree(next);
        if (total > need) InsertFree(ptr + need, total - need);
        u->second = need;
        in_use_ += need - old;
        peak_ = (std::max)(peak_, in_use_);
        return ptr;
    }
    const uint32_t fresh = Alloc(size);
    if (!fresh) return 0;
    *copy_from = ptr; *copy_len = old;          // caller copies old payload, then the old block is freed
    return fresh;
}
