#include "file.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace plat
{
    namespace
    {
        uint64_t FileTimeOf(const timespec& time)
        {
            constexpr uint64_t UNIX_EPOCH_AS_FILETIME = 116444736000000000ull;
            return UNIX_EPOCH_AS_FILETIME + uint64_t(time.tv_sec) * 10000000ull + uint64_t(time.tv_nsec) / 100;
        }

        void Fill(const struct stat& st, FileStat& stat)
        {
            stat.creation = FileTimeOf(st.st_ctim); // no birth time in struct stat; ctime is close enough
            stat.access = FileTimeOf(st.st_atim);
            stat.write = FileTimeOf(st.st_mtim);
            stat.size = uint64_t(st.st_size);
            stat.attributes = 0;
            if (S_ISDIR(st.st_mode))
                stat.attributes |= FILE_ATTR_DIRECTORY;
            if (!(st.st_mode & S_IWUSR))
                stat.attributes |= FILE_ATTR_READONLY;
        }

        // pread/pwrite, retried through a host buffer when the kernel can't copy into guest pages
        // that aren't committed yet (EFAULT; the memcpy then commits them on demand).
        bool Transfer(int fd, bool write, void* data, size_t length, uint64_t offset, size_t& transferred)
        {
            transferred = 0;
            auto once = [&](void* buffer) -> bool {
                while (transferred < length)
                {
                    ssize_t n = write ? pwrite(fd, static_cast<uint8_t*>(buffer) + transferred, length - transferred, off_t(offset + transferred))
                                      : pread(fd, static_cast<uint8_t*>(buffer) + transferred, length - transferred, off_t(offset + transferred));
                    if (n < 0)
                    {
                        if (errno == EINTR)
                            continue;
                        return false;
                    }
                    if (n == 0)
                        break; // end of file
                    transferred += size_t(n);
                }
                return true;
            };
            if (once(data))
                return true;
            if (errno != EFAULT)
                return false;

            std::vector<uint8_t> buffer(length);
            if (write)
                memcpy(buffer.data(), data, length);
            transferred = 0;
            if (!once(buffer.data()))
                return false;
            if (!write)
                memcpy(data, buffer.data(), transferred);
            return true;
        }
    }

    bool StatPath(const std::filesystem::path& path, FileStat& stat)
    {
        struct stat st;
        if (::stat(path.c_str(), &st) != 0)
            return false;
        Fill(st, stat);
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
        int flags = (write ? O_RDWR : O_RDONLY) | O_CLOEXEC;
        switch (disposition)
        {
        case FileDisposition::CreateAlways: flags |= O_CREAT | O_TRUNC; break;
        case FileDisposition::CreateNew: flags |= O_CREAT | O_EXCL; break;
        case FileDisposition::OpenAlways: flags |= O_CREAT; break;
        case FileDisposition::TruncateExisting: flags |= O_TRUNC; break;
        default: break;
        }
        int fd = ::open(path.c_str(), flags, 0644);
        if (fd < 0)
        {
            if (error)
                *error = errno == ENOENT || errno == ENOTDIR ? FileError::NotFound
                       : errno == EACCES || errno == EPERM || errno == EROFS ? FileError::AccessDenied
                       : errno == EEXIST ? FileError::Exists
                       : FileError::Other;
            return false;
        }
        m_handle = fd;
        if (error)
            *error = FileError::None;
        return true;
    }

    void File::Close()
    {
        if (m_handle != INVALID)
            ::close(int(m_handle));
        m_handle = INVALID;
    }

    bool File::ReadAt(uint64_t offset, void* data, size_t length, size_t& transferred) const
    {
        return Transfer(int(m_handle), false, data, length, offset, transferred);
    }

    bool File::WriteAt(uint64_t offset, const void* data, size_t length, size_t& transferred)
    {
        return Transfer(int(m_handle), true, const_cast<void*>(data), length, offset, transferred);
    }

    bool File::ReadExactAt(uint64_t offset, void* data, size_t length) const
    {
        size_t transferred;
        return ReadAt(offset, data, length, transferred) && transferred == length;
    }

    uint64_t File::Size() const
    {
        struct stat st;
        return fstat(int(m_handle), &st) == 0 ? uint64_t(st.st_size) : 0;
    }

    bool File::SetSize(uint64_t size)
    {
        return ftruncate(int(m_handle), off_t(size)) == 0;
    }

    bool File::Flush()
    {
        return fsync(int(m_handle)) == 0;
    }

    bool File::Stat(FileStat& stat) const
    {
        struct stat st;
        if (fstat(int(m_handle), &st) != 0)
            return false;
        Fill(st, stat);
        return true;
    }
}
