// xboxkrnl file I/O on top of the virtual file system (fs/vfs.h): NtCreateFile/NtOpenFile,
// NtReadFile/NtWriteFile, NtQueryInformationFile/NtSetInformationFile, NtQueryDirectoryFile,
// NtQueryVolumeInformationFile, NtFlushBuffersFile (NtClose is in thread_imports.cpp and reaches
// FileObject::OnHandleClosed), plus RtlInitAnsiString.
//
// I/O completes synchronously: the IO_STATUS_BLOCK is filled and the optional event signaled
// before returning, and the result is never STATUS_PENDING. APC routines are not supported (the
// title's XAPI wrappers don't pass any). Guest structures are big-endian; layouts follow the NT
// ones, as in Xenia's xboxkrnl_io.cc.

#include "kernel.h"
#include "memory.h"
#include "objects.h"
#include "../fs/vfs.h"
#include "../fs/rcf_overlay.h"
#include "input.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t X_STATUS_UNSUCCESSFUL = 0xC0000001;
    constexpr uint32_t X_STATUS_INVALID_INFO_CLASS = 0xC0000003;
    constexpr uint32_t X_STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
    constexpr uint32_t X_STATUS_NO_SUCH_FILE = 0xC000000F;
    constexpr uint32_t X_STATUS_INVALID_DEVICE_REQUEST = 0xC0000010;
    constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
    constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;
    constexpr uint32_t X_STATUS_OBJECT_NAME_INVALID = 0xC0000033;
    constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
    constexpr uint32_t X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
    constexpr uint32_t X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
    constexpr uint32_t X_STATUS_MEDIA_WRITE_PROTECTED = 0xC00000A2;
    constexpr uint32_t X_STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA;
    constexpr uint32_t X_STATUS_NOT_A_DIRECTORY = 0xC0000103;
    constexpr uint32_t X_STATUS_BUFFER_OVERFLOW = 0x80000005;
    constexpr uint32_t X_STATUS_NO_MORE_FILES = 0x80000006;

    constexpr uint32_t DOS_DEVICES_DIRECTORY = 0xFFFFFFFD;

    // CreateDisposition and the IO_STATUS_BLOCK.Information values for opens.
    enum : uint32_t { X_FILE_SUPERSEDE, X_FILE_OPEN, X_FILE_CREATE, X_FILE_OPEN_IF, X_FILE_OVERWRITE, X_FILE_OVERWRITE_IF };
    enum : uint32_t { X_FILE_SUPERSEDED, X_FILE_OPENED, X_FILE_CREATED, X_FILE_OVERWRITTEN };

    constexpr uint32_t X_FILE_DIRECTORY_FILE = 0x00000001;
    constexpr uint32_t X_FILE_NON_DIRECTORY_FILE = 0x00000040;

    constexpr uint32_t X_FILE_ATTRIBUTE_READONLY = 0x01;
    constexpr uint32_t X_FILE_ATTRIBUTE_DIRECTORY = 0x10;
    constexpr uint32_t X_FILE_ATTRIBUTE_NORMAL = 0x80;

    // Access bits that need write access to the file.
    constexpr uint32_t WRITE_ACCESS_MASK = 0x40000000 /* GENERIC_WRITE */ | 0x10000000 /* GENERIC_ALL */ |
                                           0x00000002 /* FILE_WRITE_DATA */ | 0x00000004 /* FILE_APPEND_DATA */;

    constexpr uint64_t FILE_USE_FILE_POINTER_POSITION = 0xFFFFFFFFFFFFFFFEull;
    constexpr uint64_t FILE_WRITE_TO_END_OF_FILE = 0xFFFFFFFFFFFFFFFFull;

    struct FileObject : KernelObject
    {
        vfs::VirtualPath path;
        fs::path host;
        std::string source;
        HANDLE hostFile = INVALID_HANDLE_VALUE;
        std::shared_ptr<const rcf_overlay::Archive> archive;  // .rcf with entries replaced by mods
        bool directory{};
        bool canWrite{};
        bool deleteOnClose{};

        std::mutex mutex;
        uint64_t position{};
        std::vector<vfs::DirectoryEntry> entries;  // NtQueryDirectoryFile scan
        size_t nextEntry{};
        bool scanned{};

        bool IsSignaled(uint32_t) const override { return true; }

        void OnHandleClosed() override
        {
            if (hostFile != INVALID_HANDLE_VALUE)
                CloseHandle(hostFile);
            hostFile = INVALID_HANDLE_VALUE;

            if (deleteOnClose && path.Writable())
            {
                std::error_code error;
                fs::remove(host, error);
                Log("[fs] deleted %s (%s)", path.Display().c_str(), host.string().c_str());
            }
        }
    };

    struct HostInfo
    {
        uint64_t creation{}, access{}, write{}, change{};
        uint64_t size{}, allocation{};
        uint32_t attributes{};
    };

    uint64_t FileTimeValue(const FILETIME& time)
    {
        return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    HostInfo MakeInfo(const FILETIME& creation, const FILETIME& access, const FILETIME& write,
                      DWORD sizeHigh, DWORD sizeLow, DWORD attributes, bool readOnlyMount)
    {
        HostInfo info;
        info.creation = FileTimeValue(creation);
        info.access = FileTimeValue(access);
        info.write = FileTimeValue(write);
        info.change = info.write;
        info.size = (uint64_t(sizeHigh) << 32) | sizeLow;
        info.allocation = (info.size + 0xFFF) & ~0xFFFull;

        if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            info.attributes = X_FILE_ATTRIBUTE_DIRECTORY;
            info.size = info.allocation = 0;
        }
        else
        {
            info.attributes = attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
        }
        if (readOnlyMount)
            info.attributes |= X_FILE_ATTRIBUTE_READONLY;
        if (!info.attributes)
            info.attributes = X_FILE_ATTRIBUTE_NORMAL;
        return info;
    }

    bool QueryHostPath(const fs::path& host, bool readOnlyMount, HostInfo& info)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(host.c_str(), GetFileExInfoStandard, &data))
            return false;
        info = MakeInfo(data.ftCreationTime, data.ftLastAccessTime, data.ftLastWriteTime,
                        data.nFileSizeHigh, data.nFileSizeLow, data.dwFileAttributes, readOnlyMount);
        return true;
    }

    bool QueryFile(FileObject& file, HostInfo& info)
    {
        if (file.hostFile == INVALID_HANDLE_VALUE)
            return QueryHostPath(file.host, !file.path.Writable(), info);

        BY_HANDLE_FILE_INFORMATION data;
        if (!GetFileInformationByHandle(file.hostFile, &data))
            return false;
        info = MakeInfo(data.ftCreationTime, data.ftLastAccessTime, data.ftLastWriteTime,
                        data.nFileSizeHigh, data.nFileSizeLow, data.dwFileAttributes, !file.path.Writable());
        if (file.archive)
        {
            info.size = rcf_overlay::Size(*file.archive);
            info.allocation = (info.size + 0xFFF) & ~0xFFFull;
        }
        return true;
    }

    FileObject* FileFromHandle(uint32_t handle)
    {
        KernelObject* object = ObjectFromHandle(handle);
        return object && object->type == OBJECT_FILE ? static_cast<FileObject*>(object) : nullptr;
    }

    std::string LoadAnsiString(uint8_t* base, uint32_t string)
    {
        if (!string)
            return {};
        uint16_t length = PPC_LOAD_U16(string + 0x0);
        uint32_t buffer = PPC_LOAD_U32(string + 0x4);
        return buffer ? std::string(reinterpret_cast<const char*>(base + buffer), length) : std::string();
    }

    void StoreBE64(uint8_t* base, uint32_t guest, uint64_t value)
    {
        PPC_STORE_U64(guest, value);
    }

    void CompleteIo(uint8_t* base, uint32_t ioStatus, uint32_t status, uint32_t information)
    {
        if (!ioStatus)
            return;
        PPC_STORE_U32(ioStatus + 0x0, status);
        PPC_STORE_U32(ioStatus + 0x4, information);
    }

    void SignalEventHandle(uint32_t handle)
    {
        if (!handle)
            return;

        KernelObject* object = ObjectFromHandle(handle);
        if (!object || (object->type != DISPATCHER_NOTIFICATION_EVENT && object->type != DISPATCHER_SYNCHRONIZATION_EVENT))
        {
            Log("[fs] I/O completion event 0x%08X is not an event", handle);
            return;
        }

        {
            std::lock_guard lock(DispatcherMutex());
            static_cast<EventObject*>(object)->Set(true);
        }
        NotifyDispatcher();
    }

    void WarnApc(uint32_t apcRoutine, const char* name)
    {
        static std::atomic<bool> warned;
        if (apcRoutine && !warned.exchange(true))
            Log("[fs] %s with an APC routine (0x%08X): APCs are not supported, the routine is not called", name, apcRoutine);
    }

    // Shared by NtCreateFile and NtOpenFile.
    uint32_t OpenFile(uint8_t* base, const char* api, uint32_t handlePtr, uint32_t access, uint32_t attributes,
                      uint32_t ioStatus, uint32_t disposition, uint32_t options, uint32_t lr)
    {
        uint32_t root = attributes ? PPC_LOAD_U32(attributes + 0x0) : 0;
        std::string name = attributes ? LoadAnsiString(base, PPC_LOAD_U32(attributes + 0x4)) : std::string();

        uint32_t information = 0;
        auto fail = [&](uint32_t status, const char* why)
        {
            Log("[fs] %s(\"%s\"%s, access=0x%08X, disposition=%u, options=0x%X) lr=0x%08X -> 0x%08X (%s)",
                api, name.c_str(), root && root != DOS_DEVICES_DIRECTORY ? " relative" : "", access, disposition, options, lr, status, why);
            if (handlePtr)
                PPC_STORE_U32(handlePtr, 0);
            CompleteIo(base, ioStatus, status, 0);
            return status;
        };

        const vfs::VirtualPath* directory = nullptr;
        if (root && root != DOS_DEVICES_DIRECTORY)
        {
            FileObject* rootFile = FileFromHandle(root);
            if (!rootFile)
                return fail(X_STATUS_INVALID_HANDLE, "bad RootDirectory");
            directory = &rootFile->path;
        }

        auto path = vfs::Parse(name, directory);
        if (!path)
            return fail(X_STATUS_OBJECT_PATH_NOT_FOUND, "unknown device or invalid path");

        auto location = vfs::Locate(*path);
        bool exists = location.has_value();
        bool writable = path->Writable();
        fs::path host = exists ? location->host : vfs::WritableHostPath(*path);
        std::string source = exists ? location->source : "userdata";
        std::error_code error;

        if (!exists)
        {
            // NOT_FOUND only when the parent directory exists.
            auto parent = path->relative.find_last_of('\\');
            vfs::VirtualPath parentPath{ path->mount, parent == std::string::npos ? std::string() : path->relative.substr(0, parent) };
            bool parentExists = vfs::Locate(parentPath).has_value();

            if (disposition == X_FILE_OPEN || disposition == X_FILE_OVERWRITE)
                return fail(parentExists ? X_STATUS_OBJECT_NAME_NOT_FOUND : X_STATUS_OBJECT_PATH_NOT_FOUND, "not found");
            if (!writable)
                return fail(X_STATUS_MEDIA_WRITE_PROTECTED, "the game folder is read-only");
            if (!parentExists)
            {
                if (parentPath.relative.empty())
                    fs::create_directories(vfs::WritableHostPath(parentPath), error);
                else
                    return fail(X_STATUS_OBJECT_PATH_NOT_FOUND, "parent directory not found");
            }
        }
        else if (disposition == X_FILE_CREATE)
        {
            return fail(X_STATUS_OBJECT_NAME_COLLISION, "already exists");
        }
        else if (!writable && disposition != X_FILE_OPEN && disposition != X_FILE_OPEN_IF)
        {
            return fail(X_STATUS_MEDIA_WRITE_PROTECTED, "the game folder is read-only");
        }

        bool isDirectory = exists ? fs::is_directory(host, error) : (options & X_FILE_DIRECTORY_FILE) != 0;
        if ((options & X_FILE_DIRECTORY_FILE) && !isDirectory)
            return fail(X_STATUS_NOT_A_DIRECTORY, "not a directory");
        if ((options & X_FILE_NON_DIRECTORY_FILE) && isDirectory)
            return fail(X_STATUS_FILE_IS_A_DIRECTORY, "is a directory");

        bool wantsWrite = (access & WRITE_ACCESS_MASK) != 0;
        auto* file = new FileObject();
        file->type = OBJECT_FILE;
        file->path = *path;
        file->host = host;
        file->source = source;
        file->directory = isDirectory;
        file->canWrite = writable && wantsWrite;

        if (isDirectory)
        {
            if (!exists && !fs::create_directories(host, error))
            {
                delete file;
                return fail(X_STATUS_UNSUCCESSFUL, "could not create the directory");
            }
            information = exists ? X_FILE_OPENED : X_FILE_CREATED;
        }
        else
        {
            DWORD creation = OPEN_EXISTING;
            if (writable)
            {
                switch (disposition)
                {
                case X_FILE_SUPERSEDE:
                case X_FILE_OVERWRITE_IF: creation = CREATE_ALWAYS; break;
                case X_FILE_CREATE: creation = CREATE_NEW; break;
                case X_FILE_OPEN_IF: creation = OPEN_ALWAYS; break;
                case X_FILE_OVERWRITE: creation = TRUNCATE_EXISTING; break;
                default: creation = OPEN_EXISTING; break;
                }
            }

            // Truncation needs write access on the host even if the title only asked to read.
            bool truncates = creation == CREATE_ALWAYS || creation == TRUNCATE_EXISTING;
            DWORD hostAccess = GENERIC_READ | (file->canWrite || (writable && truncates) ? GENERIC_WRITE : 0);
            file->hostFile = CreateFileW(host.c_str(), hostAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file->hostFile == INVALID_HANDLE_VALUE)
            {
                DWORD hostError = GetLastError();
                delete file;
                char why[64];
                snprintf(why, sizeof(why), "host error %lu", hostError);
                return fail(hostError == ERROR_ACCESS_DENIED || hostError == ERROR_SHARING_VIOLATION ? X_STATUS_ACCESS_DENIED : X_STATUS_UNSUCCESSFUL, why);
            }

            if (!writable)
                file->archive = rcf_overlay::Open(*path, host);

            if (!exists)
                information = X_FILE_CREATED;
            else if (disposition == X_FILE_SUPERSEDE)
                information = X_FILE_SUPERSEDED;
            else if (truncates)
                information = X_FILE_OVERWRITTEN;
            else
                information = X_FILE_OPENED;
        }

        RegisterObject(file);
        uint32_t handle = CreateHandle(file);
        if (handlePtr)
            PPC_STORE_U32(handlePtr, handle);
        CompleteIo(base, ioStatus, X_STATUS_SUCCESS, information);

        Log("[fs] %s %s%s -> handle 0x%08X, %s (%s)%s%s", api, path->Display().c_str(), isDirectory ? " [dir]" : "",
            handle, source.c_str(), host.string().c_str(),
            information == X_FILE_CREATED ? ", created" : information == X_FILE_OVERWRITTEN || information == X_FILE_SUPERSEDED ? ", truncated" : "",
            wantsWrite && !writable ? ", write access dropped (read-only)" : "");
        return X_STATUS_SUCCESS;
    }

    // ReadFile/WriteFile at an offset, into/out of guest memory. Kernel-mode copies into guest
    // pages that aren't committed yet fail (ERROR_NOACCESS) instead of faulting, so those go
    // through a host buffer (whose memcpy commits on demand).
    bool HostTransfer(HANDLE file, bool write, uint8_t* data, uint32_t length, uint64_t offset, DWORD& transferred)
    {
        OVERLAPPED overlapped{};
        overlapped.Offset = DWORD(offset);
        overlapped.OffsetHigh = DWORD(offset >> 32);
        transferred = 0;

        BOOL ok = write ? WriteFile(file, data, length, &transferred, &overlapped)
                        : ReadFile(file, data, length, &transferred, &overlapped);
        if (ok)
            return true;

        DWORD error = GetLastError();
        if (error == ERROR_HANDLE_EOF)
            return true;
        if (error != ERROR_NOACCESS)
            return false;

        std::vector<uint8_t> buffer(length);
        if (write)
            memcpy(buffer.data(), data, length);
        ok = write ? WriteFile(file, buffer.data(), length, &transferred, &overlapped)
                   : ReadFile(file, buffer.data(), length, &transferred, &overlapped);
        if (!ok && GetLastError() != ERROR_HANDLE_EOF)
            return false;
        if (!write)
            memcpy(data, buffer.data(), transferred);
        return true;
    }

    // NtReadFile/NtWriteFile(HANDLE File, HANDLE Event, void* ApcRoutine, void* ApcContext,
    //                        IO_STATUS_BLOCK* IoStatus, void* Buffer, u32 Length, s64* ByteOffset)
    uint32_t TransferFile(PPCContext& ctx, uint8_t* base, bool write, bool logCall)
    {
        const char* api = write ? "NtWriteFile" : "NtReadFile";
        uint32_t handle = ctx.r3.u32;
        uint32_t event = ctx.r4.u32;
        uint32_t ioStatus = ctx.r7.u32;
        uint32_t buffer = ctx.r8.u32;
        uint32_t length = ctx.r9.u32;
        uint32_t offsetPtr = ctx.r10.u32;
        WarnApc(ctx.r5.u32, api);

        FileObject* file = FileFromHandle(handle);
        if (!file)
        {
            Log("[fs] %s(0x%08X): not a file handle (lr=0x%08X)", api, handle, GuestLR(ctx));
            return X_STATUS_INVALID_HANDLE;
        }
        if (file->directory)
            return X_STATUS_INVALID_DEVICE_REQUEST;
        if (write && !file->canWrite)
        {
            Log("[fs] NtWriteFile to %s refused: %s", file->path.Display().c_str(),
                file->path.Writable() ? "opened without write access" : "the game folder is read-only");
            CompleteIo(base, ioStatus, X_STATUS_ACCESS_DENIED, 0);
            return X_STATUS_ACCESS_DENIED;
        }

        std::lock_guard lock(file->mutex);

        uint64_t offset = offsetPtr ? PPC_LOAD_U64(offsetPtr) : FILE_USE_FILE_POINTER_POSITION;
        if (offset == FILE_USE_FILE_POINTER_POSITION)
        {
            offset = file->position;
        }
        else if (offset == FILE_WRITE_TO_END_OF_FILE && write)
        {
            LARGE_INTEGER size{};
            GetFileSizeEx(file->hostFile, &size);
            offset = uint64_t(size.QuadPart);
        }

        DWORD transferred;
        uint32_t status;
        bool ok;
        if (file->archive && !write)
        {
            // Through a host buffer: guest pages may not be committed yet (see HostTransfer).
            std::vector<uint8_t> data(length);
            uint32_t read = 0;
            ok = rcf_overlay::Read(*file->archive, file->hostFile, offset, data.data(), length, read);
            memcpy(base + buffer, data.data(), read);
            transferred = read;
        }
        else
        {
            ok = HostTransfer(file->hostFile, write, base + buffer, length, offset, transferred);
        }
        if (!ok)
        {
            Log("[fs] %s(%s, 0x%X bytes at 0x%llX) failed: host error %lu", api, file->path.Display().c_str(), length,
                (unsigned long long)offset, GetLastError());
            status = X_STATUS_UNSUCCESSFUL;
        }
        else
        {
            status = !write && transferred == 0 && length > 0 ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
            file->position = offset + transferred;
            if (!write && !file->path.Writable())
                input::NoteGameRead(offset, transferred, _wcsicmp(file->host.filename().c_str(), L"default.rcf") == 0);
        }

        CompleteIo(base, ioStatus, status, transferred);
        SignalEventHandle(event);

        static const bool logAll = getenv("CRASH_LOG_ALL_IO") != nullptr; // every transfer, not just the first ones
        if (logCall || logAll)
        {
            Log("[fs] %s(%s, buffer=0x%08X, 0x%X bytes at 0x%llX) lr=0x%08X -> 0x%08X, 0x%lX bytes", api, file->path.Display().c_str(),
                buffer, length, (unsigned long long)offset, GuestLR(ctx), status, transferred);
        }
        return status;
    }
}

// void RtlInitAnsiString(ANSI_STRING* Destination, const char* Source)
PPC_FUNC(__imp__RtlInitAnsiString)
{
    KERNEL_IMPORT_ENTER(RtlInitAnsiString);

    uint32_t destination = ctx.r3.u32;
    uint32_t source = ctx.r4.u32;
    uint16_t length = source ? uint16_t(strlen(reinterpret_cast<const char*>(base + source))) : 0;

    PPC_STORE_U16(destination + 0x0, length);
    PPC_STORE_U16(destination + 0x2, source ? uint16_t(length + 1) : 0);
    PPC_STORE_U32(destination + 0x4, source);
}

// NTSTATUS NtCreateFile(HANDLE* Handle, u32 DesiredAccess, OBJECT_ATTRIBUTES* Attributes,
//                       IO_STATUS_BLOCK* IoStatus, s64* AllocationSize, u32 FileAttributes,
//                       u32 ShareAccess, u32 CreateDisposition, u32 CreateOptions (stack))
PPC_FUNC(__imp__NtCreateFile)
{
    KERNEL_IMPORT_ENTER(NtCreateFile);
    ctx.r3.u64 = OpenFile(base, "NtCreateFile", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32,
                          GuestStackArgument(ctx, base, 8), GuestLR(ctx));
}

// NTSTATUS NtOpenFile(HANDLE* Handle, u32 DesiredAccess, OBJECT_ATTRIBUTES* Attributes,
//                     IO_STATUS_BLOCK* IoStatus, u32 OpenOptions)
PPC_FUNC(__imp__NtOpenFile)
{
    KERNEL_IMPORT_ENTER(NtOpenFile);
    ctx.r3.u64 = OpenFile(base, "NtOpenFile", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, X_FILE_OPEN,
                          ctx.r7.u32, GuestLR(ctx));
}

PPC_FUNC(__imp__NtReadFile)
{
    KERNEL_IMPORT_ENTER(NtReadFile);
    ctx.r3.u64 = TransferFile(ctx, base, false, logCall);
}

PPC_FUNC(__imp__NtWriteFile)
{
    KERNEL_IMPORT_ENTER(NtWriteFile);
    ctx.r3.u64 = TransferFile(ctx, base, true, logCall);
}

// NTSTATUS NtQueryInformationFile(HANDLE File, IO_STATUS_BLOCK* IoStatus, void* Info, u32 Length, u32 Class)
PPC_FUNC(__imp__NtQueryInformationFile)
{
    KERNEL_IMPORT_ENTER(NtQueryInformationFile);

    uint32_t handle = ctx.r3.u32;
    uint32_t ioStatus = ctx.r4.u32;
    uint32_t info = ctx.r5.u32;
    uint32_t length = ctx.r6.u32;
    uint32_t infoClass = ctx.r7.u32;

    FileObject* file = FileFromHandle(handle);
    if (!file)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }

    HostInfo host{};
    QueryFile(*file, host);

    uint32_t size;
    switch (infoClass)
    {
    case 4: size = 0x28; break;   // FileBasicInformation
    case 5: size = 0x18; break;   // FileStandardInformation
    case 6: size = 0x08; break;   // FileInternalInformation
    case 14: size = 0x08; break;  // FilePositionInformation
    case 20: size = 0x08; break;  // FileEndOfFileInformation
    case 34: size = 0x38; break;  // FileNetworkOpenInformation
    default:
        Log("[fs] NtQueryInformationFile(%s, class %u): unsupported class (lr=0x%08X)", file->path.Display().c_str(), infoClass, GuestLR(ctx));
        ctx.r3.u64 = X_STATUS_INVALID_INFO_CLASS;
        return;
    }

    if (length < size)
    {
        ctx.r3.u64 = X_STATUS_INFO_LENGTH_MISMATCH;
        return;
    }

    memset(base + info, 0, size);
    switch (infoClass)
    {
    case 4:
        StoreBE64(base, info + 0x00, host.creation);
        StoreBE64(base, info + 0x08, host.access);
        StoreBE64(base, info + 0x10, host.write);
        StoreBE64(base, info + 0x18, host.change);
        PPC_STORE_U32(info + 0x20, host.attributes);
        break;
    case 5:
        StoreBE64(base, info + 0x00, host.allocation);
        StoreBE64(base, info + 0x08, host.size);
        PPC_STORE_U32(info + 0x10, 1);
        PPC_STORE_U8(info + 0x14, file->deleteOnClose);
        PPC_STORE_U8(info + 0x15, file->directory);
        break;
    case 6:
        StoreBE64(base, info, file->guest);
        break;
    case 14:
    {
        std::lock_guard lock(file->mutex);
        StoreBE64(base, info, file->position);
        break;
    }
    case 20:
        StoreBE64(base, info, host.size);
        break;
    case 34:
        StoreBE64(base, info + 0x00, host.creation);
        StoreBE64(base, info + 0x08, host.access);
        StoreBE64(base, info + 0x10, host.write);
        StoreBE64(base, info + 0x18, host.change);
        StoreBE64(base, info + 0x20, host.allocation);
        StoreBE64(base, info + 0x28, host.size);
        PPC_STORE_U32(info + 0x30, host.attributes);
        break;
    }

    CompleteIo(base, ioStatus, X_STATUS_SUCCESS, size);
    KERNEL_LOG("[fs] NtQueryInformationFile(%s, class %u) lr=0x%08X -> size 0x%llX", file->path.Display().c_str(), infoClass,
               GuestLR(ctx), (unsigned long long)host.size);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

// NTSTATUS NtSetInformationFile(HANDLE File, IO_STATUS_BLOCK* IoStatus, void* Info, u32 Length, u32 Class)
PPC_FUNC(__imp__NtSetInformationFile)
{
    KERNEL_IMPORT_ENTER(NtSetInformationFile);

    uint32_t handle = ctx.r3.u32;
    uint32_t ioStatus = ctx.r4.u32;
    uint32_t info = ctx.r5.u32;
    uint32_t length = ctx.r6.u32;
    uint32_t infoClass = ctx.r7.u32;

    FileObject* file = FileFromHandle(handle);
    if (!file)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }

    uint32_t status = X_STATUS_SUCCESS;
    switch (infoClass)
    {
    case 14: // FilePositionInformation
    {
        std::lock_guard lock(file->mutex);
        file->position = PPC_LOAD_U64(info);
        break;
    }
    case 20: // FileEndOfFileInformation
    case 19: // FileAllocationInformation
        if (!file->canWrite)
        {
            status = X_STATUS_ACCESS_DENIED;
        }
        else if (infoClass == 20)
        {
            FILE_END_OF_FILE_INFO endOfFile{};
            endOfFile.EndOfFile.QuadPart = int64_t(PPC_LOAD_U64(info));
            if (!SetFileInformationByHandle(file->hostFile, FileEndOfFileInfo, &endOfFile, sizeof(endOfFile)))
                status = X_STATUS_UNSUCCESSFUL;
        }
        break;
    case 13: // FileDispositionInformation
        if (file->path.Writable())
            file->deleteOnClose = PPC_LOAD_U8(info) != 0;
        else
            status = X_STATUS_ACCESS_DENIED;
        break;
    case 4: // FileBasicInformation: times/attributes are not changed
        if (!file->path.Writable())
            status = X_STATUS_ACCESS_DENIED;
        break;
    default:
        Log("[fs] NtSetInformationFile(%s, class %u): unsupported class (lr=0x%08X)", file->path.Display().c_str(), infoClass, GuestLR(ctx));
        status = X_STATUS_INVALID_INFO_CLASS;
        break;
    }

    if (status == X_STATUS_ACCESS_DENIED)
        Log("[fs] NtSetInformationFile(%s, class %u) refused: %s", file->path.Display().c_str(), infoClass,
            file->path.Writable() ? "no write access" : "the game folder is read-only");

    CompleteIo(base, ioStatus, status, status == X_STATUS_SUCCESS ? length : 0);
    KERNEL_LOG("[fs] NtSetInformationFile(%s, class %u) lr=0x%08X -> 0x%08X", file->path.Display().c_str(), infoClass, GuestLR(ctx), status);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryVolumeInformationFile(HANDLE File, IO_STATUS_BLOCK* IoStatus, void* Info, u32 Length, u32 Class)
PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    KERNEL_IMPORT_ENTER(NtQueryVolumeInformationFile);

    uint32_t handle = ctx.r3.u32;
    uint32_t ioStatus = ctx.r4.u32;
    uint32_t info = ctx.r5.u32;
    uint32_t length = ctx.r6.u32;
    uint32_t infoClass = ctx.r7.u32;

    FileObject* file = FileFromHandle(handle);
    if (!file)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }

    bool writable = file->path.Writable();
    const char* label = writable ? "USERDATA" : "CRASH";
    const char* fsName = writable ? "FATX" : "GDFX";
    uint32_t size;
    uint32_t status = X_STATUS_SUCCESS;

    switch (infoClass)
    {
    case 1: // FileFsVolumeInformation: creation time, serial, label length, supports objects, label
        size = 0x14 + uint32_t(strlen(label));
        if (length < size) { status = X_STATUS_BUFFER_OVERFLOW; break; }
        memset(base + info, 0, size);
        PPC_STORE_U32(info + 0x08, 0x43524153);
        PPC_STORE_U32(info + 0x0C, uint32_t(strlen(label)));
        memcpy(base + info + 0x14, label, strlen(label));
        break;
    case 3: // FileFsSizeInformation: total units, available units, sectors per unit, bytes per sector
    {
        size = 0x18;
        if (length < size) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
        uint64_t totalUnits = writable ? 0x200000 : 0x100000; // 8 GB / 4 GB in 4 KB units
        StoreBE64(base, info + 0x00, totalUnits);
        StoreBE64(base, info + 0x08, writable ? totalUnits / 2 : 0);
        PPC_STORE_U32(info + 0x10, writable ? 8 : 2);
        PPC_STORE_U32(info + 0x14, writable ? 512 : 2048);
        break;
    }
    case 4: // FileFsDeviceInformation: device type, characteristics
        size = 0x08;
        if (length < size) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
        PPC_STORE_U32(info + 0x00, writable ? 0x07 /* FILE_DEVICE_DISK */ : 0x02 /* FILE_DEVICE_CD_ROM */);
        PPC_STORE_U32(info + 0x04, writable ? 0 : 0x03 /* removable, read-only */);
        break;
    case 5: // FileFsAttributeInformation: attributes, max component length, name length, name
        size = 0x0C + uint32_t(strlen(fsName));
        if (length < size) { status = X_STATUS_BUFFER_OVERFLOW; break; }
        PPC_STORE_U32(info + 0x00, writable ? 0 : 0x00080000 /* FILE_READ_ONLY_VOLUME */);
        PPC_STORE_U32(info + 0x04, 255);
        PPC_STORE_U32(info + 0x08, uint32_t(strlen(fsName)));
        memcpy(base + info + 0x0C, fsName, strlen(fsName));
        break;
    default:
        Log("[fs] NtQueryVolumeInformationFile(%s, class %u): unsupported class (lr=0x%08X)", file->path.Display().c_str(), infoClass, GuestLR(ctx));
        size = 0;
        status = X_STATUS_INVALID_INFO_CLASS;
        break;
    }

    CompleteIo(base, ioStatus, status, status == X_STATUS_SUCCESS ? size : 0);
    KERNEL_LOG("[fs] NtQueryVolumeInformationFile(%s, class %u) lr=0x%08X -> 0x%08X", file->path.Display().c_str(), infoClass, GuestLR(ctx), status);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryDirectoryFile(HANDLE File, HANDLE Event, void* ApcRoutine, void* ApcContext,
//                               IO_STATUS_BLOCK* IoStatus, FILE_DIRECTORY_INFORMATION* Info,
//                               u32 Length, ANSI_STRING* FileName, BOOL RestartScan (stack))
// One entry per call, as in Xenia:
//   0x00 NextEntryOffset, 0x04 FileIndex, 0x08 creation, 0x10 access, 0x18 write, 0x20 change,
//   0x28 EndOfFile, 0x30 AllocationSize, 0x38 attributes, 0x3C name length, 0x40 name (chars).
PPC_FUNC(__imp__NtQueryDirectoryFile)
{
    KERNEL_IMPORT_ENTER(NtQueryDirectoryFile);

    uint32_t handle = ctx.r3.u32;
    uint32_t event = ctx.r4.u32;
    uint32_t ioStatus = ctx.r7.u32;
    uint32_t info = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    std::string pattern = LoadAnsiString(base, ctx.r10.u32);
    bool restart = GuestStackArgument(ctx, base, 8) != 0;
    WarnApc(ctx.r5.u32, "NtQueryDirectoryFile");

    FileObject* file = FileFromHandle(handle);
    if (!file || !file->directory)
    {
        ctx.r3.u64 = file ? X_STATUS_INVALID_PARAMETER : X_STATUS_INVALID_HANDLE;
        return;
    }

    std::lock_guard lock(file->mutex);
    bool firstCall = !file->scanned || restart;
    if (firstCall)
    {
        file->entries.clear();
        for (auto& entry : vfs::List(file->path))
        {
            if (vfs::MatchWildcard(pattern, entry.name))
                file->entries.push_back(entry);
        }
        file->nextEntry = 0;
        file->scanned = true;
        Log("[fs] NtQueryDirectoryFile(%s, \"%s\") lr=0x%08X: %zu match(es)", file->path.Display().c_str(), pattern.c_str(),
            GuestLR(ctx), file->entries.size());
    }

    uint32_t status;
    uint32_t written = 0;
    if (file->nextEntry >= file->entries.size())
    {
        status = firstCall ? X_STATUS_NO_SUCH_FILE : X_STATUS_NO_MORE_FILES;
    }
    else
    {
        auto& entry = file->entries[file->nextEntry];
        uint32_t needed = 0x40 + uint32_t(entry.name.size());
        if (length < needed)
        {
            status = X_STATUS_BUFFER_OVERFLOW;
        }
        else
        {
            HostInfo host{};
            QueryHostPath(entry.host, !file->path.Writable(), host);
            memset(base + info, 0, needed);
            PPC_STORE_U32(info + 0x04, uint32_t(file->nextEntry));
            StoreBE64(base, info + 0x08, host.creation);
            StoreBE64(base, info + 0x10, host.access);
            StoreBE64(base, info + 0x18, host.write);
            StoreBE64(base, info + 0x20, host.change);
            StoreBE64(base, info + 0x28, host.size);
            StoreBE64(base, info + 0x30, host.allocation);
            PPC_STORE_U32(info + 0x38, host.attributes);
            PPC_STORE_U32(info + 0x3C, uint32_t(entry.name.size()));
            memcpy(base + info + 0x40, entry.name.data(), entry.name.size());
            file->nextEntry++;
            written = needed;
            status = X_STATUS_SUCCESS;
        }
    }

    CompleteIo(base, ioStatus, status, written);
    SignalEventHandle(event);
    ctx.r3.u64 = status;
}

// NTSTATUS NtFlushBuffersFile(HANDLE File, IO_STATUS_BLOCK* IoStatus)
PPC_FUNC(__imp__NtFlushBuffersFile)
{
    KERNEL_IMPORT_ENTER(NtFlushBuffersFile);

    FileObject* file = FileFromHandle(ctx.r3.u32);
    if (!file)
    {
        ctx.r3.u64 = X_STATUS_INVALID_HANDLE;
        return;
    }
    if (file->canWrite && file->hostFile != INVALID_HANDLE_VALUE)
        FlushFileBuffers(file->hostFile);
    CompleteIo(base, ctx.r4.u32, X_STATUS_SUCCESS, 0);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}
