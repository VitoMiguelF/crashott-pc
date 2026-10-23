#pragma once

// Ring allocator with fence-based reclamation (rhi.h backends: the upload ring, D3D12's
// shader-visible descriptors). Offsets are handed out in order; OnSubmit marks the head with the
// fence (D3D12) or timeline value (Vulkan) of a submission, Retire frees up to the completed one.

#include <cstdint>
#include <deque>

namespace gpu::rhi
{
    inline uint64_t AlignUp64(uint64_t value, uint64_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    // Ring allocator with fence-based reclamation (upload memory, descriptors).
    class Ring
    {
    public:
        void Init(uint64_t capacity) { m_capacity = capacity; }

        // Returns the offset, or UINT64_MAX if there's no contiguous room right now.
        uint64_t TryAllocate(uint64_t size, uint64_t alignment)
        {
            if (size > m_capacity)
                return UINT64_MAX;
            uint64_t used = m_allocated - m_freed;
            if (used == 0)
                m_head = m_tail = 0;
            if (m_head >= m_tail && used < m_capacity)
            {
                uint64_t aligned = AlignUp64(m_head, alignment);
                if (aligned + size <= m_capacity)
                    return Take(aligned, size);
                // Wrap: the end of the buffer is wasted.
                if (size <= m_tail || used == 0)
                {
                    m_allocated += m_capacity - m_head;
                    m_head = 0;
                    return Take(0, size);
                }
                return UINT64_MAX;
            }
            if (m_head < m_tail)
            {
                uint64_t aligned = AlignUp64(m_head, alignment);
                if (aligned + size <= m_tail)
                    return Take(aligned, size);
            }
            return UINT64_MAX;
        }

        void OnSubmit(uint64_t fence) { m_marks.push_back({ fence, m_head, m_allocated }); }

        void Retire(uint64_t completedFence)
        {
            while (!m_marks.empty() && m_marks.front().fence <= completedFence)
            {
                m_tail = m_marks.front().position;
                m_freed = m_marks.front().allocated;
                m_marks.pop_front();
            }
        }

        bool HasPending() const { return !m_marks.empty(); }
        uint64_t OldestPendingFence() const { return m_marks.front().fence; }

    private:
        struct Mark
        {
            uint64_t fence;
            uint64_t position;
            uint64_t allocated;
        };

        uint64_t Take(uint64_t offset, uint64_t size)
        {
            m_allocated += offset + size - m_head;
            m_head = offset + size;
            if (m_head == m_capacity)
                m_head = 0;
            return offset;
        }

        uint64_t m_capacity = 0;
        uint64_t m_head = 0;
        uint64_t m_tail = 0;
        uint64_t m_allocated = 0;
        uint64_t m_freed = 0;
        std::deque<Mark> m_marks;
    };
}
