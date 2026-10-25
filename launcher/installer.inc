// CrashSetup: the installer screen, compiled into launcher.cpp's anonymous namespace when
// CRASH_INSTALLER is defined (CMake target CrashSetup), so it has the launcher's look: the game's
// cloth, logo, Titans fonts, message frame and stars, read from the chosen dump's default.rcf.
//
// It is a front end for tools\setup.py, run with the release's own Python (toolchain\python\python.exe).
// Two ways to run:
//   - the single-file installer (release CrashSetup-<tag>.exe, tools/make_installer.py): everything
//     the install needs is inside the exe (installer_payload.inc). The player chooses the install
//     folder (default %LOCALAPPDATA%\Programs\Crash of the Titans Definitive Edition, any drive; the
//     space needed and free is shown) and the game folder; INSTALL extracts the project into its
//     CrashRecomp\ subfolder (EXTRACTING FILES), then runs setup.py from there with --play-dir, offline
//     (only the optional subtitles / HD AI Remaster are downloaded). The install folder ends up with
//     only what the player uses (tools/make_dist.py --installed): CrashLauncher.exe, CrashOTTDE.exe,
//     the DLLs, game\ (the dump, always copied), userdata\, mods\, setup.log and .crashrecomp_files;
//     languages\, data\, licencas\ and the build are in CrashRecomp\. Only what building and running
//     needs is in the exe (no docs, tests or development tools); the asset export's tools and
//     numpy / Pillow (component "modding") are extracted only with ASSETS FOR MODDING.
//     The DLSS files (CrashDlss.dll, nvngx_dlss.dll) are installed only on a PC where NGX says DLSS
//     runs (gpu_probe.inc: an NVIDIA RTX GPU, checked at startup with those two files unpacked to
//     %TEMP%); elsewhere setup.py gets --no-dlss and the options offer FSR or nothing. At the end
//     it asks for a desktop shortcut and a Start menu entry to the launcher (CrashLauncher.exe);
//   - next to tools\setup.py (a clone, or build\CrashSetup.exe copied to the project root): the
//     same screen without the install folder; it checks the prerequisites (setup.py --check; BUILD
//     TOOLS = VISUAL STUDIO (advanced) builds with an installed Visual Studio + LLVM clang-cl
//     instead, setup.py --toolchain msvc, installing the missing ones with winget; a clone without
//     toolchain\ gets it with tools\toolchain.py fetch).
// Both: options (subtitles, HD AI Remaster, asset export for modding, copy the dump into
// dist\game); setup.py's steps ("@@step"), progress ("@@progress", "@@download") and log; at the end
// PLAY opens dist\CrashLauncher.exe. When only the asset export failed, the game is playable and
// TRY AGAIN runs just that step (setup.py --assets-only). Release zips placed next to CrashSetup.exe
// are used instead of downloading (setup.py --assets-dir).
//
// Tests and scripts: --install-dir <folder>, --autostart (install with the options given),
// --option subtitles|no-subtitles|remaster|assets|copy|desktop|no-desktop|startmenu|no-startmenu (copy:
// always in the single-file installer),
// --autofinish (at the end: make the chosen shortcuts and exit, code 0, or 1 if it failed),
// --shots <folder> (a PNG of every screen and step, 01_setup.png...), CRASH_GPU_VENDOR (gpu_vendor.h).
// Included inside launcher.cpp's anonymous namespace: its headers (<mutex>, <thread>) are at the top there.

std::wstring Widen(const std::string& text)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), n);
    return out;
}

#include "installer_payload.inc"

struct InstallStep
{
    Text name;
};
const InstallStep EXTRACT_STEP = { { "EXTRACTING FILES", "EXTRAINDO ARQUIVOS" } };
const InstallStep INSTALL_STEPS[] = {
    { { "PREREQUISITES", "PRÉ-REQUISITOS" } }, { { "GAME", "JOGO" } }, { { "XENONRECOMP", "XENONRECOMP" } },
    { { "LIBRARIES", "BIBLIOTECAS" } }, { { "RECOMPILING THE GAME CODE", "RECOMPILANDO O CÓDIGO DO JOGO" } },
    { { "BUILDING", "COMPILANDO" } }, { { "EXTRAS", "EXTRAS" } }, { { "PLAY FOLDER", "PASTA DO JOGO" } },
    { { "ASSETS FOR MODDING", "ASSETS PARA MODDING" } },
};
constexpr int INSTALL_STEP_COUNT = int(sizeof(INSTALL_STEPS) / sizeof(INSTALL_STEPS[0]));

// Disk space, beyond the extracted files: the build (objects, the exes), generated\ppc, dist\
// with its DLL copies and shader cache, and each option.
constexpr uint64_t GB = 1ull << 30, MB = 1ull << 20;
constexpr uint64_t BUILD_BYTES = 900 * MB, SUBTITLES_BYTES = 10 * MB, REMASTER_BYTES = 9500 * MB, ASSETS_BYTES = 18 * GB;

struct Prereq
{
    std::string name, path;
    bool ok = false;
};

// winget packages for each prerequisite setup.py checks with --toolchain msvc (its names).
struct WingetPackage
{
    const char* prereq;
    const wchar_t* command;
};
const WingetPackage WINGET[] = {
    { "Python", L"winget install --id Python.Python.3.13 -e --accept-source-agreements --accept-package-agreements" },
    { "Git", L"winget install --id Git.Git -e --accept-source-agreements --accept-package-agreements" },
    { "CMake", L"winget install --id Kitware.CMake -e --accept-source-agreements --accept-package-agreements" },
    { "Ninja", L"winget install --id Ninja-build.Ninja -e --accept-source-agreements --accept-package-agreements" },
    { "clang-cl", L"winget install --id LLVM.LLVM -e --accept-source-agreements --accept-package-agreements" },
    { "Visual Studio", L"winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-source-agreements --accept-package-agreements "
                       L"--override \"--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\"" },
};

enum class Phase { Setup, Running, Done, Failed };

struct Installer
{
    Phase phase = Phase::Setup;
    std::wstring python;  // command prefix: toolchain\python\python.exe, else "py -3" or a python.exe path
    std::string toolchain = "bundled";  // setup.py --toolchain: "bundled" (toolchain\) or "msvc"
    bool toolchainChosen = false;       // the user changed BUILD TOOLS (else setup.py's "@@toolchain" sets it)
    bool checking = false, checked = false;
    std::vector<Prereq> prereqs;
    std::vector<Prereq> packages;  // tools\requirements.txt ("@@pypkg"): never block the install
    bool subtitles = true, remaster = false, extractAssets = false, copyGame = false;
    int selected = 0;
    bool mouseMode = false;
    bool autoStart = false;   // --autostart: install as soon as everything is known
    bool autoFinish = false;  // --autofinish: at the end, shortcuts and exit

    // The single-file installer (g_payload.present).
    bool bundle = false;
    fs::path installDir;
    bool desktopShortcut = true, startMenu = true, shortcutsDone = false;
    GpuInfo gpu;
    bool gpuKnown = false;          // DLSS decided (no NVIDIA GPU, or NGX's check finished)
    bool dlss = false;              // install the DLSS files
    fs::path probeDir;              // %TEMP% copy of the DLSS files for the check
    uint64_t gameBytes = 0;         // the dump's size (COPY THE GAME)
    bool extracting = false;
    ExtractProgress extract;
    std::thread extractor;
    std::string extractError;
    bool extractFinished = false;
    std::string pendingArgs;        // setup.py's arguments once the files are extracted (UTF-8)
    bool setupPending = false;      // the files are there: start setup.py
    fs::path shots;                 // --shots
    fs::path pendingShot;           // saved by the main loop after this frame is drawn
    std::string lastShot;
    int shotNumber = 0, shotWait = 0;

    // The running setup.py.
    std::mutex mutex;
    std::vector<std::string> log;  // plain lines (not "@@")
    int step = 0;                  // 1-based, 0 = not started
    std::string stepDetail;
    float progress = -1.0f;        // of the current step, -1 = unknown
    std::string download;          // "name 12/80 MB"
    std::string error, hint, finishedDir;
    bool gameReady = false;        // "@@game-ready": dist\ can be played even if a later step fails
    bool processDone = false;
    DWORD exitCode = 0;
    HANDLE job = nullptr, process = nullptr;
    std::thread reader;
    FILE* logFile = nullptr;
    int finalCode = 0;             // --autofinish exit code
};
Installer g_inst;

// Where setup.py and the toolchain are: the install folder's CrashRecomp\, or next to this exe.
const wchar_t* PROJECT_FOLDER = L"CrashRecomp";
fs::path Root() { return g_inst.bundle ? g_inst.installDir / PROJECT_FOLDER : paths::ExeDir(); }
// Where the game is played from (the launcher, userdata\): the install folder, or dist\.
fs::path PlayDir() { return g_inst.bundle ? g_inst.installDir : paths::ExeDir() / L"dist"; }
fs::path SetupLog() { return (g_inst.bundle ? g_inst.installDir : paths::ExeDir()) / L"setup.log"; }
fs::path SetupScript() { return Root() / "tools" / "setup.py"; }
fs::path BundledPython() { return Root() / "toolchain" / "python" / "python.exe"; }
bool UsesBundledToolchain() { return g_inst.toolchain != "msvc"; }
fs::path Requirements() { return Root() / "tools" / "requirements.txt"; }

std::wstring ToolchainArg() { return g_inst.toolchainChosen ? L" --toolchain " + Widen(g_inst.toolchain) : L""; }

// --- install folder, space, shortcuts --------------------------------------------------------

const wchar_t* REGISTRY_KEY = L"Software\\CrashRecomp";

fs::path KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path)))
        out = path;
    CoTaskMemFree(path);
    return out;
}

const wchar_t* INSTALL_FOLDER_NAME = L"Crash of the Titans Definitive Edition";

// The last install folder (HKCU\Software\CrashRecomp InstallDir), else
// %LOCALAPPDATA%\Programs\Crash of the Titans Definitive Edition.
fs::path DefaultInstallDir()
{
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, REGISTRY_KEY, L"InstallDir", RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS && buffer[0])
        return fs::path(buffer);
    fs::path local = KnownFolder(FOLDERID_LocalAppData);
    return (local.empty() ? fs::path(L"C:\\") : local / L"Programs") / INSTALL_FOLDER_NAME;
}

void RememberInstallDir()
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    std::wstring dir = g_inst.installDir.wstring();
    RegSetValueExW(key, L"InstallDir", 0, REG_SZ, reinterpret_cast<const BYTE*>(dir.c_str()), DWORD((dir.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

// Free bytes on the drive of `dir` (its nearest existing parent).
uint64_t FreeBytes(fs::path dir)
{
    std::error_code error;
    while (!dir.empty() && !fs::exists(dir, error) && dir != dir.parent_path())
        dir = dir.parent_path();
    ULARGE_INTEGER free{};
    return GetDiskFreeSpaceExW(dir.c_str(), &free, nullptr, nullptr) ? free.QuadPart : 0;
}

uint64_t FolderBytes(const fs::path& dir)
{
    uint64_t total = 0;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(dir, error); !error && it != fs::recursive_directory_iterator(); it.increment(error))
        if (it->is_regular_file(error))
            total += it->file_size(error);
    return total;
}

// The parts of the payload this install leaves out: DLSS without an NVIDIA RTX GPU, the asset
// export's tools without ASSETS FOR MODDING.
std::set<std::string> SkippedComponents()
{
    std::set<std::string> skip;
    if (!g_inst.dlss)
        skip.insert("dlss");
    if (!g_inst.extractAssets)
        skip.insert("modding");
    return skip;
}

// What the chosen options need on the install drive.
uint64_t RequiredBytes()
{
    uint64_t bytes = g_payload.Bytes(SkippedComponents()) + BUILD_BYTES;
    if (g_inst.subtitles) bytes += SUBTITLES_BYTES;
    if (g_inst.remaster) bytes += REMASTER_BYTES;
    if (g_inst.extractAssets) bytes += ASSETS_BYTES;
    if (g_inst.copyGame || g_inst.bundle) bytes += g_inst.gameBytes;
    return bytes;
}

std::string SizeText(uint64_t bytes)
{
    char text[32];
    if (bytes >= GB)
        snprintf(text, sizeof(text), "%.1f GB", double(bytes) / double(GB));
    else
        snprintf(text, sizeof(text), "%.0f MB", double(bytes) / double(MB));
    std::string s = text;
    if (Pt())
        std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

bool EnoughSpace() { return FreeBytes(g_inst.installDir) >= RequiredBytes(); }

// The folder can be created and written to (Program Files needs an administrator: not used).
bool Writable(const fs::path& dir, std::string& why)
{
    std::error_code error;
    fs::create_directories(dir, error);
    if (!fs::is_directory(dir, error))
    {
        why = T({ "Cannot create this folder.", "Não dá para criar esta pasta." });
        return false;
    }
    fs::path probe = dir / L".crashrecomp_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        why = T({ "No permission to write here: choose a folder of your user (not Program Files).",
                  "Sem permissão para gravar aqui: escolha uma pasta do seu usuário (não Arquivos de Programas)." });
        return false;
    }
    CloseHandle(h);
    return true;
}

void ChooseInstallDir()
{
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(Pt() ? L"Onde instalar (a pasta \"Crash of the Titans Definitive Edition\" é criada dentro dela)"
                              : L"Where to install (a \"Crash of the Titans Definitive Edition\" folder is made inside it)");
        IShellItem* start = nullptr;
        fs::path parent = g_inst.installDir.parent_path();
        if (SUCCEEDED(SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_PPV_ARGS(&start))))
            dialog->SetFolder(start), start->Release();
        if (dialog->Show(g_hwnd) == S_OK)
        {
            IShellItem* item = nullptr;
            PWSTR name = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)))
            {
                fs::path chosen(name);
                // A folder with that name (or an install already) is used as it is; else one is made inside.
                std::error_code error;
                bool install = fs::exists(chosen / PROJECT_FOLDER / L"tools" / L"setup.py", error) ||
                               _wcsicmp(chosen.filename().c_str(), INSTALL_FOLDER_NAME) == 0;
                g_inst.installDir = install ? chosen : chosen / INSTALL_FOLDER_NAME;
                CoTaskMemFree(name);
                std::string why;
                if (!Writable(g_inst.installDir, why))
                {
                    g_message[0] = why;
                    g_message[1] = Narrow(g_inst.installDir.wstring());
                    g_modal = Modal::Message;
                }
            }
            if (item)
                item->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
}

// A .lnk at `link` to `target`, with its icon and its folder as the working folder.
bool CreateShortcut(const fs::path& link, const fs::path& target, const std::wstring& description)
{
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IShellLinkW* shell = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell))))
    {
        shell->SetPath(target.c_str());
        shell->SetWorkingDirectory(target.parent_path().c_str());
        shell->SetIconLocation(target.c_str(), 0);
        shell->SetDescription(description.c_str());
        IPersistFile* file = nullptr;
        if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file))))
        {
            std::error_code error;
            fs::create_directories(link.parent_path(), error);
            ok = SUCCEEDED(file->Save(link.c_str(), TRUE));
            file->Release();
        }
        shell->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    Log("[setup] shortcut %s: %s", Narrow(link.wstring()).c_str(), ok ? "ok" : "failed");
    return ok;
}

const wchar_t* SHORTCUT_NAME = L"Crash of the Titans - Definitive Edition.lnk";

// The shortcuts chosen at the end (once, when leaving the last screen).
void ApplyShortcuts()
{
    if (!g_inst.bundle || g_inst.shortcutsDone || g_inst.phase != Phase::Done)
        return;
    g_inst.shortcutsDone = true;
    fs::path launcher = PlayDir() / L"CrashLauncher.exe";
    std::wstring description = L"CrashRecomp: Crash of the Titans (launcher)";
    if (g_inst.desktopShortcut)
        CreateShortcut(KnownFolder(FOLDERID_Desktop) / SHORTCUT_NAME, launcher, description);
    if (g_inst.startMenu)
        CreateShortcut(KnownFolder(FOLDERID_Programs) / L"CrashRecomp" / SHORTCUT_NAME, launcher, description);
}

// The files of an older install that the new one doesn't have any more (a source file moved:
// CMake would still compile the old copy). Only files the installer itself put there are listed
// (in the install folder, relative to CrashRecomp\).
const wchar_t* MANIFEST = L".crashrecomp_files";

void RemoveStaleFiles(const std::set<std::string>& skip)
{
    fs::path manifest = PlayDir() / MANIFEST;
    std::set<std::wstring> now;
    for (const PayloadFile& f : g_payload.files)
        if (!skip.count(f.component))
            now.insert(f.path);
    if (FILE* in = _wfopen(manifest.c_str(), L"rb"))
    {
        char line[2048];
        int removed = 0;
        while (fgets(line, sizeof(line), in))
        {
            line[strcspn(line, "\r\n")] = 0;
            std::wstring path = Widen(line);
            if (path.empty() || path.find(L"..") != std::wstring::npos || now.count(path))
                continue;
            std::error_code error;
            if (fs::remove(Root() / path, error))
                removed++;
        }
        fclose(in);
        if (removed)
            Log("[setup] %d file(s) of the previous version removed", removed);
    }
    if (FILE* out = _wfopen(manifest.c_str(), L"wb"))
    {
        for (const std::wstring& path : now)
            fprintf(out, "%s\n", Narrow(path).c_str());
        fclose(out);
    }
}

// --- the GPU (bundle): which upscaler files to install ----------------------------------------

// NVIDIA: the DLSS files are unpacked to %TEMP% and NGX is asked there (gpu_probe.inc).
void StartGpuCheck()
{
    g_inst.gpu = DetectGpu();
    Log("[setup] GPU %s (%s%s)", g_inst.gpu.name.empty() ? "?" : g_inst.gpu.name.c_str(), settings::GpuVendorName(g_inst.gpu.vendor),
        g_inst.gpu.forced ? ", CRASH_GPU_VENDOR" : "");
    bool haveDlss = std::any_of(g_payload.files.begin(), g_payload.files.end(), [](const PayloadFile& f) { return f.component == "dlss"; });
    if (g_inst.gpu.vendor != settings::VENDOR_NVIDIA || !haveDlss)
    {
        g_inst.dlss = false, g_inst.gpuKnown = true;
        return;
    }
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    g_inst.probeDir = fs::path(temp) / (L"CrashSetup_dlss_" + std::to_wstring(GetCurrentProcessId()));
    std::thread([] {
        ExtractProgress progress;
        std::string error = ExtractPayload(g_payload, g_inst.probeDir, { "base", "modding" }, progress);
        if (!error.empty())
        {
            Log("[setup] DLSS check: %s", error.c_str());
            g_dlssProbe.reason = error;
            g_dlssProbe.state = 2;
            return;
        }
        StartDlssProbe(g_inst.probeDir / L"thirdparty" / L"dlss" / L"bin");
    }).detach();
}

void UpdateGpuCheck()
{
    if (g_inst.gpuKnown || g_dlssProbe.state != 2)
        return;
    JoinDlssProbe();
    g_inst.dlss = g_dlssProbe.available;
    g_inst.gpuKnown = true;
    if (!g_dlssProbe.gpu.empty())
        g_inst.gpu.name = g_dlssProbe.gpu;
    std::error_code error;
    fs::remove_all(g_inst.probeDir, error);
}

// --- prerequisites and setup.py --------------------------------------------------------------

// PATH as Windows has it now (machine + user), so tools installed while the installer is open are
// found without restarting it.
void RefreshPath()
{
    auto read = [](HKEY root, const wchar_t* key) {
        wchar_t buffer[32768];
        DWORD size = sizeof(buffer);
        if (RegGetValueW(root, key, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buffer, &size) != ERROR_SUCCESS)
            return std::wstring();
        wchar_t expanded[32768];
        DWORD n = ExpandEnvironmentStringsW(buffer, expanded, 32768);
        return n ? std::wstring(expanded) : std::wstring(buffer);
    };
    std::wstring path = read(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
    std::wstring user = read(HKEY_CURRENT_USER, L"Environment");
    if (!user.empty())
        path += L";" + user;
    if (!path.empty())
        SetEnvironmentVariableW(L"PATH", path.c_str());
}

// Runs a command hidden and returns its exit code (-1 if it could not start).
int RunHidden(std::wstring command, DWORD timeoutMs = 60000)
{
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, Root().c_str(), &si, &pi))
        return -1;
    DWORD code = DWORD(-1);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

// Python 3.11+ (setup.py needs tomllib): the release's own (toolchain\python), else an activated
// virtual environment (CrashSetup started from its prompt), else the py launcher, else python.exe on
// PATH (not the Microsoft Store alias, which only opens the Store).
std::wstring FindPython()
{
    if (Exists(BundledPython()))
        return L"\"" + BundledPython().wstring() + L"\"";
    const wchar_t* test = L" -c \"import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)\"";
    wchar_t found[MAX_PATH];
    if (GetEnvironmentVariableW(L"VIRTUAL_ENV", found, MAX_PATH))
    {
        fs::path venv = fs::path(found) / L"Scripts" / L"python.exe";
        if (Exists(venv) && RunHidden(L"\"" + venv.wstring() + L"\"" + test) == 0)
            return L"\"" + venv.wstring() + L"\"";
    }
    if (SearchPathW(nullptr, L"py.exe", nullptr, MAX_PATH, found, nullptr) && RunHidden(L"\"" + std::wstring(found) + L"\" -3" + test) == 0)
        return L"\"" + std::wstring(found) + L"\" -3";
    if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, found, nullptr) && !wcsstr(found, L"\\WindowsApps\\") &&
        RunHidden(L"\"" + std::wstring(found) + L"\"" + test) == 0)
        return L"\"" + std::wstring(found) + L"\"";
    return L"";
}

void AddLogLine(const std::string& line)
{
    g_inst.log.push_back(line);
    if (g_inst.log.size() > 400)
        g_inst.log.erase(g_inst.log.begin(), g_inst.log.begin() + 100);
}

// One line of setup.py's output (with the mutex held).
void HandleLine(const std::string& line)
{
    if (g_inst.logFile)
        fprintf(g_inst.logFile, "%s\n", line.c_str()), fflush(g_inst.logFile);
    if (line.rfind("@@", 0) != 0)
    {
        AddLogLine(line);
        return;
    }
    if (line.rfind("@@step ", 0) == 0)
    {
        g_inst.step = atoi(line.c_str() + 7);
        size_t bar = line.find(" | ");
        g_inst.stepDetail = bar == std::string::npos ? "" : line.substr(bar + 3);
        g_inst.progress = -1.0f, g_inst.download.clear();
    }
    else if (line.rfind("@@progress ", 0) == 0)
        g_inst.progress = float(atof(line.c_str() + 11)), g_inst.download.clear();
    else if (line.rfind("@@download ", 0) == 0)
    {
        // "@@download <name> <done> <total>"
        size_t b = line.rfind(' '), a = line.rfind(' ', b - 1);
        double done = atof(line.substr(a + 1, b - a - 1).c_str()), total = atof(line.c_str() + b + 1);
        if (total > 0)
        {
            g_inst.progress = float(done / total);
            char text[160];
            snprintf(text, sizeof(text), "%s  %.0f / %.0f MB", line.substr(11, a - 11).c_str(), done / 1048576.0, total / 1048576.0);
            g_inst.download = text;
        }
    }
    else if (line.rfind("@@prereq ", 0) == 0 || line.rfind("@@pypkg ", 0) == 0)
    {
        // "@@prereq ok|missing <name> | <path>", "@@pypkg ok|missing <name>>=<min> | <version>"
        bool package = line[2] == 'p' && line[3] == 'y';
        size_t at = line.find(' ') + 1;
        Prereq p;
        p.ok = line.compare(at, 2, "ok") == 0;
        size_t start = line.find(' ', at) + 1, bar = line.find(" | ");
        p.name = line.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        p.path = bar == std::string::npos ? "" : line.substr(bar + 3);
        (package ? g_inst.packages : g_inst.prereqs).push_back(p);
    }
    else if (line.rfind("@@toolchain ", 0) == 0)
    {
        if (!g_inst.toolchainChosen)
            g_inst.toolchain = line.substr(12);
    }
    else if (line.rfind("@@error ", 0) == 0)
        g_inst.error = line.substr(8);
    else if (line.rfind("@@hint ", 0) == 0)
        g_inst.hint = line.substr(7);
    else if (line.rfind("@@game-ready ", 0) == 0)
        g_inst.gameReady = true, g_inst.finishedDir = line.substr(13);
    else if (line.rfind("@@finished ", 0) == 0)
        g_inst.finishedDir = line.substr(11);
}

// Starts `python tools\setup.py <args>` with its output read on a thread. A job object holds it and
// everything it starts (cmake, ninja, clang...) so CANCEL stops them all.
bool StartSetup(const std::wstring& args, bool check)
{
    if (g_inst.reader.joinable())
        g_inst.reader.join();
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readPipe, writePipe;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = g_inst.python + L" -u \"" + SetupScript().wstring() + L"\" " + args;
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             Root().c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (!ok)
    {
        CloseHandle(readPipe);
        return false;
    }
    if (!g_inst.job)
    {
        g_inst.job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_inst.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    AssignProcessToJobObject(g_inst.job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard lock(g_inst.mutex);
        g_inst.process = pi.hProcess;
        g_inst.processDone = false;
        g_inst.error.clear();
        g_inst.hint.clear();
        if (check)
            g_inst.prereqs.clear(), g_inst.packages.clear();
        else
        {
            if (!g_inst.bundle)
                g_inst.log.clear();
            g_inst.step = 0, g_inst.progress = -1.0f;
            g_inst.download.clear();
            if (!g_inst.logFile)
                g_inst.logFile = _wfopen(SetupLog().c_str(), L"w");
        }
    }
    g_inst.reader = std::thread([readPipe, process = pi.hProcess] {
        std::string pending;
        char buffer[4096];
        DWORD read;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read)
        {
            pending.append(buffer, read);
            size_t eol;
            while ((eol = pending.find('\n')) != std::string::npos)
            {
                std::string line = pending.substr(0, eol);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                pending.erase(0, eol + 1);
                std::lock_guard lock(g_inst.mutex);
                HandleLine(line);
            }
        }
        CloseHandle(readPipe);
        WaitForSingleObject(process, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        std::lock_guard lock(g_inst.mutex);
        if (!pending.empty())
            HandleLine(pending);
        g_inst.exitCode = code;
        g_inst.processDone = true;
    });
    return true;
}

void CheckPrerequisites()
{
    if (g_inst.bundle)
    {
        // Everything is inside this exe: setup.py checks it again after the extraction (step 1).
        std::lock_guard lock(g_inst.mutex);
        g_inst.prereqs.clear();
        g_inst.checked = true;
        return;
    }
    RefreshPath();
    g_inst.python = FindPython();
    g_inst.checked = false;
    if (g_inst.python.empty())
    {
        std::lock_guard lock(g_inst.mutex);
        g_inst.prereqs = { { "Python 3.11+", "", false } };
        g_inst.checked = true;
        return;
    }
    if (!Exists(SetupScript()))
    {
        std::lock_guard lock(g_inst.mutex);
        g_inst.prereqs = { { "tools\\setup.py", "", false } };
        g_inst.checked = true;
        return;
    }
    g_inst.checking = StartSetup(L"--check" + ToolchainArg(), true);
}

int MissingCount()
{
    int n = 0;
    for (const Prereq& p : g_inst.prereqs)
        n += !p.ok;
    return n;
}

// The missing Python packages ("numpy, Pillow"), empty when all are there.
std::string MissingPackages()
{
    std::string names;
    for (const Prereq& p : g_inst.packages)
        if (!p.ok)
            names += (names.empty() ? "" : ", ") + p.name.substr(0, p.name.find(">="));
    return names;
}

// pip with the same Python setup.py runs with.
std::wstring PipCommand() { return g_inst.python + L" -m pip install -r \"" + Requirements().wstring() + L"\""; }

// Opens a console that gets what is missing (the user sees and answers the prompts); CHECK AGAIN
// afterwards. The bundled toolchain: tools\toolchain.py fetch (only a clone lacks it). Visual
// Studio: winget. Python packages (only another Python than the bundled one lacks them): pip.
void InstallMissing()
{
    std::wstring commands;
    for (const Prereq& p : g_inst.prereqs)
    {
        if (p.ok)
            continue;
        if (p.name.rfind("Toolchain", 0) == 0)
        {
            if (!g_inst.python.empty() && commands.find(L"toolchain.py") == std::wstring::npos)
                commands += (commands.empty() ? L"" : L" & ") + g_inst.python + L" \"" +
                            (Root() / "tools" / "toolchain.py").wstring() + L"\" fetch";
            continue;
        }
        for (const WingetPackage& w : WINGET)
            if (p.name.find(w.prereq) != std::string::npos)
                commands += (commands.empty() ? L"" : L" & ") + std::wstring(w.command);
    }
    if (!g_inst.python.empty() && !MissingPackages().empty())
        commands += (commands.empty() ? L"" : L" & ") + PipCommand();
    if (commands.empty())
        return;
    std::wstring command = L"cmd.exe /k \"" + commands + L" & echo. & echo " +
                           (Pt() ? std::wstring(L"Pronto: feche esta janela e clique em VERIFICAR DE NOVO.") : std::wstring(L"Done: close this window and click CHECK AGAIN.")) + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// Bundle: the files go to the install folder on a thread, then setup.py runs from there.
void StartExtraction(const std::wstring& args)
{
    std::string why;
    if (!Writable(g_inst.installDir, why))
    {
        g_message[0] = why, g_message[1] = Narrow(g_inst.installDir.wstring());
        g_modal = Modal::Message;
        return;
    }
    if (!EnoughSpace())
    {
        g_message[0] = T({ "Not enough free space on this drive:", "Falta espaço livre neste disco:" });
        g_message[1] = std::string(T({ "needs ", "precisa de " })) + SizeText(RequiredBytes()) + T({ ", free ", ", livre " }) + SizeText(FreeBytes(g_inst.installDir));
        g_modal = Modal::Message;
        return;
    }
    if (g_inst.extractor.joinable())
        g_inst.extractor.join();
    {
        std::lock_guard lock(g_inst.mutex);
        g_inst.log.clear();
        g_inst.error.clear(), g_inst.hint.clear();
        g_inst.step = 0, g_inst.progress = 0.0f;
        g_inst.download.clear();
        g_inst.stepDetail.clear();
        AddLogLine(std::string(T({ "Installing to ", "Instalando em " })) + Narrow(g_inst.installDir.wstring()));
        AddLogLine(g_inst.dlss ? T({ "NVIDIA RTX GPU: DLSS included.", "Placa NVIDIA RTX: DLSS incluído." })
                               : T({ "No NVIDIA RTX GPU: DLSS left out (FSR or no upscaler).", "Sem placa NVIDIA RTX: DLSS fora (FSR ou sem upscaler)." }));
        g_inst.extracting = true, g_inst.extractFinished = false;
    }
    g_inst.pendingArgs = Narrow(args);
    g_inst.extract.done = 0, g_inst.extract.total = 0, g_inst.extract.cancel = false;
    g_inst.phase = Phase::Running;
    g_inst.extractor = std::thread([] {
        std::set<std::string> skip = SkippedComponents();
        LARGE_INTEGER start, end, frequency;
        QueryPerformanceCounter(&start);
        std::string error = ExtractPayload(g_payload, Root(), skip, g_inst.extract);
        if (error.empty())
        {
            RemoveStaleFiles(skip);
            if (!g_inst.dlss)
            {
                // An older install's DLSS files go too (a GPU swap): the game would offer DLSS otherwise.
                std::error_code ec;
                for (const wchar_t* name : { L"thirdparty\\dlss\\bin\\CrashDlss.dll", L"thirdparty\\dlss\\bin\\nvngx_dlss.dll", L"build\\CrashDlss.dll",
                                             L"build\\nvngx_dlss.dll" })
                    fs::remove(Root() / name, ec);
                for (const wchar_t* name : { L"CrashDlss.dll", L"nvngx_dlss.dll" })
                    fs::remove(PlayDir() / name, ec);
            }
        }
        QueryPerformanceCounter(&end);
        QueryPerformanceFrequency(&frequency);
        Log("[setup] extraction to %s: %s (%.1f s)", Narrow(g_inst.installDir.wstring()).c_str(), error.empty() ? "ok" : error.c_str(),
            double(end.QuadPart - start.QuadPart) / double(frequency.QuadPart));
        std::lock_guard lock(g_inst.mutex);
        g_inst.extractError = error;
        g_inst.extractFinished = true;
    });
}

// assetsOnly: TRY AGAIN after only the asset export failed (the game is already installed).
void StartInstall(bool assetsOnly = false)
{
    fs::path game = GameFolder();
    std::wstring args = L"--game \"" + game.wstring() + L"\"";
    if (assetsOnly)
        args += L" --assets-only";
    else
    {
        if (g_inst.subtitles) args += L" --subtitles";
        if (g_inst.remaster) args += L" --remaster";
        if (g_inst.extractAssets) args += L" --extract-assets";
        if (g_inst.copyGame || g_inst.bundle) args += L" --copy-game";  // the single-file installer always copies the dump
        if (g_inst.bundle) args += L" --play-dir \"" + PlayDir().wstring() + L"\"";
        if (g_inst.bundle && !g_inst.dlss) args += L" --no-dlss";
        args += L" --assets-dir \"" + paths::ExeDir().wstring() + L"\"";
        args += ToolchainArg();
        g_inst.gameReady = false;
    }
    if (g_inst.bundle && !assetsOnly)
    {
        StartExtraction(args);
        return;
    }
    if (StartSetup(args, false))
        g_inst.phase = Phase::Running;
    else
    {
        g_message[0] = T({ "Could not start Python.", "Não foi possível abrir o Python." });
        g_message[1] = "";
        g_modal = Modal::Message;
    }
}

void CancelInstall()
{
    g_inst.extract.cancel = true;
    if (g_inst.job)
        TerminateJobObject(g_inst.job, 1);
}

void OpenLauncher()
{
    ApplyShortcuts();
    fs::path dir = g_inst.finishedDir.empty() ? PlayDir() : fs::u8path(g_inst.finishedDir);
    fs::path exe = dir / "CrashLauncher.exe";
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        g_quit = true;
    }
}

// What a restart of this exe (Relaunch: the art is read from the game folder at startup) keeps.
std::wstring InstallerRelaunchArgs()
{
    std::wstring args;
    if (g_inst.bundle)
        args += L" --install-dir \"" + g_inst.installDir.wstring() + L"\"";
    args += g_inst.subtitles ? L" --option subtitles" : L" --option no-subtitles";
    if (g_inst.remaster) args += L" --option remaster";
    if (g_inst.extractAssets) args += L" --option assets";
    if (g_inst.copyGame) args += L" --option copy";
    if (!g_inst.shots.empty())
        args += L" --shots \"" + g_inst.shots.wstring() + L"\"";
    return args;
}

void ChooseInstallerGameFolder()
{
    fs::path folder = paths::PickGameFolder(g_hwnd);
    if (folder.empty())
        return;
    // Start again with --game: the art (logo, fonts, frame) is read from that folder at startup.
    g_passArgs = { "--game", Narrow(folder.wstring()) };
    Relaunch();
}

// ---------------------------------------------------------------------------------------------
// Drawing

struct InstallRow
{
    Text label;
    std::string value;
    std::string help[2];
    bool header = false, enabled = true, plainHelp = false;
    int id = 0;
};

enum RowId
{
    RowGame = 1, RowGameStatus, RowPrereqs, RowSubtitles, RowRemaster, RowAssets, RowCopy, RowToolchain,
    RowInstallDir, RowGpu, RowSpace, RowDesktop, RowStartMenu, RowInstalledIn
};

InstallRow GpuRow()
{
    const GpuInfo& gpu = g_inst.gpu;
    std::string name = gpu.name.empty() ? std::string(settings::GpuVendorName(gpu.vendor)) : gpu.name;
    if (gpu.forced)
        name += std::string(" (CRASH_GPU_VENDOR = ") + settings::GpuVendorName(gpu.vendor) + ")";
    std::string value, help1;
    if (!g_inst.gpuKnown)
        value = T({ "CHECKING...", "VERIFICANDO..." }),
        help1 = T({ "Asking NVIDIA's driver whether DLSS runs here.", "Perguntando ao driver da NVIDIA se o DLSS roda aqui." });
    else if (g_inst.dlss)
        value = "DLSS + FSR", help1 = T({ "NVIDIA RTX: DLSS and FSR 3.1 are installed.", "NVIDIA RTX: o DLSS e o FSR 3.1 são instalados." });
    else
    {
        value = T({ "FSR (NO DLSS)", "FSR (SEM DLSS)" });
        help1 = gpu.vendor == settings::VENDOR_NVIDIA ? T({ "This NVIDIA GPU can't run DLSS (RTX only): FSR 3.1 instead.", "Esta NVIDIA não roda DLSS (só RTX): FSR 3.1 no lugar." })
                                                      : T({ "DLSS needs an NVIDIA RTX GPU: it is left out; FSR 3.1 instead.", "O DLSS precisa de NVIDIA RTX: fica de fora; FSR 3.1 no lugar." });
    }
    return { { "VIDEO CARD", "PLACA DE VÍDEO" }, value, { name, help1 }, false, false, true, RowGpu };
}

std::vector<InstallRow> InstallRows()
{
    std::vector<InstallRow> rows;
    fs::path game = GameFolder();
    bool haveGame = paths::IsGameFolder(game);
    if (g_inst.bundle)
        rows.push_back({ { "INSTALL TO", "INSTALAR EM" }, T({ "CHANGE...", "TROCAR..." }),
                         { Narrow(g_inst.installDir.wstring()), T({ "Any folder or drive of yours. Your saves stay in its userdata folder.",
                                                                   "Qualquer pasta ou disco seu. Seus saves ficam na pasta userdata dela." }) },
                         false, true, true, RowInstallDir });
    rows.push_back({ { "GAME FOLDER", "PASTA DO JOGO" }, game.empty() ? T({ "CHOOSE...", "ESCOLHER..." }) : T({ "CHANGE...", "TROCAR..." }),
                     { game.empty() ? T({ "The folder of your Crash of the Titans (Xbox 360) dump:", "A pasta do seu Crash of the Titans (Xbox 360):" })
                                    : Narrow(game.wstring()),
                       T({ "default.xex, default.rcf, english.rcf, sound1.rcf... It is never changed.",
                           "default.xex, default.rcf, english.rcf, sound1.rcf... Ela nunca é alterada." }) },
                     false, true, !game.empty(), RowGame });
    rows.push_back({ { "GAME", "JOGO" },
                     haveGame ? (g_artLoaded ? std::string("CRASH OF THE TITANS (XBOX 360)") : std::string(T({ "FOUND", "ENCONTRADO" })))
                              : std::string(T({ "NOT FOUND", "NÃO ENCONTRADO" })),
                     { T({ "The installer checks default.xex (only the known version is supported)", "O instalador confere o default.xex (só a versão conhecida é suportada)" }),
                       T({ "and recompiles it on this PC. No game file is downloaded.", "e o recompila neste PC. Nenhum arquivo do jogo é baixado." }) },
                     false, false, false, RowGameStatus });
    if (g_inst.bundle)
    {
        rows.push_back(GpuRow());
        uint64_t need = RequiredBytes(), free = FreeBytes(g_inst.installDir);
        rows.push_back({ { "DISK SPACE", "ESPAÇO EM DISCO" }, SizeText(need) + T({ " (", " (LIVRE " }) + SizeText(free) + T({ " FREE)", ")" }),
                         { std::string(T({ "Needed with these options: ", "Necessário com estas opções: " })) + SizeText(need) +
                               T({ "; free on that drive: ", "; livre nesse disco: " }) + SizeText(free) + ".",
                           free >= need ? T({ "The compiler, Python, CMake and the libraries come inside the installer.",
                                              "O compilador, o Python, o CMake e as bibliotecas vêm dentro do instalador." })
                                        : T({ "Not enough: choose another drive or fewer options.", "Não cabe: escolha outro disco ou menos opções." }) },
                         false, false, true, RowSpace });
    }
    else
    {
        std::string prereq;
        std::string help0, help1, packages;
        {
            std::lock_guard lock(g_inst.mutex);
            int missing = MissingCount();
            packages = MissingPackages();
            if (g_inst.checking || !g_inst.checked)
                prereq = T({ "CHECKING...", "VERIFICANDO..." });
            else if (!missing)
                prereq = UsesBundledToolchain() ? T({ "INCLUDED", "INCLUÍDOS" }) : T({ "ALL READY", "TUDO PRONTO" });
            else
                prereq = std::string(T({ "MISSING ", "FALTAM " })) + std::to_string(missing) + T({ " (INSTALL)", " (INSTALAR)" });
            for (const Prereq& p : g_inst.prereqs)
                (p.ok ? help1 : help0) += (p.ok ? (help1.empty() ? "" : ", ") : (help0.empty() ? "" : ", ")) + p.name;
        }
        bool bundled = UsesBundledToolchain(), allIncluded = bundled && help0.empty() && packages.empty();
        if (!help0.empty())
            help0 = std::string(T({ "Missing: ", "Faltam: " })) + help0 +
                    (bundled ? T({ ". The release zip has them; click to download them (tools\\toolchain.py).",
                                   ". O zip da release os traz; clique para baixá-los (tools\\toolchain.py)." })
                             : T({ ". Click to install with winget, then click again to check.",
                                   ". Clique para instalar pelo winget e depois de novo para conferir." }));
        else if (!packages.empty())
            help0 = std::string(T({ "Python packages for ASSETS FOR MODDING: ", "Pacotes Python para ASSETS PARA MODDING: " })) + packages +
                    T({ " (pip installs them then; click to install now).", " (o pip instala na hora; clique para instalar agora)." });
        else if (bundled)
            help0 = T({ "Compiler (clang), CMake, Ninja and Python come in the toolchain folder.",
                        "Compilador (clang), CMake, Ninja e Python vêm na pasta toolchain." });
        else
            help0 = T({ "Visual Studio Build Tools (C++), LLVM clang-cl, CMake and Ninja.",
                        "Visual Studio Build Tools (C++), LLVM clang-cl, CMake e Ninja." });
        if (allIncluded)
            help1 = T({ "Nothing is installed on this PC.", "Nada é instalado neste PC." });
        else
            help1 = help1.empty() ? std::string() : std::string("OK: ") + help1;
        rows.push_back({ { "PREREQUISITES", "PRÉ-REQUISITOS" }, prereq, { help0, help1 }, false, true, true, RowPrereqs });
    }
    rows.push_back({ { "OPTIONS", "OPÇÕES" }, "", {}, true });
    rows.push_back({ { "SUBTITLES", "LEGENDAS" }, OnOff(g_inst.subtitles),
                     { T({ "Subtitles for the cutscenes and dialogue: English transcript and the Brazilian", "Legendas nas cenas e falas: transcrição em inglês e a tradução" }),
                       T({ "Portuguese translation (about 1 MB download).", "em português do Brasil (cerca de 1 MB para baixar)." }) },
                     false, true, false, RowSubtitles });
    rows.push_back({ { "HD AI REMASTER", "HD AI REMASTER" }, OnOff(g_inst.remaster),
                     { T({ "AI-upscaled textures and smoother models for the whole game (fan made).", "Texturas ampliadas por IA e modelos suavizados no jogo todo (feito por fãs)." }),
                       T({ "About 4.5 GB to download; turn it on in the launcher afterwards.", "Cerca de 4,5 GB para baixar; ligue depois no launcher." }) },
                     false, true, false, RowRemaster });
    rows.push_back({ { "ASSETS FOR MODDING", "ASSETS PARA MODDING" }, OnOff(g_inst.extractAssets),
                     { T({ "Exports the game's sounds, videos, textures and 3D models to open formats", "Exporta sons, vídeos, texturas e modelos 3D do jogo para formatos abertos" }),
                       T({ "in private\\assets (about 9 minutes, 18 GB). Only for mods.",
                           "em private\\assets (cerca de 9 minutos, 18 GB). Só para mods." }) },
                     false, true, false, RowAssets });
    if (!g_inst.bundle)  // the single-file installer always copies it (into game\ of the install folder)
        rows.push_back({ { "COPY THE GAME INTO THE FOLDER", "COPIAR O JOGO PARA A PASTA" }, OnOff(g_inst.copyGame),
                         { T({ "On: the dump is copied to dist\\game (3 GB). Off: the game keeps", "Ligado: o jogo é copiado para dist\\game (3 GB). Desligado: o jogo usa" }),
                           T({ "using it where it is.", "a pasta onde ele está." }) },
                         false, true, false, RowCopy });
    if (!g_inst.bundle)
        rows.push_back({ { "BUILD TOOLS", "FERRAMENTAS" }, UsesBundledToolchain() ? T({ "INCLUDED", "INCLUÍDAS" }) : std::string("VISUAL STUDIO"),
                         { T({ "Advanced. INCLUDED: the toolchain folder of the zip, nothing to install. VISUAL STUDIO:",
                               "Avançado. INCLUÍDAS: a pasta toolchain do zip, nada a instalar. VISUAL STUDIO:" }),
                           T({ "an installed Visual Studio Build Tools + LLVM clang-cl (winget installs them).",
                               "Visual Studio Build Tools + LLVM clang-cl instalados (o winget os instala)." }) },
                         false, true, false, RowToolchain });
    return rows;
}

// The last screen of the single-file installer: the shortcuts to make.
std::vector<InstallRow> DoneRows()
{
    std::vector<InstallRow> rows;
    rows.push_back({ { "INSTALLED IN", "INSTALADO EM" }, T({ "OPEN...", "ABRIR..." }),
                     { Narrow(PlayDir().wstring()), T({ "The launcher is CrashLauncher.exe there; your saves are in userdata.",
                                                        "O launcher é o CrashLauncher.exe de lá; seus saves ficam em userdata." }) },
                     false, true, true, RowInstalledIn });
    rows.push_back({ { "SHORTCUTS", "ATALHOS" }, "", {}, true });
    rows.push_back({ { "DESKTOP SHORTCUT", "ATALHO NA ÁREA DE TRABALHO" }, g_inst.desktopShortcut ? T({ "YES", "SIM" }) : T({ "NO", "NÃO" }),
                     { T({ "Create a shortcut to the launcher on the desktop?", "Criar um atalho para o launcher na área de trabalho?" }),
                       g_inst.shortcutsDone ? T({ "Done.", "Feito." }) : T({ "It is made when you click PLAY or FINISH.", "Ele é criado ao clicar em JOGAR ou CONCLUIR." }) },
                     false, !g_inst.shortcutsDone, false, RowDesktop });
    rows.push_back({ { "START MENU", "MENU INICIAR" }, g_inst.startMenu ? T({ "YES", "SIM" }) : T({ "NO", "NÃO" }),
                     { T({ "An entry for the launcher in the Start menu (CrashRecomp folder)?", "Uma entrada do launcher no menu Iniciar (pasta CrashRecomp)?" }),
                       g_inst.shortcutsDone ? T({ "Done.", "Feito." }) : T({ "It is made when you click PLAY or FINISH.", "Ela é criada ao clicar em JOGAR ou CONCLUIR." }) },
                     false, !g_inst.shortcutsDone, false, RowStartMenu });
    return rows;
}

void ActivateRow(int id, int direction)
{
    switch (id)
    {
    case RowGame: ChooseInstallerGameFolder(); break;
    case RowInstallDir: ChooseInstallDir(); break;
    case RowInstalledIn: OpenFolder(PlayDir()); break;
    case RowDesktop: g_inst.desktopShortcut = !g_inst.desktopShortcut; break;
    case RowStartMenu: g_inst.startMenu = !g_inst.startMenu; break;
    case RowPrereqs:
        if (!g_inst.checking)
        {
            if (g_inst.checked && (MissingCount() || !MissingPackages().empty()))
                InstallMissing();
            CheckPrerequisites();
        }
        break;
    case RowSubtitles: g_inst.subtitles = !g_inst.subtitles; break;
    case RowRemaster: g_inst.remaster = !g_inst.remaster; break;
    case RowAssets: g_inst.extractAssets = !g_inst.extractAssets; break;
    case RowCopy:
        g_inst.copyGame = !g_inst.copyGame;
        if (g_inst.copyGame && !g_inst.gameBytes)
            g_inst.gameBytes = FolderBytes(GameFolder());
        break;
    case RowToolchain:
        if (!g_inst.checking)
        {
            g_inst.toolchain = UsesBundledToolchain() ? "msvc" : "bundled";
            g_inst.toolchainChosen = true;
            CheckPrerequisites();
        }
        break;
    }
    (void)direction;
}

bool CanInstall()
{
    if (g_inst.bundle && (!g_inst.gpuKnown || g_inst.installDir.empty()))
        return false;
    std::lock_guard lock(g_inst.mutex);
    return paths::IsGameFolder(GameFolder()) && g_inst.checked && !g_inst.checking && MissingCount() == 0;
}

// The install stopped at the last step (the asset export) with the game already in dist\.
bool AssetsOnlyFailed()
{
    std::lock_guard lock(g_inst.mutex);
    return g_inst.phase == Phase::Failed && g_inst.gameReady && g_inst.step == INSTALL_STEP_COUNT;
}

void DrawInstallerLeft()
{
    float logoW = 404.0f, logoH = g_logo.view ? logoW * g_logo.h / g_logo.w : 0.0f, y = 34.0f;
    if (g_logo.view)
    {
        g_draw->AddImage(g_logo.Id(), g_view.P(LEFT_CX - logoW * 0.5f, y), g_view.P(LEFT_CX + logoW * 0.5f, y + logoH));
        y += logoH - 6.0f;
    }
    else
    {
        Draw(1, "CRASH OF THE TITANS", LEFT_CX, y + 90.0f, 46.0f, TITLE, 0.5f, 420.0f);
        y += 150.0f;
    }
    Draw(1, "DEFINITIVE EDITION", LEFT_CX, y, 38.0f, GOLD, 0.5f, 420.0f);

    struct Entry { Text text; int id; bool enabled; };
    std::vector<Entry> entries;
    switch (g_inst.phase)
    {
    case Phase::Setup: entries = { { { "INSTALL", "INSTALAR" }, 0, CanInstall() }, { { "EXIT", "SAIR" }, 3, true } }; break;
    case Phase::Running: entries = { { { "CANCEL", "CANCELAR" }, 1, true } }; break;
    case Phase::Done:
        if (g_inst.bundle)
            entries = { { { "PLAY", "JOGAR" }, 2, true }, { { "FINISH", "CONCLUIR" }, 8, true } };
        else
            entries = { { { "PLAY", "JOGAR" }, 2, true }, { { "OPEN FOLDER", "ABRIR PASTA" }, 4, true }, { { "EXIT", "SAIR" }, 3, true } };
        break;
    case Phase::Failed:
        if (AssetsOnlyFailed())
            entries = { { { "TRY AGAIN", "TENTAR DE NOVO" }, 7, true }, { { "PLAY", "JOGAR" }, 2, true }, { { "OPEN LOG", "ABRIR LOG" }, 6, true },
                        { { "EXIT", "SAIR" }, 3, true } };
        else
            entries = { { { "TRY AGAIN", "TENTAR DE NOVO" }, 5, true }, { { "OPEN LOG", "ABRIR LOG" }, 6, true }, { { "EXIT", "SAIR" }, 3, true } };
        break;
    }
    float my = 384.0f;
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (size_t i = 0; i < entries.size(); i++)
    {
        const Entry& entry = entries[i];
        std::string text = T(entry.text);
        float size = i == 0 ? 54.0f : 42.0f, width = TextWidth(1, text, size);
        ImGui::SetCursorScreenPos(g_view.P(LEFT_CX - width * 0.5f - 20.0f, my - 4.0f));
        ImGui::PushID(entry.id);
        bool pressed = ImGui::InvisibleButton("menu", ImVec2((width + 40.0f) * g_view.s, (size + 8.0f) * g_view.s));
        bool hovered = ImGui::IsItemHovered() && g_modal == Modal::None;
        ImGui::PopID();
        Draw(1, text, LEFT_CX, my, size, !entry.enabled ? INFO : hovered ? MENU_SELECTED : MENU, 0.5f);
        if (pressed && entry.enabled && g_modal == Modal::None)
        {
            switch (entry.id)
            {
            case 0: StartInstall(); break;
            case 1: CancelInstall(); break;
            case 2: OpenLauncher(); break;
            case 3: g_quit = true; break;
            case 4: OpenFolder(PlayDir()); break;
            case 5: g_inst.phase = Phase::Setup; CheckPrerequisites(); break;
            case 6: ShellExecuteW(nullptr, L"open", SetupLog().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
            case 7: StartInstall(true); break;  // only the asset export, again
            case 8: ApplyShortcuts(); g_quit = true; break;
            }
        }
        my += size + 18.0f;
    }
    ImGui::PopItemFlag();
    if (g_inst.bundle)
    {
        std::string version = g_payload.tag.empty() ? std::string() : " " + g_payload.tag;
        DrawUi(std::string(T({ "Installer", "Instalador" })) + version + T({ ": builds the game from your own copy", ": monta o jogo a partir da sua cópia" }),
               LEFT_CX, 650.0f, 17.0f, IM_COL32(255, 245, 220, 235), 0.5f, 430.0f);
        DrawUi(T({ "Everything included: nothing else to install", "Tudo incluído: nada mais para instalar" }), LEFT_CX, 674.0f, 15.0f,
               IM_COL32(255, 245, 220, 180), 0.5f, 430.0f);
        return;
    }
    DrawUi(T({ "Installer: builds the game from your own copy", "Instalador: monta o jogo a partir da sua cópia" }), LEFT_CX, 650.0f, 17.0f,
           IM_COL32(255, 245, 220, 235), 0.5f, 430.0f);
    DrawUi(Narrow(paths::ExeDir().wstring()), LEFT_CX, 674.0f, 15.0f, IM_COL32(255, 245, 220, 180), 0.5f, 430.0f);
}

void DrawPanelTitle(const std::string& title)
{
    float cx = (PANEL_L + PANEL_R) * 0.5f;
    Draw(1, title, cx, TABS_Y, TAB_SIZE + 4.0f, SELECTED, 0.5f);
    float dy = TABS_Y + TAB_SIZE + 12.0f;
    if (g_dividerL.view && g_dividerR.view)
    {
        float h = 18.0f, wl = h * g_dividerL.w / g_dividerL.h, wr = h * g_dividerR.w / g_dividerR.h;
        g_draw->AddImage(g_dividerL.Id(), g_view.P(cx - 8.0f - wl, dy), g_view.P(cx - 8.0f, dy + h), ImVec2(0, 0), ImVec2(1, 1), TITLE.outline);
        g_draw->AddImage(g_dividerR.Id(), g_view.P(cx + 8.0f, dy), g_view.P(cx + 8.0f + wr, dy + h), ImVec2(0, 0), ImVec2(1, 1), TITLE.outline);
    }
}

// The rows of the setup screen (allowInstall: START / F5 installs) or of the last screen.
void DrawRows(const std::vector<InstallRow>& rows, bool allowInstall)
{
    std::vector<int> selectable;
    for (size_t i = 0; i < rows.size(); i++)
        if (!rows[i].header && rows[i].enabled)
            selectable.push_back(int(i));
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
        g_inst.mouseMode = true;
    if (g_modal == Modal::None && !selectable.empty())
    {
        int at = int(std::find(selectable.begin(), selectable.end(), g_inst.selected) - selectable.begin());
        if (at >= int(selectable.size()) && !(g_inst.selected >= 0 && g_inst.selected < int(rows.size()) && rows[g_inst.selected].plainHelp))
            at = 0;
        bool moved = false;
        if (KeyPressed({ ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown }))
            at = (std::min(at, int(selectable.size()) - 1) + 1) % int(selectable.size()), g_inst.mouseMode = false, moved = true;
        if (KeyPressed({ ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp }))
            at = (std::min(at, int(selectable.size()) - 1) + int(selectable.size()) - 1) % int(selectable.size()), g_inst.mouseMode = false, moved = true;
        if (at < int(selectable.size()) && (moved || !g_inst.mouseMode))
            g_inst.selected = selectable[at];
        bool onSelectable = std::find(selectable.begin(), selectable.end(), g_inst.selected) != selectable.end();
        if (onSelectable && KeyPressed({ ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown, ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
                                         ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight }, false))
            ActivateRow(rows[g_inst.selected].id, +1);
        if (allowInstall && KeyPressed({ ImGuiKey_GamepadStart, ImGuiKey_F5 }, false) && CanInstall())
            StartInstall();
        if (KeyPressed({ ImGuiKey_Escape, ImGuiKey_GamepadFaceRight }, false))
        {
            ApplyShortcuts();
            g_quit = true;
        }
    }
    for (size_t i = 0; i < rows.size(); i++)
    {
        const InstallRow& row = rows[i];
        float vy = ROWS_T + (ROW_PITCH - ROW_SIZE) * 0.5f + i * ROW_PITCH;
        if (row.header)
        {
            std::string text = T(row.label);
            float cx = (PANEL_L + PANEL_R) * 0.5f;
            Draw(1, text, cx, vy + 2.0f, ROW_SIZE - 4.0f, TITLE, 0.5f);
            float w = TextWidth(1, text, ROW_SIZE - 4.0f) * 0.5f + 16.0f, ly = vy + ROW_SIZE * 0.5f + 2.0f;
            g_draw->AddLine(g_view.P(PANEL_L + 70.0f, ly), g_view.P(cx - w, ly), IM_COL32(214, 120, 14, 140), 2.0f * g_view.s);
            g_draw->AddLine(g_view.P(cx + w, ly), g_view.P(PANEL_R - 70.0f, ly), IM_COL32(214, 120, 14, 140), 2.0f * g_view.s);
            continue;
        }
        ImGui::SetCursorScreenPos(g_view.P(PANEL_L + 20.0f, vy - (ROW_PITCH - ROW_SIZE) * 0.5f));
        ImGui::PushID(int(i));
        bool pressed = ImGui::InvisibleButton("row", ImVec2((PANEL_R - PANEL_L - 40.0f) * g_view.s, ROW_PITCH * g_view.s));
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        // Info rows with details (VIDEO CARD, DISK SPACE) show their help on hover too.
        if ((row.enabled || row.plainHelp) && hovered && g_inst.mouseMode)
            g_inst.selected = int(i);
        bool selected = row.enabled && g_inst.selected == int(i);
        const Style& style = !row.enabled ? INFO : selected ? SELECTED : NORMAL;
        Draw(0, T(row.label), LABEL_RIGHT, vy, ROW_SIZE, style, 1.0f, LABEL_RIGHT - LABEL_LEFT);
        const Style& valueStyle = row.id == RowGameStatus ? (paths::IsGameFolder(GameFolder()) ? SELECTED : WARNING)
                                  : row.id == RowPrereqs && MissingCount() && g_inst.checked ? WARNING
                                  : row.id == RowSpace ? (EnoughSpace() ? SELECTED : WARNING)
                                  : row.id == RowGpu ? (g_inst.gpuKnown ? SELECTED : INFO)
                                                     : style;
        Draw(0, row.value, VALUE_X, vy, ROW_SIZE, valueStyle, 0.0f, VALUE_R - VALUE_X);
        if (pressed && row.enabled && g_modal == Modal::None)
        {
            g_inst.selected = int(i);
            ActivateRow(row.id, +1);
        }
    }
    // Help of the selected row.
    if (g_inst.selected >= 0 && g_inst.selected < int(rows.size()))
    {
        const InstallRow& row = rows[g_inst.selected];
        float maxW = PANEL_R - PANEL_L - 160.0f, cx = (PANEL_L + PANEL_R) * 0.5f;
        g_draw->AddLine(g_view.P(PANEL_L + 90.0f, HELP_Y - 8.0f), g_view.P(PANEL_R - 90.0f, HELP_Y - 8.0f), IM_COL32(236, 222, 190, 70), 2.0f * g_view.s);
        if (row.plainHelp)
            DrawUi(row.help[0], cx, HELP_Y, 21.0f, IM_COL32(238, 226, 196, 255), 0.5f, maxW);
        else
            Draw(0, row.help[0], cx, HELP_Y, HELP_SIZE, HELP, 0.5f, maxW);
        Draw(0, row.help[1], cx, HELP_Y + HELP_SIZE + 4.0f, HELP_SIZE, HELP, 0.5f, maxW);
    }
}

// The steps as shown: EXTRACTING FILES first in the single-file installer, then setup.py's.
int ShownStepCount() { return INSTALL_STEP_COUNT + (g_inst.bundle ? 1 : 0); }
const InstallStep& ShownStep(int i) { return g_inst.bundle ? (i == 0 ? EXTRACT_STEP : INSTALL_STEPS[i - 1]) : INSTALL_STEPS[i]; }
// 1-based, as setup.py's "@@step" (0 = not started).
int CurrentShownStep() { return g_inst.bundle ? (g_inst.extracting ? 1 : std::max(2, g_inst.step + 1)) : g_inst.step; }

void DrawProgress()
{
    std::lock_guard lock(g_inst.mutex);
    float cx = (PANEL_L + PANEL_R) * 0.5f;
    float y = ROWS_T - 6.0f;
    int current = CurrentShownStep(), count = ShownStepCount();
    float pitch = count > INSTALL_STEP_COUNT ? 31.0f : 34.0f;
    for (int i = 0; i < count; i++)
    {
        int number = i + 1;
        bool done = number < current || g_inst.phase == Phase::Done;
        bool now = number == current && g_inst.phase != Phase::Done;
        const Style& style = done ? SELECTED : now ? (g_inst.phase == Phase::Failed ? WARNING : TITLE) : INFO;
        Draw(0, T(ShownStep(i).name), LABEL_RIGHT, y, 26.0f, style, 1.0f, LABEL_RIGHT - LABEL_LEFT + 60.0f);
        float progress = g_inst.extracting && number == 1 ? (g_inst.extract.total ? float(double(g_inst.extract.done) / double(g_inst.extract.total)) : 0.0f)
                                                          : g_inst.progress;
        if (done)
            Draw(0, "OK", VALUE_X, y, 26.0f, SELECTED);
        else if (now && g_inst.phase == Phase::Running)
        {
            if (progress >= 0.0f)
            {
                DrawStars(VALUE_X, y - 2.0f, int(progress * 100.0f), 100, TITLE);
                Draw(0, std::to_string(int(progress * 100.0f)) + "%", VALUE_R, y, 24.0f, TITLE, 1.0f);
            }
            else
            {
                // An indeterminate run of stars.
                int lit = int(ImGui::GetTime() * 4.0) % (STARS + 1);
                DrawStars(VALUE_X, y - 2.0f, lit, STARS, TITLE);
            }
        }
        else if (now && g_inst.phase == Phase::Failed)
            Draw(0, T({ "FAILED", "FALHOU" }), VALUE_X, y, 26.0f, WARNING);
        y += pitch;
    }
    // The current detail (download, step detail or the error) and the end of the log, in the Windows font.
    float maxW = PANEL_R - PANEL_L - 120.0f;
    std::string detail = g_inst.phase == Phase::Failed ? g_inst.error : !g_inst.download.empty() ? g_inst.download : g_inst.stepDetail;
    if (g_inst.extracting && g_inst.phase == Phase::Running)
    {
        char text[64];
        snprintf(text, sizeof(text), "%.0f / %.0f MB", double(g_inst.extract.done) / double(MB), double(g_inst.extract.total) / double(MB));
        detail = text;
    }
    g_draw->AddLine(g_view.P(PANEL_L + 90.0f, y + 2.0f), g_view.P(PANEL_R - 90.0f, y + 2.0f), IM_COL32(236, 222, 190, 70), 2.0f * g_view.s);
    DrawUi(detail, cx, y + 10.0f, 19.0f, g_inst.phase == Phase::Failed ? IM_COL32(255, 140, 100, 255) : IM_COL32(255, 236, 160, 255), 0.5f, maxW);
    float ly = y + 38.0f;
    if (g_inst.phase == Phase::Failed && !g_inst.hint.empty())
    {
        // The command to run by hand (setup.py "@@hint").
        DrawUi(g_inst.hint, cx, ly, 16.0f, IM_COL32(255, 236, 160, 255), 0.5f, maxW);
        ly += 28.0f;
    }
    int lines = std::max(0, int((PANEL_B - 30.0f - ly) / 17.0f));
    size_t first = g_inst.log.size() > size_t(lines) ? g_inst.log.size() - lines : 0;
    for (size_t i = first; i < g_inst.log.size(); i++, ly += 17.0f)
        DrawUi(g_inst.log[i], PANEL_L + 60.0f, ly, 14.0f, IM_COL32(238, 226, 196, 210), 0.0f, maxW);
    if (g_inst.phase == Phase::Done)
    {
        Draw(1, T({ "READY!", "PRONTO!" }), cx, PANEL_B + 14.0f, 40.0f, GOLD, 0.5f);
    }
}

void UpdateInstaller()
{
    UpdateGpuCheck();
    if (g_inst.extracting)
    {
        std::string error;
        {
            std::lock_guard lock(g_inst.mutex);
            if (!g_inst.extractFinished)
                return;
            error = g_inst.extractError;
        }
        g_inst.extractor.join();
        std::lock_guard lock(g_inst.mutex);
        g_inst.extracting = false;
        if (!error.empty())
        {
            g_inst.error = error == "cancelled" ? T({ "Stopped.", "Interrompido." }) : error;
            g_inst.phase = Phase::Failed;
            return;
        }
        g_inst.python = BundledPython().wstring();
        g_inst.python = L"\"" + g_inst.python + L"\"";
        AddLogLine(T({ "Files extracted; building the game...", "Arquivos extraídos; montando o jogo..." }));
        g_inst.step = 1;
        g_inst.setupPending = true;
        return;  // setup.py starts on the next frame, without the mutex (StartSetup takes it)
    }
    if (g_inst.setupPending)
    {
        g_inst.setupPending = false;
        if (!StartSetup(Widen(g_inst.pendingArgs), false))
        {
            std::lock_guard lock(g_inst.mutex);
            g_inst.error = T({ "Could not start the installed Python (toolchain\\python).", "Não foi possível abrir o Python instalado (toolchain\\python)." });
            g_inst.phase = Phase::Failed;
        }
        return;
    }
    std::lock_guard lock(g_inst.mutex);
    if (!g_inst.processDone || !g_inst.process)
        return;
    CloseHandle(g_inst.process);
    g_inst.process = nullptr;
    if (g_inst.checking)
    {
        g_inst.checking = false;
        g_inst.checked = true;
        return;
    }
    g_inst.phase = g_inst.exitCode == 0 ? Phase::Done : Phase::Failed;
    if (g_inst.phase == Phase::Failed && g_inst.error.empty())
        g_inst.error = g_inst.exitCode == 1 && g_inst.job ? T({ "Stopped.", "Interrompido." }) : T({ "setup.py failed (see the log).", "o setup.py falhou (veja o log)." });
    if (g_inst.phase == Phase::Done && g_inst.bundle)
    {
        RememberInstallDir();
        g_inst.selected = 2;  // DESKTOP SHORTCUT
    }
    if (g_inst.logFile)
        fclose(g_inst.logFile), g_inst.logFile = nullptr;
}

// --shots: a PNG each time the screen shows something new (the setup screen, each step, the end).
std::string ShotKey()
{
    return std::to_string(int(g_inst.phase)) + "_" + std::to_string(g_inst.phase == Phase::Running ? CurrentShownStep() : 0) +
           (g_inst.phase == Phase::Setup ? (g_inst.gpuKnown ? "k" : "u") : "");
}

void TakeShots()
{
    if (g_inst.shots.empty())
        return;
    std::string key = ShotKey();
    if (key != g_inst.lastShot)
    {
        g_inst.lastShot = key;
        g_inst.shotWait = 20;  // let the step's first lines arrive
        return;
    }
    if (g_inst.shotWait < 0 || --g_inst.shotWait > 0)
        return;
    g_inst.shotWait = -1;
    static const char* phases[] = { "setup", "step", "done", "failed" };
    std::string name = phases[int(g_inst.phase)];
    if (g_inst.phase == Phase::Running)
        name += "_" + std::to_string(CurrentShownStep());
    char file[96];
    snprintf(file, sizeof(file), "%02d_%s.png", ++g_inst.shotNumber, name.c_str());
    std::error_code error;
    fs::create_directories(g_inst.shots, error);
    g_inst.pendingShot = g_inst.shots / file;
}

// Whether the main loop may end (--autofinish): the last screenshot was taken.
bool InstallerShotsDone() { return g_inst.shots.empty() || (g_inst.pendingShot.empty() && g_inst.shotWait < 0 && g_inst.lastShot == ShotKey()); }

void DrawInstallerContents()
{
    ImGuiIO& io = ImGui::GetIO();
    float s = std::min(io.DisplaySize.x / VW, io.DisplaySize.y / VH);
    g_view = { s, (io.DisplaySize.x - VW * s) * 0.5f, (io.DisplaySize.y - VH * s) * 0.5f };
    UpdateInstaller();
    if (g_inst.autoStart && g_inst.phase == Phase::Setup && CanInstall() && g_modal == Modal::None && InstallerShotsDone())
    {
        g_inst.autoStart = false;
        StartInstall();
    }
    if (g_inst.autoStart && g_inst.phase == Phase::Setup && g_inst.checked && !g_inst.checking && (!g_inst.bundle || g_inst.gpuKnown) && !CanInstall())
        g_inst.autoStart = false;  // nothing to wait for: missing game folder or prerequisites
    if (g_inst.autoFinish && g_modal != Modal::None)
        g_modal = Modal::None, g_inst.finalCode = 1, g_quit = true;  // a message (no space, can't write): it stops there
    if (g_inst.autoFinish && !g_inst.autoStart && g_inst.phase != Phase::Running && InstallerShotsDone() && (g_inst.phase != Phase::Setup || g_inst.gpuKnown))
    {
        ApplyShortcuts();
        g_inst.finalCode = g_inst.phase == Phase::Done ? 0 : 1;
        g_quit = true;
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("installer", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
    g_draw = ImGui::GetWindowDrawList();
    if (g_backdrop.view)
        ImGui::GetBackgroundDrawList()->AddImage(g_backdrop.Id(), ImVec2(0, 0), io.DisplaySize);
    DrawFrame(PANEL_L, PANEL_T, PANEL_R, PANEL_B);
    DrawInstallerLeft();
    switch (g_inst.phase)
    {
    case Phase::Setup:
        DrawPanelTitle(T({ "INSTALLATION", "INSTALAÇÃO" }));
        DrawRows(InstallRows(), true);
        break;
    case Phase::Running: DrawPanelTitle(T({ "INSTALLING...", "INSTALANDO..." })); DrawProgress(); break;
    case Phase::Done:
        DrawPanelTitle(T({ "INSTALLED", "INSTALADO" }));
        if (g_inst.bundle)
        {
            DrawRows(DoneRows(), false);
            Draw(1, T({ "READY!", "PRONTO!" }), (PANEL_L + PANEL_R) * 0.5f, PANEL_B + 14.0f, 40.0f, GOLD, 0.5f);
        }
        else
            DrawProgress();
        break;
    case Phase::Failed:
        DrawPanelTitle(AssetsOnlyFailed() ? T({ "THE GAME IS READY", "O JOGO ESTÁ PRONTO" }) : T({ "SOMETHING WENT WRONG", "ALGO DEU ERRADO" }));
        DrawProgress();
        break;
    }
    if (g_modal != Modal::None)
    {
        g_draw = ImGui::GetForegroundDrawList();
        DrawModal();
    }
    ImGui::End();
    TakeShots();
}

// Closing the window while setup.py runs stops it (the job object kills cmake / ninja too); on the
// last screen it makes the chosen shortcuts.
void InstallerExit()
{
    if (g_inst.phase == Phase::Running)
        CancelInstall();
    ApplyShortcuts();
    g_quit = true;
}

// Before paths::Init (WinMain): the payload, --install-dir / --shots (wide: any letters) and the
// folder to install to.
void PrepareInstaller()
{
    g_payload = ReadPayload();
    g_inst.bundle = g_payload.present;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; i++)
    {
        if (wcscmp(argv[i], L"--install-dir") == 0)
            g_inst.installDir = fs::absolute(fs::path(argv[++i]));
        else if (wcscmp(argv[i], L"--shots") == 0)
            g_inst.shots = fs::absolute(fs::path(argv[++i]));
    }
    LocalFree(argv);
    if (g_inst.bundle && g_inst.installDir.empty())
        g_inst.installDir = DefaultInstallDir();
}

// The game folder of an earlier install in the same place (a second run), empty if none.
fs::path InstalledGameFolder()
{
    fs::path dist = PlayDir();
    if (paths::IsGameFolder(dist / "game"))
        return dist / "game";
    std::string game = settings::ReadGameFolder(dist / "userdata");
    if (!game.empty() && paths::IsGameFolder(fs::u8path(game)))
        return fs::u8path(game).make_preferred();
    return {};
}

void InitInstaller()
{
    LANGID ui = GetUserDefaultUILanguage();
    g_values.language = PRIMARYLANGID(ui) == LANG_PORTUGUESE ? "pt-br" : "english";
    if (g_inst.bundle)
    {
        Log("[setup] single-file installer %s: %zu files, %.0f MB; install folder %s", g_payload.tag.c_str(), g_payload.files.size(),
            double(g_payload.Bytes({})) / double(MB), Narrow(g_inst.installDir.wstring()).c_str());
        StartGpuCheck();
        g_inst.gameBytes = FolderBytes(GameFolder());  // always copied
    }
    CheckPrerequisites();
}

void ShutdownInstaller()
{
    CancelInstall();
    if (g_inst.extractor.joinable())
        g_inst.extractor.join();
    if (g_inst.reader.joinable())
        g_inst.reader.join();
    if (g_inst.logFile)
        fclose(g_inst.logFile);
    if (!g_inst.probeDir.empty() && g_dlssProbe.state == 2)
    {
        std::error_code error;
        fs::remove_all(g_inst.probeDir, error);
    }
}
