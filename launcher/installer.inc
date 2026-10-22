// CrashSetup: the installer screen, compiled into launcher.cpp's anonymous namespace when
// CRASH_INSTALLER is defined (CMake target CrashSetup), so it has the launcher's look: the game's
// cloth, logo, Titans fonts, message frame and stars, read from the chosen dump's default.rcf.
//
// It is a front end for tools\setup.py (next to the exe, in the release bundle or a clone):
//   - choose the game folder (then it starts again with --game to load the art);
//   - check the prerequisites (setup.py --check) and install the missing ones with winget;
//   - options: subtitles, HD AI Remaster, asset export for modding, copy the dump into dist\game;
//   - run setup.py, showing its steps ("@@step"), progress ("@@progress", "@@download") and log;
//   - at the end, open dist\CrashLauncher.exe.
// Release zips placed next to CrashSetup.exe are used instead of downloading (setup.py --assets-dir).
// Included inside launcher.cpp's anonymous namespace: its headers (<mutex>, <thread>) are at the top there.

struct InstallStep
{
    Text name;
};
const InstallStep INSTALL_STEPS[] = {
    { { "PREREQUISITES", "PRÉ-REQUISITOS" } }, { { "GAME", "JOGO" } }, { { "XENONRECOMP", "XENONRECOMP" } },
    { { "LIBRARIES", "BIBLIOTECAS" } }, { { "RECOMPILING THE GAME CODE", "RECOMPILANDO O CÓDIGO DO JOGO" } },
    { { "BUILDING", "COMPILANDO" } }, { { "EXTRAS", "EXTRAS" } }, { { "PLAY FOLDER", "PASTA DO JOGO" } },
    { { "ASSETS FOR MODDING", "ASSETS PARA MODDING" } },
};
constexpr int INSTALL_STEP_COUNT = int(sizeof(INSTALL_STEPS) / sizeof(INSTALL_STEPS[0]));

struct Prereq
{
    std::string name, path;
    bool ok = false;
};

// winget packages for each prerequisite setup.py checks (its names).
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
    std::wstring python;  // command prefix: "py -3" or a python.exe path
    bool checking = false, checked = false;
    std::vector<Prereq> prereqs;
    bool subtitles = true, remaster = false, extractAssets = false, copyGame = false;
    int selected = 0;
    bool mouseMode = false;
    bool autoStart = false;  // --autostart: install as soon as the prerequisites are checked

    // The running setup.py.
    std::mutex mutex;
    std::vector<std::string> log;  // plain lines (not "@@")
    int step = 0;                  // 1-based, 0 = not started
    std::string stepDetail;
    float progress = -1.0f;        // of the current step, -1 = unknown
    std::string download;          // "name 12/80 MB"
    std::string error, finishedDir;
    bool processDone = false;
    DWORD exitCode = 0;
    HANDLE job = nullptr, process = nullptr;
    std::thread reader;
    FILE* logFile = nullptr;
};
Installer g_inst;

fs::path SetupScript() { return paths::ExeDir() / "tools" / "setup.py"; }

std::wstring Widen(const std::string& text)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), n);
    return out;
}

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
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, paths::ExeDir().c_str(), &si, &pi))
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

// Python 3.11+ (setup.py needs tomllib): the py launcher, else python.exe on PATH (not the
// Microsoft Store alias, which only opens the Store).
std::wstring FindPython()
{
    const wchar_t* test = L" -c \"import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)\"";
    wchar_t found[MAX_PATH];
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
    else if (line.rfind("@@prereq ", 0) == 0)
    {
        // "@@prereq ok|missing <name> | <path>"
        Prereq p;
        p.ok = line.compare(9, 2, "ok") == 0;
        size_t start = line.find(' ', 9) + 1, bar = line.find(" | ");
        p.name = line.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        p.path = bar == std::string::npos ? "" : line.substr(bar + 3);
        g_inst.prereqs.push_back(p);
    }
    else if (line.rfind("@@error ", 0) == 0)
        g_inst.error = line.substr(8);
    else if (line.rfind("@@finished ", 0) == 0)
        g_inst.finishedDir = line.substr(11);
}

// Starts `python tools\setup.py <args>` with its output read on a thread. A job object holds it and
// everything it starts (cmake, ninja, clang-cl...) so CANCEL stops them all.
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
                             paths::ExeDir().c_str(), &si, &pi);
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
        if (check)
            g_inst.prereqs.clear();
        else
        {
            g_inst.log.clear();
            g_inst.step = 0, g_inst.progress = -1.0f;
            g_inst.download.clear();
            if (!g_inst.logFile)
                g_inst.logFile = _wfopen((paths::ExeDir() / L"setup.log").c_str(), L"w");
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
    g_inst.checking = StartSetup(L"--check", true);
}

int MissingCount()
{
    int n = 0;
    for (const Prereq& p : g_inst.prereqs)
        n += !p.ok;
    return n;
}

// Opens a console that installs the missing prerequisites with winget (the user sees and answers
// the prompts); CHECK AGAIN afterwards.
void InstallMissing()
{
    std::wstring commands;
    for (const Prereq& p : g_inst.prereqs)
    {
        if (p.ok)
            continue;
        for (const WingetPackage& w : WINGET)
            if (p.name.find(w.prereq) != std::string::npos)
                commands += (commands.empty() ? L"" : L" & ") + std::wstring(w.command);
    }
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

void StartInstall()
{
    fs::path game = GameFolder();
    std::wstring args = L"--game \"" + game.wstring() + L"\"";
    if (g_inst.subtitles) args += L" --subtitles";
    if (g_inst.remaster) args += L" --remaster";
    if (g_inst.extractAssets) args += L" --extract-assets";
    if (g_inst.copyGame) args += L" --copy-game";
    args += L" --assets-dir \"" + paths::ExeDir().wstring() + L"\"";
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
    if (g_inst.job)
        TerminateJobObject(g_inst.job, 1);
}

void OpenLauncher()
{
    fs::path dir = g_inst.finishedDir.empty() ? paths::ExeDir() / "dist" : fs::u8path(g_inst.finishedDir);
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

enum RowId { RowGame = 1, RowGameStatus, RowPrereqs, RowSubtitles, RowRemaster, RowAssets, RowCopy };

std::vector<InstallRow> InstallRows()
{
    std::vector<InstallRow> rows;
    fs::path game = GameFolder();
    bool haveGame = paths::IsGameFolder(game);
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
    std::string prereq;
    std::string help0, help1;
    {
        std::lock_guard lock(g_inst.mutex);
        int missing = MissingCount();
        if (g_inst.checking || !g_inst.checked)
            prereq = T({ "CHECKING...", "VERIFICANDO..." });
        else if (!missing)
            prereq = T({ "ALL READY", "TUDO PRONTO" });
        else
            prereq = std::string(T({ "MISSING ", "FALTAM " })) + std::to_string(missing) + T({ " (INSTALL)", " (INSTALAR)" });
        for (const Prereq& p : g_inst.prereqs)
            (p.ok ? help1 : help0) += (p.ok ? (help1.empty() ? "" : ", ") : (help0.empty() ? "" : ", ")) + p.name;
    }
    if (!help0.empty())
        help0 = std::string(T({ "Missing: ", "Faltam: " })) + help0 + T({ ". Click to install with winget, then click again to check.",
                                                                          ". Clique para instalar pelo winget e depois de novo para conferir." });
    else
        help0 = T({ "Visual Studio Build Tools (C++), LLVM clang-cl, CMake, Ninja, Git and Python.",
                    "Visual Studio Build Tools (C++), LLVM clang-cl, CMake, Ninja, Git e Python." });
    help1 = help1.empty() ? std::string() : std::string("OK: ") + help1;
    rows.push_back({ { "PREREQUISITES", "PRÉ-REQUISITOS" }, prereq, { help0, help1 }, false, true, true, RowPrereqs });
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
                       T({ "in private\\assets (about 8 minutes, 4 GB). Only for making mods.", "em private\\assets (cerca de 8 minutos, 4 GB). Só para fazer mods." }) },
                     false, true, false, RowAssets });
    rows.push_back({ { "COPY THE GAME INTO THE FOLDER", "COPIAR O JOGO PARA A PASTA" }, OnOff(g_inst.copyGame),
                     { T({ "On: the dump is copied to dist\\game (3 GB). Off: the game keeps", "Ligado: o jogo é copiado para dist\\game (3 GB). Desligado: o jogo usa" }),
                       T({ "using it where it is.", "a pasta onde ele está." }) },
                     false, true, false, RowCopy });
    return rows;
}

void ActivateRow(int id, int direction)
{
    switch (id)
    {
    case RowGame: ChooseInstallerGameFolder(); break;
    case RowPrereqs:
        if (!g_inst.checking)
        {
            if (g_inst.checked && MissingCount())
                InstallMissing();
            CheckPrerequisites();
        }
        break;
    case RowSubtitles: g_inst.subtitles = !g_inst.subtitles; break;
    case RowRemaster: g_inst.remaster = !g_inst.remaster; break;
    case RowAssets: g_inst.extractAssets = !g_inst.extractAssets; break;
    case RowCopy: g_inst.copyGame = !g_inst.copyGame; break;
    }
    (void)direction;
}

bool CanInstall()
{
    std::lock_guard lock(g_inst.mutex);
    return paths::IsGameFolder(GameFolder()) && g_inst.checked && !g_inst.checking && MissingCount() == 0;
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
    case Phase::Done: entries = { { { "PLAY", "JOGAR" }, 2, true }, { { "OPEN FOLDER", "ABRIR PASTA" }, 4, true }, { { "EXIT", "SAIR" }, 3, true } }; break;
    case Phase::Failed: entries = { { { "TRY AGAIN", "TENTAR DE NOVO" }, 5, true }, { { "OPEN LOG", "ABRIR LOG" }, 6, true }, { { "EXIT", "SAIR" }, 3, true } }; break;
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
            case 4: OpenFolder(paths::ExeDir() / "dist"); break;
            case 5: g_inst.phase = Phase::Setup; CheckPrerequisites(); break;
            case 6: ShellExecuteW(nullptr, L"open", (paths::ExeDir() / L"setup.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
            }
        }
        my += size + 18.0f;
    }
    ImGui::PopItemFlag();
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

void DrawSetupRows()
{
    std::vector<InstallRow> rows = InstallRows();
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
        if (at >= int(selectable.size()))
            at = 0;
        if (KeyPressed({ ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown }))
            at = (at + 1) % int(selectable.size()), g_inst.mouseMode = false;
        if (KeyPressed({ ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp }))
            at = (at + int(selectable.size()) - 1) % int(selectable.size()), g_inst.mouseMode = false;
        g_inst.selected = selectable[at];
        if (KeyPressed({ ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown, ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
                         ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight }, false))
            ActivateRow(rows[g_inst.selected].id, +1);
        if (KeyPressed({ ImGuiKey_GamepadStart, ImGuiKey_F5 }, false) && CanInstall())
            StartInstall();
        if (KeyPressed({ ImGuiKey_Escape, ImGuiKey_GamepadFaceRight }, false))
            g_quit = true;
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
        if (row.enabled && hovered && g_inst.mouseMode)
            g_inst.selected = int(i);
        bool selected = row.enabled && g_inst.selected == int(i);
        const Style& style = !row.enabled ? INFO : selected ? SELECTED : NORMAL;
        Draw(0, T(row.label), LABEL_RIGHT, vy, ROW_SIZE, style, 1.0f, LABEL_RIGHT - LABEL_LEFT);
        const Style& valueStyle = row.id == RowGameStatus ? (paths::IsGameFolder(GameFolder()) ? SELECTED : WARNING)
                                  : row.id == RowPrereqs && MissingCount() && g_inst.checked ? WARNING
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

void DrawProgress()
{
    std::lock_guard lock(g_inst.mutex);
    float cx = (PANEL_L + PANEL_R) * 0.5f;
    float y = ROWS_T - 6.0f;
    for (int i = 0; i < INSTALL_STEP_COUNT; i++)
    {
        int number = i + 1;
        bool done = number < g_inst.step || g_inst.phase == Phase::Done;
        bool current = number == g_inst.step && g_inst.phase != Phase::Done;
        const Style& style = done ? SELECTED : current ? (g_inst.phase == Phase::Failed ? WARNING : TITLE) : INFO;
        Draw(0, T(INSTALL_STEPS[i].name), LABEL_RIGHT, y, 26.0f, style, 1.0f, LABEL_RIGHT - LABEL_LEFT + 60.0f);
        if (done)
            Draw(0, "OK", VALUE_X, y, 26.0f, SELECTED);
        else if (current && g_inst.phase == Phase::Running)
        {
            if (g_inst.progress >= 0.0f)
            {
                DrawStars(VALUE_X, y - 2.0f, int(g_inst.progress * 100.0f), 100, TITLE);
                Draw(0, std::to_string(int(g_inst.progress * 100.0f)) + "%", VALUE_R, y, 24.0f, TITLE, 1.0f);
            }
            else
            {
                // An indeterminate run of stars.
                int lit = int(ImGui::GetTime() * 4.0) % (STARS + 1);
                DrawStars(VALUE_X, y - 2.0f, lit, STARS, TITLE);
            }
        }
        else if (current && g_inst.phase == Phase::Failed)
            Draw(0, T({ "FAILED", "FALHOU" }), VALUE_X, y, 26.0f, WARNING);
        y += 34.0f;
    }
    // The current detail (download, step detail or the error) and the end of the log, in the Windows font.
    float maxW = PANEL_R - PANEL_L - 120.0f;
    std::string detail = g_inst.phase == Phase::Failed ? g_inst.error : !g_inst.download.empty() ? g_inst.download : g_inst.stepDetail;
    g_draw->AddLine(g_view.P(PANEL_L + 90.0f, y + 2.0f), g_view.P(PANEL_R - 90.0f, y + 2.0f), IM_COL32(236, 222, 190, 70), 2.0f * g_view.s);
    DrawUi(detail, cx, y + 10.0f, 19.0f, g_inst.phase == Phase::Failed ? IM_COL32(255, 140, 100, 255) : IM_COL32(255, 236, 160, 255), 0.5f, maxW);
    float ly = y + 38.0f;
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
    if (g_inst.logFile)
        fclose(g_inst.logFile), g_inst.logFile = nullptr;
}

void DrawInstallerContents()
{
    ImGuiIO& io = ImGui::GetIO();
    float s = std::min(io.DisplaySize.x / VW, io.DisplaySize.y / VH);
    g_view = { s, (io.DisplaySize.x - VW * s) * 0.5f, (io.DisplaySize.y - VH * s) * 0.5f };
    UpdateInstaller();
    if (g_inst.autoStart && g_inst.phase == Phase::Setup && CanInstall())
        StartInstall();
    if (g_inst.autoStart && g_inst.phase == Phase::Setup && g_inst.checked && !g_inst.checking && !CanInstall())
        g_inst.autoStart = false;  // nothing to wait for: missing game folder or prerequisites

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
        DrawSetupRows();
        break;
    case Phase::Running: DrawPanelTitle(T({ "INSTALLING...", "INSTALANDO..." })); DrawProgress(); break;
    case Phase::Done: DrawPanelTitle(T({ "INSTALLED", "INSTALADO" })); DrawProgress(); break;
    case Phase::Failed: DrawPanelTitle(T({ "SOMETHING WENT WRONG", "ALGO DEU ERRADO" })); DrawProgress(); break;
    }
    if (g_modal != Modal::None)
    {
        g_draw = ImGui::GetForegroundDrawList();
        DrawModal();
    }
    ImGui::End();
}

// Closing the window while setup.py runs stops it (the job object kills cmake / ninja too).
void InstallerExit()
{
    if (g_inst.phase == Phase::Running)
        CancelInstall();
    g_quit = true;
}

void InitInstaller()
{
    LANGID ui = GetUserDefaultUILanguage();
    g_values.language = PRIMARYLANGID(ui) == LANG_PORTUGUESE ? "pt-br" : "english";
    CheckPrerequisites();
}

void ShutdownInstaller()
{
    CancelInstall();
    if (g_inst.reader.joinable())
        g_inst.reader.join();
    if (g_inst.logFile)
        fclose(g_inst.logFile);
}
