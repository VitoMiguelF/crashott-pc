#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

// Page-granular allocator for a fixed guest range, with Windows-style reserve/commit semantics.
// Used for the title's virtual heap (NtAllocateVirtualMemory, 0x40000000+) and physical heap
// (MmAllocatePhysicalMemoryEx, 0xA0000000+). State is tracked in 4 KB pages; callers pick the
// rounding (4 KB or 64 KB pages) and the reservation alignment. Freshly committed pages read as
// zero: host memory is committed and decommitted with the guest pages, except in the physical
// views (a mapped section, always committed), where pages used before are cleared on commit.
class PageHeap
{
public:
    static constexpr uint32_t PAGE_SIZE = 0x1000;

    enum class State : uint8_t { Free, Reserved, Committed };

    struct RegionInfo
    {
        uint32_t base;
        uint32_t allocationBase;
        uint32_t allocationProtect;
        uint32_t size;
        State state;
        uint32_t protect;
    };

    PageHeap(const char* name, uint32_t begin, uint32_t end);

    bool Contains(uint32_t guest) const { return guest >= m_begin && guest < m_end; }
    uint32_t Begin() const { return m_begin; }
    uint32_t End() const { return m_end; }

    // Reserves `size` bytes (a multiple of PAGE_SIZE). With `fixedBase` != 0 the range must be
    // free; otherwise the first free range aligned to `alignment` inside [low, high) is used,
    // searching downward when `topDown` is set. Returns the base, or 0.
    uint32_t Reserve(uint32_t fixedBase, uint32_t size, uint32_t alignment, uint32_t protect, bool topDown,
                     uint32_t low = 0, uint32_t high = 0);

    // Commits [guest, guest + size), which must lie inside one reservation.
    bool Commit(uint32_t guest, uint32_t size, uint32_t protect);

    // Returns committed pages in [guest, guest + size) to the reserved state.
    bool Decommit(uint32_t guest, uint32_t size);

    // Frees the whole reservation starting at `allocationBase`. Returns its size, or 0 if
    // `allocationBase` is not the start of a reservation.
    uint32_t Release(uint32_t allocationBase);

    // Run of pages starting at `guest` (rounded down) with the same state and protection.
    RegionInfo Query(uint32_t guest);

    // Current protection of the page holding `guest`, or 0 if it isn't committed.
    uint32_t QueryProtect(uint32_t guest);

    uint32_t ReservedPageCount();
    uint32_t TotalPageCount() const { return uint32_t(m_pages.size()); }

private:
    struct Page
    {
        uint32_t allocationBase;  // 0 when free
        uint32_t protect;
        State state;
        bool dirty;  // physical views only: held data since the section was created
    };

    struct Allocation
    {
        uint32_t pageCount;
        uint32_t protect;
    };

    uint32_t PageIndex(uint32_t guest) const { return (guest - m_begin) / PAGE_SIZE; }
    bool IsRangeFree(uint32_t first, uint32_t count) const;

    const char* m_name;
    uint32_t m_begin;
    uint32_t m_end;
    std::mutex m_mutex;
    std::vector<Page> m_pages;
    std::map<uint32_t, Allocation> m_allocations;  // by base address
};
