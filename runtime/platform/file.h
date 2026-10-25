#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

// Host files for the kernel's Nt*File imports and the .rcf readers: positional reads and writes,
// size, NT-style attributes and FILETIME stamps. file_win32.cpp (CreateFileW) / file_posix.cpp
// (open/pread). Case-insensitive lookup of game paths is the VFS's job (fs/vfs.cpp), not this one.
namespace plat
{
    enum class FileDisposition { OpenExisting, CreateAlways, CreateNew, OpenAlways, TruncateExisting };
    enum class FileError { None, NotFound, AccessDenied, Exists, Other };

    // NT FILE_ATTRIBUTE_* bits, the same values the guest uses.
    constexpr uint32_t FILE_ATTR_READONLY = 0x01;
    constexpr uint32_t FILE_ATTR_HIDDEN = 0x02;
    constexpr uint32_t FILE_ATTR_SYSTEM = 0x04;
    constexpr uint32_t FILE_ATTR_DIRECTORY = 0x10;
    constexpr uint32_t FILE_ATTR_ARCHIVE = 0x20;

    struct FileStat
    {
        uint64_t creation{}, access{}, write{}; // FILETIME units (100 ns since 1601, UTC)
        uint64_t size{};
        uint32_t attributes{};                  // FILE_ATTR_*
    };

    bool StatPath(const std::filesystem::path& path, FileStat& stat);

    class File
    {
    public:
        File() = default;
        File(const File&) = delete;
        File& operator=(const File&) = delete;
        File(File&& other) noexcept : m_handle(other.m_handle) { other.m_handle = INVALID; }
        File& operator=(File&& other) noexcept;
        ~File() { Close(); }

        // Shared read/write/delete like the console's file system. `write` asks for write access.
        bool Open(const std::filesystem::path& path, bool write = false,
                  FileDisposition disposition = FileDisposition::OpenExisting, FileError* error = nullptr);
        bool IsOpen() const { return m_handle != INVALID; }
        void Close();

        // Positional transfers (the file has no shared position). Reading past the end succeeds
        // with fewer bytes. Buffers may be guest pages that aren't committed yet: kernel copies
        // into those fail instead of faulting, so they are retried through a host buffer.
        bool ReadAt(uint64_t offset, void* data, size_t length, size_t& transferred) const;
        bool WriteAt(uint64_t offset, const void* data, size_t length, size_t& transferred);
        // ReadAt that wants exactly `length` bytes.
        bool ReadExactAt(uint64_t offset, void* data, size_t length) const;

        uint64_t Size() const;
        bool SetSize(uint64_t size);
        bool Flush();
        bool Stat(FileStat& stat) const;

    private:
        static constexpr intptr_t INVALID = -1;
        intptr_t m_handle = INVALID; // HANDLE or file descriptor
    };
}
