#include "objects.h"
#include "memory.h"
#include "../diagnostics.h"

#include <windows.h>

#include <condition_variable>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32_t OBJECT_SIGNATURE = 0x52434D50; // 'RCMP'
    constexpr uint32_t HEADER_SIZE = 0x10;
    constexpr uint32_t OBJECT_BLOCK_SIZE = 0x20;     // header + type-specific fields (semaphore limit, mutant owner)
    constexpr uint32_t HANDLE_BASE = 0xF8000000;

    std::mutex g_dispatcherMutex;
    std::condition_variable g_dispatcherCondition;

    // Host objects by index (stored in the guest header). Never freed: Ke* callers may still hold
    // the guest pointer after the last handle is closed.
    std::vector<KernelObject*> g_objects;

    std::mutex g_handleMutex;
    std::unordered_map<uint32_t, KernelObject*> g_handles;
    uint32_t g_nextHandle = HANDLE_BASE + 4;

    thread_local ThreadObject* t_currentThread;

    // With the dispatcher lock held.
    void RegisterLocked(KernelObject* object, uint32_t guest)
    {
        object->guest = guest;
        g_objects.push_back(object);

        StoreBE32(guest + 0x08, OBJECT_SIGNATURE);
        StoreBE32(guest + 0x0C, uint32_t(g_objects.size() - 1));
    }

    int32_t LoadSignalState(uint32_t guest)
    {
        return int32_t(LoadBE32(guest + 0x04));
    }
}

void KernelObject::SetGuestSignalState(int32_t state) const
{
    StoreBE32(guest + 0x04, uint32_t(state));
}

void EventObject::Acquire(uint32_t)
{
    if (type == DISPATCHER_SYNCHRONIZATION_EVENT)
        Set(false);
}

void EventObject::Set(bool value)
{
    signaled = value;
    SetGuestSignalState(value);
}

void SemaphoreObject::Acquire(uint32_t)
{
    count--;
    SetGuestSignalState(count);
}

void MutantObject::Acquire(uint32_t thread)
{
    owner = thread;
    recursion++;
    SetGuestSignalState(1 - recursion);
    StoreBE32(guest + 0x18, owner);
}

std::mutex& DispatcherMutex()
{
    return g_dispatcherMutex;
}

void NotifyDispatcher()
{
    g_dispatcherCondition.notify_all();
}

void RegisterObject(KernelObject* object, uint32_t guest)
{
    if (!guest)
        guest = GuestAlloc(OBJECT_BLOCK_SIZE);

    *g_memory.Translate(guest) = object->type;

    std::lock_guard lock(g_dispatcherMutex);
    RegisterLocked(object, guest);
}

KernelObject* ObjectFromGuest(uint32_t guest)
{
    if (guest < 0x10000)
        return nullptr;

    std::lock_guard lock(g_dispatcherMutex);

    if (LoadBE32(guest + 0x08) == OBJECT_SIGNATURE)
    {
        uint32_t index = LoadBE32(guest + 0x0C);
        if (index < g_objects.size() && g_objects[index]->guest == guest)
            return g_objects[index];
    }

    uint8_t type = *g_memory.Translate(guest);
    int32_t signalState = LoadSignalState(guest);
    KernelObject* object;

    switch (type)
    {
    case DISPATCHER_NOTIFICATION_EVENT:
    case DISPATCHER_SYNCHRONIZATION_EVENT:
    {
        auto* event = new EventObject();
        event->signaled = signalState != 0;
        object = event;
        break;
    }
    case DISPATCHER_SEMAPHORE:
    {
        auto* semaphore = new SemaphoreObject();
        semaphore->count = signalState;
        semaphore->limit = int32_t(LoadBE32(guest + HEADER_SIZE));
        object = semaphore;
        break;
    }
    case DISPATCHER_MUTANT:
    {
        auto* mutant = new MutantObject();
        mutant->owner = signalState > 0 ? 0 : LoadBE32(guest + 0x18);
        mutant->recursion = signalState > 0 ? 0 : 1 - signalState;
        object = mutant;
        break;
    }
    default:
        Log("[kernel] unsupported dispatcher object type %u at 0x%08X", type, guest);
        return nullptr;
    }

    object->type = type;
    RegisterLocked(object, guest);
    return object;
}

uint32_t CreateHandle(KernelObject* object)
{
    std::lock_guard lock(g_handleMutex);
    uint32_t handle = g_nextHandle;
    g_nextHandle += 4;
    g_handles[handle] = object;
    return handle;
}

KernelObject* ObjectFromHandle(uint32_t handle)
{
    if (handle == CURRENT_THREAD_HANDLE)
        return t_currentThread;

    std::lock_guard lock(g_handleMutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

bool CloseObjectHandle(uint32_t handle)
{
    KernelObject* object;
    {
        std::lock_guard lock(g_handleMutex);
        auto it = g_handles.find(handle);
        if (it == g_handles.end())
            return false;
        object = it->second;
        g_handles.erase(it);
    }

    object->OnHandleClosed();
    return true;
}

WaitDeadline DeadlineFromTimeout(uint32_t timeoutPtr)
{
    if (!timeoutPtr)
        return std::nullopt;

    auto now = std::chrono::steady_clock::now();
    int64_t value = int64_t(__builtin_bswap64(*reinterpret_cast<uint64_t*>(g_memory.Translate(timeoutPtr))));
    int64_t relative;

    if (value <= 0)
    {
        relative = -value;
    }
    else
    {
        FILETIME fileTime;
        GetSystemTimeAsFileTime(&fileTime);
        int64_t current = int64_t((uint64_t(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime);
        relative = value > current ? value - current : 0;
    }

    return now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(relative * 100));
}

uint32_t WaitForObjects(std::span<KernelObject* const> objects, bool waitAll, const WaitDeadline& deadline, uint32_t thread)
{
    std::unique_lock lock(g_dispatcherMutex);
    bool timedOut = false;

    while (true)
    {
        if (waitAll)
        {
            bool all = true;
            for (auto* object : objects)
                all = all && object->IsSignaled(thread);

            if (all)
            {
                for (auto* object : objects)
                    object->Acquire(thread);
                return X_STATUS_WAIT_0;
            }
        }
        else
        {
            for (size_t i = 0; i < objects.size(); i++)
            {
                if (objects[i]->IsSignaled(thread))
                {
                    objects[i]->Acquire(thread);
                    return X_STATUS_WAIT_0 + uint32_t(i);
                }
            }
        }

        // Objects are checked once more after the deadline passes, then the wait times out.
        if (timedOut)
            return X_STATUS_TIMEOUT;

        if (!deadline)
            g_dispatcherCondition.wait(lock);
        else
            timedOut = g_dispatcherCondition.wait_until(lock, *deadline) == std::cv_status::timeout;
    }
}

uint32_t CurrentKThread()
{
    return t_currentThread ? t_currentThread->guest : 0;
}

ThreadObject* CurrentThreadObject()
{
    return t_currentThread;
}

void SetCurrentThreadObject(ThreadObject* thread)
{
    t_currentThread = thread;
}
