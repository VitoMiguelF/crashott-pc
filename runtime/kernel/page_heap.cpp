#include "page_heap.h"
#include "memory.h"
#include "../diagnostics.h"

#include <windows.h>

#include <cstring>

PageHeap::PageHeap(const char* name, uint32_t begin, uint32_t end)
    : m_name(name), m_begin(begin), m_end(end), m_pages((end - begin) / PAGE_SIZE)
{
}

bool PageHeap::IsRangeFree(uint32_t first, uint32_t count) const
{
    for (uint32_t i = first; i < first + count; i++)
    {
        if (m_pages[i].state != State::Free)
            return false;
    }
    return true;
}

uint32_t PageHeap::Reserve(uint32_t fixedBase, uint32_t size, uint32_t alignment, uint32_t protect, bool topDown,
                           uint32_t low, uint32_t high)
{
    std::lock_guard lock(m_mutex);

    uint32_t count = size / PAGE_SIZE;
    if (count == 0 || count > m_pages.size())
        return 0;

    uint32_t first;
    if (fixedBase)
    {
        if (!Contains(fixedBase) || uint64_t(fixedBase) + size > m_end)
            return 0;

        first = PageIndex(fixedBase);
        if (!IsRangeFree(first, count))
            return 0;
    }
    else
    {
        uint64_t lo = std::max<uint64_t>(low ? low : m_begin, m_begin);
        uint64_t hi = std::min<uint64_t>(high ? high : m_end, m_end);
        alignment = std::max(alignment, PAGE_SIZE);
        lo = (lo + alignment - 1) & ~uint64_t(alignment - 1);
        if (lo + size > hi)
            return 0;

        uint64_t found = 0;
        if (topDown)
        {
            for (uint64_t candidate = (hi - size) & ~uint64_t(alignment - 1); candidate >= lo; candidate -= alignment)
            {
                if (IsRangeFree(PageIndex(uint32_t(candidate)), count))
                {
                    found = candidate;
                    break;
                }
                if (candidate < alignment)
                    break;
            }
        }
        else
        {
            for (uint64_t candidate = lo; candidate + size <= hi; candidate += alignment)
            {
                if (IsRangeFree(PageIndex(uint32_t(candidate)), count))
                {
                    found = candidate;
                    break;
                }
            }
        }

        if (!found)
            return 0;

        first = PageIndex(uint32_t(found));
    }

    uint32_t base = m_begin + first * PAGE_SIZE;
    for (uint32_t i = first; i < first + count; i++)
        m_pages[i] = { base, protect, State::Reserved, m_pages[i].dirty };

    m_allocations[base] = { count, protect };

    // Drop anything the demand-commit handler mapped here while the range was free, so pages
    // committed later start zeroed.
    VirtualFree(g_memory.Translate(base), size, MEM_DECOMMIT);
    return base;
}

bool PageHeap::Commit(uint32_t guest, uint32_t size, uint32_t protect)
{
    std::lock_guard lock(m_mutex);

    if (size == 0 || !Contains(guest) || uint64_t(guest) + size > m_end)
        return false;

    uint32_t first = PageIndex(guest);
    uint32_t count = size / PAGE_SIZE;
    uint32_t allocationBase = m_pages[first].allocationBase;
    for (uint32_t i = first; i < first + count; i++)
    {
        if (m_pages[i].state == State::Free || m_pages[i].allocationBase != allocationBase)
            return false;
    }

    if (!g_memory.Commit(guest, size))
        Fatal("%s heap: host commit failed at guest 0x%08X (0x%X bytes)", m_name, guest, size);

    bool physical = g_memory.IsPhysicalView(guest);
    for (uint32_t i = first; i < first + count; i++)
    {
        if (physical && m_pages[i].state != State::Committed)
        {
            if (m_pages[i].dirty)
                memset(g_memory.Translate(m_begin + i * PAGE_SIZE), 0, PAGE_SIZE);
            m_pages[i].dirty = true;
        }
        m_pages[i].state = State::Committed;
        m_pages[i].protect = protect;
    }
    return true;
}

bool PageHeap::Decommit(uint32_t guest, uint32_t size)
{
    std::lock_guard lock(m_mutex);

    if (size == 0 || !Contains(guest) || uint64_t(guest) + size > m_end)
        return false;

    uint32_t first = PageIndex(guest);
    uint32_t count = size / PAGE_SIZE;
    for (uint32_t i = first; i < first + count; i++)
    {
        if (m_pages[i].state == State::Free)
            return false;
    }

    VirtualFree(g_memory.Translate(guest), size, MEM_DECOMMIT);
    for (uint32_t i = first; i < first + count; i++)
        m_pages[i].state = State::Reserved;

    return true;
}

uint32_t PageHeap::Release(uint32_t allocationBase)
{
    std::lock_guard lock(m_mutex);

    auto it = m_allocations.find(allocationBase);
    if (it == m_allocations.end())
        return 0;

    uint32_t first = PageIndex(allocationBase);
    uint32_t count = it->second.pageCount;
    VirtualFree(g_memory.Translate(allocationBase), count * PAGE_SIZE, MEM_DECOMMIT);
    for (uint32_t i = first; i < first + count; i++)
        m_pages[i] = { 0, 0, State::Free, m_pages[i].dirty };

    m_allocations.erase(it);
    return count * PAGE_SIZE;
}

PageHeap::RegionInfo PageHeap::Query(uint32_t guest)
{
    std::lock_guard lock(m_mutex);

    uint32_t first = PageIndex(guest);
    const Page& page = m_pages[first];

    RegionInfo info{};
    info.base = m_begin + first * PAGE_SIZE;
    info.state = page.state;
    info.protect = page.state == State::Committed ? page.protect : 0;
    info.allocationBase = page.allocationBase;
    if (page.state != State::Free)
        info.allocationProtect = m_allocations[page.allocationBase].protect;

    uint32_t last = first + 1;
    while (last < m_pages.size() && m_pages[last].state == page.state &&
           m_pages[last].allocationBase == page.allocationBase &&
           (page.state != State::Committed || m_pages[last].protect == page.protect))
    {
        last++;
    }

    info.size = (last - first) * PAGE_SIZE;
    return info;
}

uint32_t PageHeap::QueryProtect(uint32_t guest)
{
    std::lock_guard lock(m_mutex);

    const Page& page = m_pages[PageIndex(guest)];
    return page.state == State::Committed ? page.protect : 0;
}

uint32_t PageHeap::ReservedPageCount()
{
    std::lock_guard lock(m_mutex);

    uint32_t count = 0;
    for (auto& [base, allocation] : m_allocations)
        count += allocation.pageCount;
    return count;
}
