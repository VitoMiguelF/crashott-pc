#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

// Kernel dispatcher objects (events, semaphores, mutants, threads) shared by the Nt*/Ke* imports.
//
// Every object has a guest DISPATCHER_HEADER, so the same object can be reached from a handle
// (Nt* functions, ObReferenceObjectByHandle) or from a guest pointer (Ke* functions):
//
//   0x00  u8 Type, u8 Absolute, u8 ProcessType, u8 Inserted
//   0x04  s32 SignalState
//   0x08  LIST_ENTRY WaitListHead
//
// As in UnleashedRecomp's QueryKernelObject, WaitListHead.Flink is overwritten with a signature and
// Blink with the host object's index once the runtime knows the object. A header the guest
// initialized itself (KeInitializeEvent is inline) has a real list there, so the first Ke* call on
// it creates the host object from Type/SignalState. Re-initializing the header inline starts a new
// host object.
//
// Host state is authoritative; SignalState in guest memory is kept in sync for code that reads it.
// All objects share one dispatcher lock and condition variable, which keeps WaitAll simple.

enum DispatcherType : uint8_t
{
    DISPATCHER_NOTIFICATION_EVENT = 0,
    DISPATCHER_SYNCHRONIZATION_EVENT = 1,
    DISPATCHER_MUTANT = 2,
    DISPATCHER_SEMAPHORE = 5,
    DISPATCHER_THREAD = 6,

    // Not a dispatcher type: file handles (kernel/file_imports.cpp). Always signaled.
    OBJECT_FILE = 0x80,
    // XAM enumerators (XamContentCreateEnumerator, kernel/xam_content_imports.cpp). Always signaled.
    OBJECT_ENUMERATOR = 0x81,
};

constexpr uint32_t X_STATUS_WAIT_0 = 0x00000000;
constexpr uint32_t X_STATUS_ABANDONED_WAIT_0 = 0x00000080;
constexpr uint32_t X_STATUS_USER_APC = 0x000000C0;
constexpr uint32_t X_STATUS_ALERTED = 0x00000101;
constexpr uint32_t X_STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t X_STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;
constexpr uint32_t X_STATUS_MUTANT_NOT_OWNED = 0xC0000046;
constexpr uint32_t X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;

constexpr uint32_t CURRENT_THREAD_HANDLE = 0xFFFFFFFE;

struct KernelObject
{
    uint32_t guest{};  // DISPATCHER_HEADER address
    uint8_t type{};

    virtual ~KernelObject() = default;

    // Both called with the dispatcher lock held. `thread` is the waiting KTHREAD.
    virtual bool IsSignaled(uint32_t thread) const = 0;
    virtual void Acquire(uint32_t thread) {}

    // Called when the handle to this object is closed (objects themselves are never freed).
    virtual void OnHandleClosed() {}

    // Writes `state` to the guest header's SignalState.
    void SetGuestSignalState(int32_t state) const;
};

struct EventObject : KernelObject
{
    bool signaled{};

    bool IsSignaled(uint32_t) const override { return signaled; }
    void Acquire(uint32_t) override;
    void Set(bool value);
};

struct SemaphoreObject : KernelObject
{
    int32_t count{};
    int32_t limit{};

    bool IsSignaled(uint32_t) const override { return count > 0; }
    void Acquire(uint32_t) override;
};

struct MutantObject : KernelObject
{
    uint32_t owner{};  // KTHREAD, 0 when free
    int32_t recursion{};

    bool IsSignaled(uint32_t thread) const override { return owner == 0 || owner == thread; }
    void Acquire(uint32_t thread) override;
};

struct GuestThreadContext;

struct ThreadObject : KernelObject
{
    GuestThreadContext* context{};
    void* hostHandle{};
    uint32_t threadId{};
    uint32_t entry{};  // guest function and its r3/r4
    uint32_t entryR3{};
    uint32_t entryR4{};
    bool exited{};
    uint32_t exitCode{};

    bool IsSignaled(uint32_t) const override { return exited; }
};

std::mutex& DispatcherMutex();

// Wakes every waiter so it re-checks its objects; call after signaling, with or without the lock.
void NotifyDispatcher();

// Gives `object` (already constructed, `type` set) a guest header: `guest` if nonzero, otherwise a
// fresh 0x20-byte block from the runtime heap. Writes Type and the signature, and keeps the object
// alive for the rest of the run. Takes the dispatcher lock.
void RegisterObject(KernelObject* object, uint32_t guest = 0);

// Host object for a guest dispatcher header, created from the header on first use.
// Returns nullptr for unsupported header types. Takes the dispatcher lock.
KernelObject* ObjectFromGuest(uint32_t guest);

// Handle table. Handles are 0xF8000004, 0xF8000008, ...; CURRENT_THREAD_HANDLE resolves to the
// calling thread.
uint32_t CreateHandle(KernelObject* object);
KernelObject* ObjectFromHandle(uint32_t handle);
bool CloseObjectHandle(uint32_t handle);

using WaitDeadline = std::optional<std::chrono::steady_clock::time_point>;

// Deadline for an NT timeout: guest pointer to a big-endian s64 in 100 ns units, negative =
// relative, positive = absolute system time; a null pointer waits forever.
WaitDeadline DeadlineFromTimeout(uint32_t timeoutPtr);

// Waits until one (waitAll = false) or all of `objects` are signaled and acquires them, or until
// the deadline. Returns X_STATUS_WAIT_0 + index (any), X_STATUS_WAIT_0 (all) or X_STATUS_TIMEOUT.
uint32_t WaitForObjects(std::span<KernelObject* const> objects, bool waitAll, const WaitDeadline& deadline, uint32_t thread);

// The calling guest thread's KTHREAD and host thread object (nullptr outside guest threads).
uint32_t CurrentKThread();
ThreadObject* CurrentThreadObject();
void SetCurrentThreadObject(ThreadObject* thread);
