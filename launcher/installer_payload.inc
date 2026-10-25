// The files the single-file installer carries after its own code (tools/make_installer.py writes
// them; the format is described there): the index, read at startup, and the extraction into the
// install folder on all cores. Included inside launcher.cpp's anonymous namespace (CRASH_INSTALLER).
//
// Decompression is LZMS through the Compression API (cabinet.dll, part of Windows since 8), loaded
// at run time: nothing to link or ship.

#ifdef _WIN32
struct PayloadFile
{
    std::string component;  // "base", "dlss"
    uint64_t offset = 0, size = 0;  // in the raw stream
    std::wstring path;      // relative, with backslashes
};

struct PayloadChunk
{
    uint64_t offset = 0, size = 0;        // compressed, from the payload's start
    uint64_t rawOffset = 0, rawSize = 0;  // in the raw stream
};

struct Payload
{
    bool present = false;
    std::string tag;
    uint64_t base = 0;  // file offset of the first chunk (= the stub's size)
    std::vector<PayloadChunk> chunks;
    std::vector<PayloadFile> files;

    uint64_t Bytes(const std::set<std::string>& skip) const
    {
        uint64_t total = 0;
        for (const PayloadFile& f : files)
            if (!skip.count(f.component))
                total += f.size;
        return total;
    }
};
Payload g_payload;

// Reads the index at the end of this exe; present = false when there is none (a CrashSetup.exe
// built by CMake, next to tools\setup.py in a clone).
Payload ReadPayload()
{
    Payload payload;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    FILE* f = _wfopen(self, L"rb");
    if (!f)
        return payload;
    char footer[32];
    std::string index;
    uint64_t indexOffset = 0, indexSize = 0, stubSize = 0;
    if (_fseeki64(f, -32, SEEK_END) == 0 && fread(footer, 1, 32, f) == 32 && memcmp(footer, "CRSHPAK1", 8) == 0)
    {
        memcpy(&indexOffset, footer + 8, 8);
        memcpy(&indexSize, footer + 16, 8);
        memcpy(&stubSize, footer + 24, 8);
        index.resize(size_t(indexSize));
        if (indexSize > (64u << 20) || _fseeki64(f, int64_t(stubSize + indexOffset), SEEK_SET) != 0 ||
            fread(index.data(), 1, index.size(), f) != index.size())
            index.clear();
    }
    fclose(f);
    if (index.rfind("CRASHRECOMP-PAYLOAD 1\n", 0) != 0)
        return payload;
    payload.base = stubSize;
    size_t at = 0;
    while (at < index.size())
    {
        size_t end = index.find('\n', at);
        if (end == std::string::npos)
            end = index.size();
        std::string line = index.substr(at, end - at);
        at = end + 1;
        if (line.rfind("tag ", 0) == 0)
            payload.tag = line.substr(4);
        else if (line.rfind("chunk ", 0) == 0)
        {
            PayloadChunk c;
            if (sscanf(line.c_str() + 6, "%llu %llu %llu %llu", reinterpret_cast<unsigned long long*>(&c.offset),
                       reinterpret_cast<unsigned long long*>(&c.size), reinterpret_cast<unsigned long long*>(&c.rawOffset),
                       reinterpret_cast<unsigned long long*>(&c.rawSize)) == 4)
                payload.chunks.push_back(c);
        }
        else if (line.rfind("file ", 0) == 0)
        {
            // "file <component> <offset> <size> <path>"
            size_t a = line.find(' ', 5), b = line.find(' ', a + 1), c = line.find(' ', b + 1);
            if (c == std::string::npos)
                continue;
            PayloadFile file;
            file.component = line.substr(5, a - 5);
            file.offset = strtoull(line.c_str() + a + 1, nullptr, 10);
            file.size = strtoull(line.c_str() + b + 1, nullptr, 10);
            file.path = Widen(line.substr(c + 1));
            std::replace(file.path.begin(), file.path.end(), L'/', L'\\');
            if (file.path.find(L"..") != std::wstring::npos || file.path.find(L':') != std::wstring::npos || file.path[0] == L'\\')
                continue;  // only paths inside the install folder
            payload.files.push_back(std::move(file));
        }
    }
    payload.present = !payload.chunks.empty() && !payload.files.empty();
    return payload;
}

// The Compression API, from cabinet.dll.
struct Lzms
{
    using CreateFn = BOOL(WINAPI*)(DWORD, void*, HANDLE*);
    using DecompressFn = BOOL(WINAPI*)(HANDLE, const void*, SIZE_T, void*, SIZE_T, SIZE_T*);
    using CloseFn = BOOL(WINAPI*)(HANDLE);
    CreateFn create = nullptr;
    DecompressFn decompress = nullptr;
    CloseFn close = nullptr;

    bool Load()
    {
        HMODULE cabinet = LoadLibraryW(L"cabinet.dll");
        if (!cabinet)
            return false;
        create = reinterpret_cast<CreateFn>(GetProcAddress(cabinet, "CreateDecompressor"));
        decompress = reinterpret_cast<DecompressFn>(GetProcAddress(cabinet, "Decompress"));
        close = reinterpret_cast<CloseFn>(GetProcAddress(cabinet, "CloseDecompressor"));
        return create && decompress && close;
    }
};

struct ExtractProgress
{
    std::atomic<uint64_t> done{ 0 }, total{ 0 };  // raw bytes
    std::atomic<bool> cancel{ false };
};

bool CreateDirectories(const fs::path& dir, std::string& error)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir))
    {
        error = Narrow(dir.wstring()) + ": " + ec.message();
        return false;
    }
    return true;
}

// Extracts every file whose component isn't in `skip` into `dest`, on several threads. Returns ""
// or what went wrong.
std::string ExtractPayload(const Payload& payload, const fs::path& dest, const std::set<std::string>& skip, ExtractProgress& progress)
{
    Lzms lzms;
    if (!lzms.Load())
        return "cabinet.dll (Compression API) is missing: Windows 8 or newer is needed";
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    // Every file is created at its size first; the threads then write their pieces into place.
    progress.total = payload.Bytes(skip);  // roughly, until the chunks are counted below
    std::string error;
    std::vector<const PayloadFile*> wanted;
    for (const PayloadFile& f : payload.files)
    {
        if (skip.count(f.component))
            continue;
        fs::path target = dest / f.path;
        if (!CreateDirectories(target.parent_path(), error))
            return error;
        SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);  // read-only files of an older install
        HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return Narrow(target.wstring()) + ": cannot write (error " + std::to_string(GetLastError()) + ")";
        LARGE_INTEGER size;
        size.QuadPart = int64_t(f.size);
        bool ok = SetFilePointerEx(h, size, nullptr, FILE_BEGIN) && SetEndOfFile(h);
        CloseHandle(h);
        if (!ok)
            return Narrow(target.wstring()) + ": not enough space?";
        wanted.push_back(&f);
    }
    // The chunks that hold a wanted file (files are in stream order).
    std::vector<size_t> todo;
    uint64_t total = 0;
    for (size_t c = 0; c < payload.chunks.size(); c++)
    {
        const PayloadChunk& chunk = payload.chunks[c];
        auto first = std::lower_bound(wanted.begin(), wanted.end(), chunk.rawOffset, [](const PayloadFile* f, uint64_t at) { return f->offset + f->size <= at; });
        if (first != wanted.end() && (*first)->offset < chunk.rawOffset + chunk.rawSize)
            todo.push_back(c), total += chunk.rawSize;
    }
    progress.total = total;
    std::atomic<size_t> next{ 0 };
    std::mutex errorMutex;
    auto fail = [&](const std::string& message) {
        std::lock_guard lock(errorMutex);
        if (error.empty())
            error = message;
        progress.cancel = true;
    };
    auto worker = [&] {
        HANDLE exe = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        HANDLE decompressor = nullptr;
        if (exe == INVALID_HANDLE_VALUE || !lzms.create(5 /* COMPRESS_ALGORITHM_LZMS */, nullptr, &decompressor))
        {
            fail("cannot read the installer's own files");
            if (exe != INVALID_HANDLE_VALUE)
                CloseHandle(exe);
            return;
        }
        std::vector<uint8_t> packed, raw;
        for (size_t i; !progress.cancel && (i = next++) < todo.size();)
        {
            const PayloadChunk& chunk = payload.chunks[todo[i]];
            packed.resize(size_t(chunk.size));
            raw.resize(size_t(chunk.rawSize));
            OVERLAPPED at{};
            uint64_t position = payload.base + chunk.offset;
            at.Offset = DWORD(position), at.OffsetHigh = DWORD(position >> 32);
            DWORD got = 0;
            SIZE_T out = 0;
            if (!ReadFile(exe, packed.data(), DWORD(packed.size()), &got, &at) || got != packed.size())
            {
                fail("the installer file is damaged or incomplete (download it again)");
                break;
            }
            if (!lzms.decompress(decompressor, packed.data(), packed.size(), raw.data(), raw.size(), &out) || out != raw.size())
            {
                fail("the installer file is damaged (download it again)");
                break;
            }
            // The pieces of the files in this chunk.
            uint64_t start = chunk.rawOffset, end = chunk.rawOffset + chunk.rawSize;
            auto first = std::lower_bound(wanted.begin(), wanted.end(), start, [](const PayloadFile* f, uint64_t pos) { return f->offset + f->size <= pos; });
            for (auto it = first; it != wanted.end() && (*it)->offset < end; ++it)
            {
                const PayloadFile& f = **it;
                uint64_t from = std::max(start, f.offset), to = std::min(end, f.offset + f.size);
                if (to <= from)
                    continue;
                fs::path target = dest / f.path;
                HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                OVERLAPPED where{};
                uint64_t inFile = from - f.offset;
                where.Offset = DWORD(inFile), where.OffsetHigh = DWORD(inFile >> 32);
                DWORD wrote = 0;
                bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, raw.data() + (from - start), DWORD(to - from), &wrote, &where) && wrote == to - from;
                if (h != INVALID_HANDLE_VALUE)
                    CloseHandle(h);
                if (!ok)
                {
                    fail(Narrow(target.wstring()) + ": write failed (error " + std::to_string(GetLastError()) + ")");
                    break;
                }
            }
            progress.done += chunk.rawSize;
        }
        lzms.close(decompressor);
        CloseHandle(exe);
    };
    unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++)
        pool.emplace_back(worker);
    for (std::thread& t : pool)
        t.join();
    if (error.empty() && progress.cancel)
        error = "cancelled";
    return error;
}

#else
// Linux: the files are the AppImage's payload/ folder (tools/make_appimage.py), read-only in its mount,
// with payload/.crashrecomp_payload listing them: "tag <tag>", then "file <component> <size> <path>"
// and "link <component> <path> <target>" (symbolic links, kept as links). The extraction is a copy.

struct PayloadFile
{
    std::string component;  // "base", "modding", "dlss"
    uint64_t offset = 0, size = 0;
    std::wstring path;      // relative, with slashes
    std::string link;       // a symbolic link's target ("" for a file)
};

struct Payload
{
    bool present = false;
    std::string tag;
    fs::path root;          // the payload folder
    std::vector<PayloadFile> files;

    uint64_t Bytes(const std::set<std::string>& skip) const
    {
        uint64_t total = 0;
        for (const PayloadFile& f : files)
            if (!skip.count(f.component))
                total += f.size;
        return total;
    }
};
Payload g_payload;

// The payload next to this exe in the AppImage ($APPDIR/payload, the exe being $APPDIR/usr/bin/CrashSetup).
Payload ReadPayload()
{
    Payload payload;
    std::vector<fs::path> roots;
    if (const char* appdir = getenv("APPDIR"); appdir && *appdir)
        roots.push_back(fs::path(appdir) / "payload");
    roots.push_back(plat::ExecutablePath().parent_path().parent_path().parent_path() / "payload");
    for (const fs::path& root : roots)
    {
        FILE* f = fopen((root / ".crashrecomp_payload").c_str(), "rb");
        if (!f)
            continue;
        char line[4096];
        while (fgets(line, sizeof(line), f))
        {
            line[strcspn(line, "\r\n")] = 0;
            std::string text = line;
            if (text.rfind("tag ", 0) == 0)
                payload.tag = text.substr(4);
            else if (text.rfind("file ", 0) == 0 || text.rfind("link ", 0) == 0)
            {
                bool link = text[0] == 'l';
                size_t a = text.find(' ', 5);
                if (a == std::string::npos)
                    continue;
                PayloadFile file;
                file.component = text.substr(5, a - 5);
                std::string rest = text.substr(a + 1);
                if (link)
                {
                    size_t tab = rest.find('\t');  // "<path>\t<target>": paths may have spaces
                    if (tab == std::string::npos)
                        continue;
                    file.path = Widen(rest.substr(0, tab));
                    file.link = rest.substr(tab + 1);
                }
                else
                {
                    size_t b = rest.find(' ');
                    if (b == std::string::npos)
                        continue;
                    file.size = strtoull(rest.c_str(), nullptr, 10);
                    file.path = Widen(rest.substr(b + 1));
                }
                payload.files.push_back(std::move(file));
            }
        }
        fclose(f);
        payload.present = !payload.files.empty();
        payload.root = root;
        break;
    }
    return payload;
}

struct ExtractProgress
{
    std::atomic<uint64_t> done{ 0 }, total{ 0 };  // bytes
    std::atomic<bool> cancel{ false };
};

bool CreateDirectories(const fs::path& dir, std::string& error)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir))
    {
        error = dir.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// Copies every file whose component isn't in `skip` into `dest` (a file already there with the same
// size is kept: a reinstall over an install), on a few threads. Returns "" or what went wrong.
std::string ExtractPayload(const Payload& payload, const fs::path& dest, const std::set<std::string>& skip, ExtractProgress& progress)
{
    std::vector<const PayloadFile*> files;
    for (const PayloadFile& f : payload.files)
        if (!skip.count(f.component))
        {
            files.push_back(&f);
            progress.total += f.size;
        }
    std::atomic<size_t> next{ 0 };
    std::mutex errorMutex;
    std::string firstError;
    auto work = [&] {
        std::vector<char> buffer(1 << 20);
        for (;;)
        {
            size_t i = next++;
            if (i >= files.size() || progress.cancel)
                return;
            const PayloadFile& file = *files[i];
            fs::path target = dest / file.path;
            std::string error;
            if (!CreateDirectories(target.parent_path(), error))
            {
                std::lock_guard lock(errorMutex);
                if (firstError.empty())
                    firstError = error;
                progress.cancel = true;
                return;
            }
            std::error_code ec;
            if (!file.link.empty())
            {
                fs::remove(target, ec);
                fs::create_symlink(file.link, target, ec);
                continue;
            }
            fs::path source = payload.root / file.path;
            if (fs::is_regular_file(target, ec) && fs::file_size(target, ec) == file.size)
            {
                progress.done += file.size;
                continue;
            }
            FILE* in = fopen(source.c_str(), "rb");
            FILE* out = in ? fopen(target.c_str(), "wb") : nullptr;
            bool ok = in && out;
            size_t read;
            while (ok && (read = fread(buffer.data(), 1, buffer.size(), in)) > 0)
            {
                ok = fwrite(buffer.data(), 1, read, out) == read;
                progress.done += read;
            }
            if (in)
                fclose(in);
            if (out)
                ok = fclose(out) == 0 && ok;
            if (ok)
                fs::permissions(target, fs::status(source, ec).permissions(), ec);  // the executables stay executable
            else
            {
                std::lock_guard lock(errorMutex);
                if (firstError.empty())
                    firstError = target.string() + ": " + strerror(errno);
                progress.cancel = true;
                return;
            }
        }
    };
    unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++)
        pool.emplace_back(work);
    for (std::thread& t : pool)
        t.join();
    if (!firstError.empty())
        return firstError;
    return progress.cancel ? "cancelled" : "";
}
#endif
