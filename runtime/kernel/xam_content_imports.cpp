// XAM system UI and content (save game) imports.
//
// There is no system UI: the device selector picks the one hard drive at once, and message boxes
// answer with their default button. Both send XN_SYS_UI on/off, as the dashboard overlay does,
// and complete their XOVERLAPPED immediately (Xenia's headless mode does the same).
//
// Content packages (XamContentCreateEx & co.) are folders on the host:
//
//   userdata\content\<XUID>\<title id>\<content type>\<file name>\       the package's files
//   userdata\content\<XUID>\<title id>\<content type>\<file name>.header  its XCONTENT_DATA
//   userdata\content\<XUID>\<title id>\<content type>\<file name>.thumbnail (if set)
//
// with XUID = the local profile's (user 0) for saved games (type 1) and 0000000000000000 for the
// rest, title id and content type in hex. While a package is open its root name ("save:\...")
// is mounted in the VFS on its folder, so the title's NtCreateFile/NtWriteFile land there.
// Layouts and error codes follow Xenia's xam_content.cc / xam_ui.cc / xam_enum.cc.

#include "kernel.h"
#include "objects.h"
#include "xam_content.h"
#include "xex_loader.h"
#include "../fs/vfs.h"
#include "../platform/platform.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t X_ERROR_SUCCESS = 0;
    constexpr uint32_t X_ERROR_PATH_NOT_FOUND = 3;
    constexpr uint32_t X_ERROR_NO_MORE_FILES = 18;
    constexpr uint32_t X_ERROR_INVALID_PARAMETER = 0x57;
    constexpr uint32_t X_ERROR_ALREADY_EXISTS = 183;
    constexpr uint32_t X_ERROR_IO_PENDING = 0x3E5;
    constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 0x48F;

    constexpr uint32_t XN_SYS_UI = 0x00000009;

    // The one storage device: a 20 GB hard drive (Xenia's dummy HDD).
    constexpr uint32_t DEVICE_ID = 1;
    constexpr uint32_t DEVICE_TYPE_HDD = 1;
    constexpr uint64_t DEVICE_TOTAL_BYTES = 20ull * 1024 * 1024 * 1024;
    constexpr uint64_t DEVICE_FREE_BYTES = 16ull * 1024 * 1024 * 1024;
    constexpr const char16_t* DEVICE_NAME = u"Hard Drive";

    constexpr uint64_t LOCAL_XUID = 0xB13EBABEBABEBABEull; // user 0 (Xenia's default profile)
    constexpr uint32_t CONTENT_TYPE_SAVED_GAME = 1;

    // XCONTENT_DATA: device id, content type, display name (UTF-16 BE, 128), file name (ASCII, 42), pad.
    constexpr uint32_t XCONTENT_DATA_SIZE = 0x134;
    constexpr uint32_t XCONTENT_DISPLAY_NAME = 0x08;
    constexpr uint32_t XCONTENT_FILE_NAME = 0x108;
    constexpr uint32_t XCONTENT_FILE_NAME_LENGTH = 42;

    std::mutex g_mutex;
    std::map<std::string, fs::path> g_openPackages; // lowercase root name -> package folder

    std::string ReadWide(uint8_t* base, uint32_t guest, size_t maxChars = 512)
    {
        std::string text;
        for (size_t i = 0; guest && i < maxChars; i++)
        {
            uint16_t c = PPC_LOAD_U16(guest + i * 2);
            if (!c)
                break;
            text += c < 0x80 ? char(c) : '?';
        }
        return text;
    }

    std::string ReadAscii(uint8_t* base, uint32_t guest, size_t maxChars)
    {
        std::string text;
        for (size_t i = 0; guest && i < maxChars; i++)
        {
            char c = char(PPC_LOAD_U8(guest + i));
            if (!c)
                break;
            text += c;
        }
        return text;
    }

    uint32_t Finish(uint8_t* base, uint32_t overlapped, uint32_t result)
    {
        if (!overlapped)
            return result;
        CompleteXOverlapped(base, overlapped, result);
        return X_ERROR_IO_PENDING;
    }

    // The packages of each content type and the saved games looked up by the save screens, read
    // again only after a change made here (create, close, delete): the game enumerates its saves
    // every frame of its menus, and on Android each listing goes through the slow shared storage
    // (FUSE).
    std::mutex g_listsMutex;
    std::map<uint32_t, std::vector<std::vector<uint8_t>>> g_lists;      // content type -> XCONTENT_DATA items
    std::map<std::string, std::optional<std::string>> g_savedGames;    // file name -> display name, or none

    void ContentChanged()
    {
        std::lock_guard lock(g_listsMutex);
        g_lists.clear();
        g_savedGames.clear();
    }

    fs::path TypeFolder(uint32_t contentType)
    {
        char xuid[17], title[9], type[9];
        snprintf(xuid, sizeof(xuid), "%016llX", (unsigned long long)(contentType == CONTENT_TYPE_SAVED_GAME ? LOCAL_XUID : 0));
        snprintf(title, sizeof(title), "%08X", CurrentTitleId());
        snprintf(type, sizeof(type), "%08X", contentType);
        return vfs::UserDataDir() / "content" / xuid / title / type;
    }

    // Host-safe package folder name from the XCONTENT_DATA file name.
    std::string SafeFileName(const std::string& name)
    {
        std::string safe;
        for (char c : name)
            safe += (uint8_t(c) < 0x20 || strchr("<>:\"/\\|?*", c)) ? '_' : c;
        while (!safe.empty() && (safe.back() == '.' || safe.back() == ' '))
            safe.pop_back();
        return safe.empty() ? "_" : safe;
    }

    struct Package
    {
        uint32_t contentType;
        std::string fileName;
        fs::path folder;
        fs::path header;
        fs::path thumbnail;
    };

    std::optional<Package> PackageFromContentData(uint8_t* base, uint32_t contentData)
    {
        if (!contentData)
            return std::nullopt;
        Package package;
        package.contentType = PPC_LOAD_U32(contentData + 4);
        package.fileName = ReadAscii(base, contentData + XCONTENT_FILE_NAME, XCONTENT_FILE_NAME_LENGTH);
        if (package.fileName.empty())
            return std::nullopt;
        std::string safe = SafeFileName(package.fileName);
        fs::path type = TypeFolder(package.contentType);
        package.folder = type / safe;
        package.header = type / (safe + ".header");
        package.thumbnail = type / (safe + ".thumbnail");
        return package;
    }

    void WriteHeader(uint8_t* base, const Package& package, uint32_t contentData)
    {
        std::ofstream out(package.header, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(base + contentData), XCONTENT_DATA_SIZE);
    }

    std::string Describe(uint8_t* base, uint32_t contentData)
    {
        if (!contentData)
            return "(null)";
        return "type " + std::to_string(PPC_LOAD_U32(contentData + 4)) + " \"" +
               ReadAscii(base, contentData + XCONTENT_FILE_NAME, XCONTENT_FILE_NAME_LENGTH) + "\" (\"" +
               ReadWide(base, contentData + XCONTENT_DISPLAY_NAME, 128) + "\")";
    }

    struct EnumeratorObject : KernelObject
    {
        std::vector<std::vector<uint8_t>> items; // XCONTENT_DATA, guest byte order
        size_t next = 0;
        uint32_t itemsPerEnumerate = 1;
        bool IsSignaled(uint32_t) const override { return true; }
    };

    void UiOpen()
    {
        BroadcastXNotification(XN_SYS_UI, 1);
    }

    void UiClose()
    {
        BroadcastXNotification(XN_SYS_UI, 0);
    }
}

// u32 XamShowDeviceSelectorUI(u32 UserIndex, u32 ContentType, u32 ContentFlags, u64 TotalRequested,
//                             u32* DeviceId, XOVERLAPPED* Overlapped)
PPC_FUNC(__imp__XamShowDeviceSelectorUI)
{
    KERNEL_IMPORT_ENTER(XamShowDeviceSelectorUI);
    uint32_t deviceIdPtr = ctx.r7.u32, overlapped = ctx.r8.u32;
    UiOpen();
    if (deviceIdPtr)
        PPC_STORE_U32(deviceIdPtr, DEVICE_ID);
    UiClose();
    uint32_t result = Finish(base, overlapped, X_ERROR_SUCCESS);
    Log("[xam] XamShowDeviceSelectorUI(user %u, content type %u, flags 0x%X, %llu bytes requested, overlapped 0x%08X) lr=0x%08X -> device %u (\"Hard Drive\")",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (unsigned long long)ctx.r6.u64, overlapped, GuestLR(ctx), DEVICE_ID);
    ctx.r3.u64 = result;
}

// u32 XamShowMessageBoxUIEx(u32 UserIndex, wchar* Title, wchar* Text, u32 ButtonCount, wchar** Buttons,
//                           u32 FocusButton, u32 Flags, u32* Result, XOVERLAPPED* Overlapped (stack))
PPC_FUNC(__imp__XamShowMessageBoxUIEx)
{
    KERNEL_IMPORT_ENTER(XamShowMessageBoxUIEx);
    uint32_t buttonCount = ctx.r6.u32, buttons = ctx.r7.u32, focus = ctx.r8.u32, resultPtr = ctx.r10.u32;
    uint32_t overlapped = GuestStackArgument(ctx, base, 8);

    std::string buttonList;
    for (uint32_t i = 0; buttons && i < buttonCount && i < 8; i++)
        buttonList += (i ? ", " : "") + std::string(i == focus ? "*" : "") + ReadWide(base, PPC_LOAD_U32(buttons + i * 4), 64);
    Log("[xam] message box \"%s\": \"%s\" [%s] -> button %u", ReadWide(base, ctx.r4.u32).c_str(),
        ReadWide(base, ctx.r5.u32).c_str(), buttonList.c_str(), focus);

    UiOpen();
    if (resultPtr)
        PPC_STORE_U32(resultPtr, focus);
    UiClose();
    ctx.r3.u64 = Finish(base, overlapped, X_ERROR_SUCCESS);
}

// u32 XamContentGetDeviceState(u32 UserIndex, u32 DeviceId, XOVERLAPPED* Overlapped)
PPC_FUNC(__imp__XamContentGetDeviceState)
{
    KERNEL_IMPORT_ENTER(XamContentGetDeviceState);
    uint32_t deviceId = ctx.r4.u32;
    uint32_t result = deviceId == DEVICE_ID ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
    KERNEL_LOG("[xam] XamContentGetDeviceState(user %u, device %u) lr=0x%08X -> 0x%X", ctx.r3.u32, deviceId, GuestLR(ctx), result);
    ctx.r3.u64 = Finish(base, ctx.r5.u32, result);
}

// u32 XamContentGetDeviceData(u32 DeviceId, XCONTENT_DEVICE_DATA* Data): id, type, total and free
// bytes, name (UTF-16, 28); 0x50 bytes.
PPC_FUNC(__imp__XamContentGetDeviceData)
{
    KERNEL_IMPORT_ENTER(XamContentGetDeviceData);
    uint32_t deviceId = ctx.r3.u32, data = ctx.r4.u32;
    uint32_t result = X_ERROR_DEVICE_NOT_CONNECTED;
    if (deviceId == DEVICE_ID && data)
    {
        memset(base + data, 0, 0x50);
        PPC_STORE_U32(data + 0x00, DEVICE_ID);
        PPC_STORE_U32(data + 0x04, DEVICE_TYPE_HDD);
        PPC_STORE_U64(data + 0x08, DEVICE_TOTAL_BYTES);
        PPC_STORE_U64(data + 0x10, DEVICE_FREE_BYTES);
        for (uint32_t i = 0; DEVICE_NAME[i] && i < 27; i++)
            PPC_STORE_U16(data + 0x18 + i * 2, uint16_t(DEVICE_NAME[i]));
        result = X_ERROR_SUCCESS;
    }
    KERNEL_LOG("[xam] XamContentGetDeviceData(device %u, 0x%08X) lr=0x%08X -> 0x%X", deviceId, data, GuestLR(ctx), result);
    ctx.r3.u64 = result;
}

// u32 XamContentCreateEx(u32 UserIndex, char* RootName, XCONTENT_DATA* Content, u32 Flags,
//                        u32* Disposition, u32* LicenseMask, u32 CacheSize, u64 ContentSize,
//                        XOVERLAPPED* Overlapped (stack))
PPC_FUNC(__imp__XamContentCreateEx)
{
    KERNEL_IMPORT_ENTER(XamContentCreateEx);
    std::string root = ReadAscii(base, ctx.r4.u32, 32);
    uint32_t contentData = ctx.r5.u32, flags = ctx.r6.u32, dispositionPtr = ctx.r7.u32, licensePtr = ctx.r8.u32;
    uint32_t overlapped = GuestStackArgument(ctx, base, 8);

    // Disposition (flags & 0xF): 1 create new, 2 create always, 3 open existing, 4 open always,
    // 5 truncate existing. Out: 1 = created, 2 = opened.
    uint32_t result = X_ERROR_SUCCESS, disposition = 0;
    auto package = PackageFromContentData(base, contentData);
    std::error_code error;
    if (!package || root.empty())
    {
        result = X_ERROR_INVALID_PARAMETER;
    }
    else
    {
        bool exists = fs::is_directory(package->folder, error);
        switch (flags & 0xF)
        {
        case 1:
            if (exists)
                result = X_ERROR_ALREADY_EXISTS;
            else
                disposition = 1;
            break;
        case 2:
            if (exists)
                fs::remove_all(package->folder, error);
            disposition = 1;
            break;
        case 3:
            if (!exists)
                result = X_ERROR_PATH_NOT_FOUND;
            else
                disposition = 2;
            break;
        case 4:
            disposition = exists ? 2 : 1;
            break;
        case 5:
            if (!exists)
                result = X_ERROR_PATH_NOT_FOUND;
            else
            {
                fs::remove_all(package->folder, error);
                disposition = 2;
            }
            break;
        default:
            result = X_ERROR_INVALID_PARAMETER;
            break;
        }

        if (result == X_ERROR_SUCCESS)
        {
            fs::create_directories(package->folder, error);
            if (disposition == 1 || !fs::exists(package->header, error))
                WriteHeader(base, *package, contentData);
            if (!vfs::MountContent(root, package->folder))
                result = X_ERROR_INVALID_PARAMETER;
            else
            {
                std::lock_guard lock(g_mutex);
                g_openPackages[root] = package->folder;
            }
        }
    }

    if (result == X_ERROR_SUCCESS)
    {
        if (dispositionPtr)
            PPC_STORE_U32(dispositionPtr, disposition);
        if (licensePtr)
            PPC_STORE_U32(licensePtr, 0);
    }
    Log("[xam] XamContentCreateEx(user %u, \"%s:\", %s, flags 0x%X, overlapped 0x%08X) lr=0x%08X -> 0x%X%s%s", ctx.r3.u32,
        root.c_str(), Describe(base, contentData).c_str(), flags, overlapped, GuestLR(ctx), result,
        disposition == 1 ? " (created)" : disposition == 2 ? " (opened)" : "",
        package && result == X_ERROR_SUCCESS ? (" at " + package->folder.string()).c_str() : "");
    ContentChanged();
    ctx.r3.u64 = Finish(base, overlapped, result);
}

// u32 XamContentClose(char* RootName, XOVERLAPPED* Overlapped)
PPC_FUNC(__imp__XamContentClose)
{
    KERNEL_IMPORT_ENTER(XamContentClose);
    std::string root = ReadAscii(base, ctx.r3.u32, 32);
    uint32_t result = X_ERROR_SUCCESS;
    {
        std::lock_guard lock(g_mutex);
        if (!g_openPackages.erase(root))
            result = X_ERROR_PATH_NOT_FOUND;
    }
    if (result == X_ERROR_SUCCESS)
        vfs::UnmountContent(root);
    Log("[xam] XamContentClose(\"%s:\") lr=0x%08X -> 0x%X", root.c_str(), GuestLR(ctx), result);
    ContentChanged();
    ctx.r3.u64 = Finish(base, ctx.r4.u32, result);
}

// u32 XamContentFlush(char* RootName, XOVERLAPPED* Overlapped): writes are already on disk.
PPC_FUNC(__imp__XamContentFlush)
{
    KERNEL_IMPORT_ENTER(XamContentFlush);
    KERNEL_LOG("[xam] XamContentFlush(\"%s:\") lr=0x%08X", ReadAscii(base, ctx.r3.u32, 32).c_str(), GuestLR(ctx));
    ctx.r3.u64 = Finish(base, ctx.r4.u32, X_ERROR_SUCCESS);
}

// u32 XamContentDelete(u32 UserIndex, XCONTENT_DATA* Content, XOVERLAPPED* Overlapped)
PPC_FUNC(__imp__XamContentDelete)
{
    KERNEL_IMPORT_ENTER(XamContentDelete);
    auto package = PackageFromContentData(base, ctx.r4.u32);
    uint32_t result = X_ERROR_INVALID_PARAMETER;
    std::error_code error;
    if (package)
    {
        result = fs::is_directory(package->folder, error) ? X_ERROR_SUCCESS : X_ERROR_PATH_NOT_FOUND;
        fs::remove_all(package->folder, error);
        fs::remove(package->header, error);
        fs::remove(package->thumbnail, error);
    }
    ContentChanged();
    Log("[xam] XamContentDelete(user %u, %s) lr=0x%08X -> 0x%X", ctx.r3.u32, Describe(base, ctx.r4.u32).c_str(), GuestLR(ctx), result);
    ctx.r3.u64 = Finish(base, ctx.r5.u32, result);
}

// u32 XamContentGetCreator(u32 UserIndex, XCONTENT_DATA* Content, u32* IsCreator, u64* CreatorXuid,
//                          XOVERLAPPED* Overlapped): the local profile created every package.
PPC_FUNC(__imp__XamContentGetCreator)
{
    KERNEL_IMPORT_ENTER(XamContentGetCreator);
    auto package = PackageFromContentData(base, ctx.r4.u32);
    std::error_code error;
    uint32_t result = package && fs::is_directory(package->folder, error) ? X_ERROR_SUCCESS : X_ERROR_PATH_NOT_FOUND;
    if (result == X_ERROR_SUCCESS)
    {
        if (ctx.r5.u32)
            PPC_STORE_U32(ctx.r5.u32, 1);
        if (ctx.r6.u32)
            PPC_STORE_U64(ctx.r6.u32, LOCAL_XUID);
    }
    KERNEL_LOG("[xam] XamContentGetCreator(user %u, %s) lr=0x%08X -> 0x%X", ctx.r3.u32, Describe(base, ctx.r4.u32).c_str(), GuestLR(ctx), result);
    ctx.r3.u64 = Finish(base, ctx.r7.u32, result);
}

// u32 XamContentSetThumbnail(u32 UserIndex, XCONTENT_DATA* Content, void* Image, u32 Size,
//                            XOVERLAPPED* Overlapped): stored next to the package (PNG data).
PPC_FUNC(__imp__XamContentSetThumbnail)
{
    KERNEL_IMPORT_ENTER(XamContentSetThumbnail);
    auto package = PackageFromContentData(base, ctx.r4.u32);
    uint32_t image = ctx.r5.u32, size = ctx.r6.u32;
    std::error_code error;
    uint32_t result = package && fs::is_directory(package->folder, error) ? X_ERROR_SUCCESS : X_ERROR_PATH_NOT_FOUND;
    if (result == X_ERROR_SUCCESS && image && size)
    {
        std::ofstream out(package->thumbnail, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(base + image), size);
    }
    KERNEL_LOG("[xam] XamContentSetThumbnail(user %u, %s, %u bytes) lr=0x%08X -> 0x%X", ctx.r3.u32,
               Describe(base, ctx.r4.u32).c_str(), size, GuestLR(ctx), result);
    ctx.r3.u64 = Finish(base, ctx.r7.u32, result);
}

// u32 XamContentCreateEnumerator(u32 UserIndex, u32 DeviceId, u32 ContentType, u32 ContentFlags,
//                                u32 ItemsPerEnumerate, u32* BufferSize, HANDLE* Handle)
PPC_FUNC(__imp__XamContentCreateEnumerator)
{
    KERNEL_IMPORT_ENTER(XamContentCreateEnumerator);
    uint32_t userIndex = ctx.r3.u32, deviceId = ctx.r4.u32, contentType = ctx.r5.u32, itemsPerEnumerate = ctx.r7.u32;
    uint32_t bufferSizePtr = ctx.r8.u32, handlePtr = ctx.r9.u32;
    if (!handlePtr || (deviceId && deviceId != DEVICE_ID))
    {
        uint32_t result = !handlePtr ? X_ERROR_INVALID_PARAMETER : X_ERROR_DEVICE_NOT_CONNECTED;
        KERNEL_LOG("[xam] XamContentCreateEnumerator(user %u, device %u, type %u) lr=0x%08X -> 0x%X", ctx.r3.u32, deviceId,
                   contentType, GuestLR(ctx), result);
        ctx.r3.u64 = result;
        return;
    }

    auto* enumerator = new EnumeratorObject();
    enumerator->type = OBJECT_ENUMERATOR;
    enumerator->itemsPerEnumerate = itemsPerEnumerate ? itemsPerEnumerate : 1;

    // Packages of this type: every folder, with the XCONTENT_DATA from its header (or one made
    // up from the folder name if the header is missing); the last listing while nothing changed.
    fs::path type = TypeFolder(contentType);
    bool listed = false;
    {
        std::lock_guard lock(g_listsMutex);
        if (auto cached = g_lists.find(contentType); cached != g_lists.end())
            enumerator->items = cached->second, listed = true;
    }
    uint64_t started = plat::TickCountMs();
    std::error_code error;
    if (!listed && fs::is_directory(type, error))
    {
        for (auto& entry : fs::directory_iterator(type, error))
        {
            if (!entry.is_directory(error))
                continue;
            std::vector<uint8_t> item(XCONTENT_DATA_SIZE, 0);
            std::ifstream in(type / (entry.path().filename().string() + ".header"), std::ios::binary);
            if (!in || !in.read(reinterpret_cast<char*>(item.data()), XCONTENT_DATA_SIZE))
            {
                std::fill(item.begin(), item.end(), 0);
                std::string name = entry.path().filename().string().substr(0, XCONTENT_FILE_NAME_LENGTH);
                memcpy(item.data() + XCONTENT_FILE_NAME, name.data(), name.size());
                for (size_t i = 0; i < name.size() && i < 127; i++)
                    item[XCONTENT_DISPLAY_NAME + i * 2 + 1] = uint8_t(name[i]);
            }
            uint32_t device = __builtin_bswap32(DEVICE_ID), typeBE = __builtin_bswap32(contentType);
            memcpy(item.data() + 0, &device, 4);
            memcpy(item.data() + 4, &typeBE, 4);
            enumerator->items.push_back(std::move(item));
        }
    }

    if (!listed)
    {
        std::lock_guard lock(g_listsMutex);
        g_lists[contentType] = enumerator->items;
    }

    RegisterObject(enumerator);
    uint32_t handle = CreateHandle(enumerator);
    PPC_STORE_U32(handlePtr, handle);
    if (bufferSizePtr)
        PPC_STORE_U32(bufferSizePtr, enumerator->itemsPerEnumerate * XCONTENT_DATA_SIZE);
    ctx.r3.u64 = X_ERROR_SUCCESS;
    if (listed)
        return;  // logged when it was read
    Log("[xam] XamContentCreateEnumerator(user %u, device %u, type %u, %u per call) lr=0x%08X -> handle 0x%08X, %zu package(s) in %s "
        "(read in %llu ms; again only after a change)",
        userIndex, deviceId, contentType, itemsPerEnumerate, GuestLR(ctx), handle, enumerator->items.size(), type.string().c_str(),
        (unsigned long long)(plat::TickCountMs() - started));
}

// u32 XamEnumerate(HANDLE Enumerator, u32 Flags, void* Buffer, u32 BufferSize, u32* ItemsReturned,
//                  XOVERLAPPED* Overlapped): the next batch; ERROR_NO_MORE_FILES at the end. With an
// overlapped, the item count goes in its length field.
PPC_FUNC(__imp__XamEnumerate)
{
    KERNEL_IMPORT_ENTER(XamEnumerate);
    uint32_t handle = ctx.r3.u32, buffer = ctx.r5.u32, bufferSize = ctx.r6.u32, itemsPtr = ctx.r7.u32, overlapped = ctx.r8.u32;
    KernelObject* object = ObjectFromHandle(handle);
    auto* enumerator = object && object->type == OBJECT_ENUMERATOR ? static_cast<EnumeratorObject*>(object) : nullptr;

    uint32_t result, count = 0;
    if (!enumerator || !buffer)
    {
        result = X_ERROR_INVALID_PARAMETER;
    }
    else
    {
        uint32_t capacity = std::min(bufferSize / XCONTENT_DATA_SIZE, enumerator->itemsPerEnumerate);
        while (count < capacity && enumerator->next < enumerator->items.size())
        {
            memcpy(base + buffer + count * XCONTENT_DATA_SIZE, enumerator->items[enumerator->next].data(), XCONTENT_DATA_SIZE);
            enumerator->next++;
            count++;
        }
        result = count ? X_ERROR_SUCCESS : X_ERROR_NO_MORE_FILES;
    }

    KERNEL_LOG("[xam] XamEnumerate(0x%08X, buffer 0x%08X size %u, overlapped 0x%08X) lr=0x%08X -> 0x%X, %u item(s)", handle,
               buffer, bufferSize, overlapped, GuestLR(ctx), result, count);
    if (itemsPtr)
        PPC_STORE_U32(itemsPtr, result == X_ERROR_SUCCESS ? count : 0);
    if (overlapped)
    {
        CompleteXOverlappedEx(base, overlapped, result, result == X_ERROR_SUCCESS ? count : 0);
        ctx.r3.u64 = X_ERROR_IO_PENDING;
    }
    else
    {
        ctx.r3.u64 = result;
    }
}

namespace xam::content
{
    bool SavedGameExists(const std::string& fileName, std::string* displayName)
    {
        // Asked every frame of the slot screen: cached until a change (ContentChanged).
        {
            std::lock_guard lock(g_listsMutex);
            if (auto cached = g_savedGames.find(fileName); cached != g_savedGames.end())
            {
                if (cached->second && displayName)
                    *displayName = *cached->second;
                return cached->second.has_value();
            }
        }
        std::string safe = SafeFileName(fileName);
        fs::path type = TypeFolder(CONTENT_TYPE_SAVED_GAME);
        std::error_code error;
        std::optional<std::string> found;
        if (fs::is_directory(type / safe, error))
        {
            // XCONTENT_DATA as the game passed it: the display name is UTF-16 big-endian.
            found.emplace();
            std::ifstream in(type / (safe + ".header"), std::ios::binary);
            std::vector<char> header(XCONTENT_DATA_SIZE);
            if (in.read(header.data(), header.size()))
            {
                for (uint32_t i = XCONTENT_DISPLAY_NAME; i + 1 < XCONTENT_FILE_NAME; i += 2)
                {
                    uint16_t c = uint16_t(uint8_t(header[i]) << 8 | uint8_t(header[i + 1]));
                    if (!c)
                        break;
                    *found += c < 0x80 ? char(c) : '?';
                }
            }
        }
        {
            std::lock_guard lock(g_listsMutex);
            g_savedGames[fileName] = found;
        }
        if (found && displayName)
            *displayName = *found;
        return found.has_value();
    }

    bool DeleteSavedGame(const std::string& fileName)
    {
        std::string safe = SafeFileName(fileName);
        fs::path type = TypeFolder(CONTENT_TYPE_SAVED_GAME);
        std::error_code error;
        fs::remove_all(type / safe, error);
        fs::remove(type / (safe + ".header"), error);
        fs::remove(type / (safe + ".thumbnail"), error);
        bool gone = !fs::exists(type / safe, error);
        ContentChanged();
        Log("[xam] saved game \"%s\" deleted by the runtime -> %s", fileName.c_str(), gone ? "gone" : "still there");
        return gone;
    }
}
