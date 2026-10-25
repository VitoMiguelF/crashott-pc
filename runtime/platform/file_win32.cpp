#include "file.h"

#include <windows.h>

#include <cstring>
#include <vector>

namespace plat
{
    namespace
    {
        uint64_t Value(const FILETIME& time)
        {
            return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        }

        HANDLE Native(intptr_t handle)
        {
            return reinterpret_cast<HANDLE>(handle);
        }

        constexpr uint32_t PASSED_ATTRIBUTES = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                               FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ARCHIVE;

        // ReadFile/WriteFile at an offset. Kernel-mode copies into guest pages that aren't
        // committed yet fail (ERROR_NOACCESS) instead of faulting, so those go through a host
        // buffer (whose memcpy commits on demand).
        bool Transfer(HANDLE file, bool write, void* data, size_t length, uint64_t offset, size_t& transferred)
        {
            OVERLAPPED overlapped{};
            overlapped.Offset = DWORD(offset);
            overlapped.OffsetHigh = DWORD(offset >> 32);
            DWORD done = 0;
            transferred = 0;

            BOOL ok = write ? WriteFile(file, data, DWORD(length), &done, &overlapped)
                            : ReadFile(file, data, DWORD(length), &done, &overlapped);
            transferred = done;
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
            ok = write ? WriteFile(file, buffer.data(), DWORD(length), &done, &overlapped)
                       : ReadFile(file, buffer.data(), DWORD(length), &done, &overlapped);
            transferred = done;
            if (!ok && GetLastError() != ERROR_HANDLE_EOF)
                return false;
            if (!write)
                memcpy(data, buffer.data(), done);
            return true;
        }
    }

    bool StatPath(const std::filesystem::path& path, FileStat& stat)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return false;
        stat.creation = Value(data.ftCreationTime);
        stat.access = Value(data.ftLastAccessTime);
        stat.write = Value(data.ftLastWriteTime);
        stat.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        stat.attributes = data.dwFileAttributes & PASSED_ATTRIBUTES;
        return true;
    }

    File& File::operator=(File&& other) noexcept
    {
        if (this != &other)
        {
            Close();
            m_handle = other.m_handle;
            other.m_handle = INVALID;
        }
        return *this;
    }

    bool File::Open(const std::filesystem::path& path, bool write, FileDisposition disposition, FileError* error)
    {
        Close();
        DWORD creation = OPEN_EXISTING;
        switch (disposition)
        {
        case FileDisposition::CreateAlways: creation = CREATE_ALWAYS; break;
        case FileDisposition::CreateNew: creation = CREATE_NEW; break;
        case FileDisposition::OpenAlways: creation = OPEN_ALWAYS; break;
        case FileDisposition::TruncateExisting: creation = TRUNCATE_EXISTING; break;
        default: break;
        }
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            if (error)
            {
                DWORD code = GetLastError();
                *error = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? FileError::NotFound
                       : code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION ? FileError::AccessDenied
                       : code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS ? FileError::Exists
                       : FileError::Other;
            }
            return false;
        }
        m_handle = reinterpret_cast<intptr_t>(handle);
        if (error)
            *error = FileError::None;
        return true;
    }

    void File::Close()
    {
        if (m_handle != INVALID)
            CloseHandle(Native(m_handle));
        m_handle = INVALID;
    }

    bool File::ReadAt(uint64_t offset, void* data, size_t length, size_t& transferred) const
    {
        return Transfer(Native(m_handle), false, data, length, offset, transferred);
    }

    bool File::WriteAt(uint64_t offset, const void* data, size_t length, size_t& transferred)
    {
        return Transfer(Native(m_handle), true, const_cast<void*>(data), length, offset, transferred);
    }

    bool File::ReadExactAt(uint64_t offset, void* data, size_t length) const
    {
        size_t transferred;
        return ReadAt(offset, data, length, transferred) && transferred == length;
    }

    uint64_t File::Size() const
    {
        LARGE_INTEGER size{};
        GetFileSizeEx(Native(m_handle), &size);
        return uint64_t(size.QuadPart);
    }

    bool File::SetSize(uint64_t size)
    {
        FILE_END_OF_FILE_INFO endOfFile{};
        endOfFile.EndOfFile.QuadPart = int64_t(size);
        return SetFileInformationByHandle(Native(m_handle), FileEndOfFileInfo, &endOfFile, sizeof(endOfFile)) != 0;
    }

    bool File::Flush()
    {
        return FlushFileBuffers(Native(m_handle)) != 0;
    }

    bool File::Stat(FileStat& stat) const
    {
        BY_HANDLE_FILE_INFORMATION data;
        if (!GetFileInformationByHandle(Native(m_handle), &data))
            return false;
        stat.creation = Value(data.ftCreationTime);
        stat.access = Value(data.ftLastAccessTime);
        stat.write = Value(data.ftLastWriteTime);
        stat.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        stat.attributes = data.dwFileAttributes & PASSED_ATTRIBUTES;
        return true;
    }
}
