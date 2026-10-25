// UPDATE, in the left menu of an install made by the single-file installer (CrashSetup-<tag>.exe
// writes its tag to .crashrecomp_version next to CrashLauncher.exe; a clone's dist\ has none, and
// no UPDATE). At startup a thread asks GitHub for the latest release; UPDATE then downloads only
// its CrashSetup-<tag>.exe to %TEMP%\CrashRecomp-update, checks it against the release's
// SHA256SUMS.txt and starts it on this folder with --autostart and the extras installed now
// (installer.inc). The launcher closes and the installer installs over this version: the saves,
// settings, mods and the copy of the game in game\ stay, the new version is built from that copy,
// and its last screen has PLAY.
// Included inside launcher.cpp's anonymous namespace (not in CRASH_INSTALLER).

#ifdef _WIN32
const wchar_t* LATEST_RELEASE = L"https://api.github.com/repos/VitoMiguelF/crashott-pc/releases/latest";
const wchar_t* SHORTCUT_NAME = L"Crash of the Titans - Definitive Edition.lnk";  // installer.inc's

enum class UpdateState { Idle, Checking, UpToDate, Available, CheckFailed, Downloading, Downloaded, DownloadFailed, Started };

struct Updater
{
    std::string installed;  // this install's tag; "" = not made by the installer (no UPDATE)
    std::atomic<UpdateState> state{ UpdateState::Idle };
    // Written by the thread before it sets `state`.
    std::string latest, setupUrl, sumsUrl, error;
    fs::path setup;  // the downloaded installer
    std::atomic<uint64_t> done{ 0 }, total{ 0 };  // bytes of the download
    std::atomic<bool> cancel{ false };
    std::thread thread;
};
Updater g_update;

std::string NetworkError(DWORD code)
{
    switch (code)
    {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR: return T({ "no connection to GitHub (internet?)", "sem conexão com o GitHub (internet?)" });
    case ERROR_WINHTTP_TIMEOUT: return T({ "GitHub did not answer in time", "o GitHub não respondeu a tempo" });
    default: return T({ "network error ", "erro de rede " }) + std::to_string(code);
    }
}

// GET of an https:// address (WinHTTP follows GitHub's redirect to its download host): into `body`
// (GitHub's JSON, at most 8 MB), or into `file`, counting g_update.done / total. Returns "" or what
// went wrong.
std::string HttpGet(const std::wstring& url, std::string* body, HANDLE file = INVALID_HANDLE_VALUE)
{
    wchar_t host[256], path[2048];
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host, parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path, parts.dwUrlPathLength = DWORD(std::size(path));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return "not an https:// address: " + Narrow(url);
    HINTERNET session = WinHttpOpen(L"CrashRecomp-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    std::string error;
    if (!request)
        error = NetworkError(GetLastError());
    else
    {
        WinHttpSetTimeouts(request, 15000, 15000, 15000, 30000);
        const wchar_t* headers = body ? L"Accept: application/vnd.github+json" : WINHTTP_NO_ADDITIONAL_HEADERS;
        DWORD status = 0, length = 0, size = sizeof(status);
        if (!WinHttpSendRequest(request, headers, body ? DWORD(-1) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr))
            error = NetworkError(GetLastError());
        else if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                      WINHTTP_NO_HEADER_INDEX) ||
                 status != 200)
            error = T({ "GitHub answered HTTP ", "o GitHub respondeu HTTP " }) + std::to_string(status);
        else
        {
            size = sizeof(length);
            if (!body && WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &length,
                                             &size, WINHTTP_NO_HEADER_INDEX))
                g_update.total = length;
            std::vector<char> buffer(1 << 16);
            for (DWORD got = 0; error.empty();)
            {
                if (g_update.cancel)
                    error = T({ "cancelled", "cancelado" });
                else if (!WinHttpReadData(request, buffer.data(), DWORD(buffer.size()), &got))
                    error = NetworkError(GetLastError());
                else if (got == 0)
                    break;
                else if (body)
                {
                    if (body->size() + got > (8u << 20))
                        error = "GitHub's answer is too big";
                    body->append(buffer.data(), got);
                }
                else
                {
                    DWORD wrote = 0;
                    if (!WriteFile(file, buffer.data(), got, &wrote, nullptr) || wrote != got)
                        error = T({ "cannot write the download (disk full?)", "não dá para gravar o download (disco cheio?)" });
                    g_update.done += got;
                }
            }
        }
    }
    for (HINTERNET h : { request, connect, session })
        if (h)
            WinHttpCloseHandle(h);
    return error;
}

// The string value of the first "key" at or after `from` in GitHub's JSON ("" if none); `end` is
// where the search may go on.
std::string JsonString(const std::string& json, const std::string& key, size_t from, size_t* end = nullptr)
{
    std::string quoted = "\"" + key + "\"";
    for (size_t at = json.find(quoted, from); at != std::string::npos; at = json.find(quoted, at + 1))
    {
        size_t i = at + quoted.size();
        while (i < json.size() && isspace(uint8_t(json[i])))
            i++;
        if (i >= json.size() || json[i] != ':')
            continue;
        for (i++; i < json.size() && isspace(uint8_t(json[i]));)
            i++;
        if (i >= json.size() || json[i] != '"')
            continue;
        std::string value;
        for (i++; i < json.size() && json[i] != '"'; i++)
        {
            if (json[i] == '\\' && i + 1 < json.size())
                i++;  // \" \\ \/ (tags and download addresses have no other escapes)
            value += json[i];
        }
        if (end)
            *end = i;
        return value;
    }
    return {};
}

// v1.05 < v1.06 < v1.10: the numbers of the tag, in order.
std::vector<int> TagNumbers(const std::string& tag)
{
    std::vector<int> numbers;
    for (size_t i = 0; i < tag.size();)
    {
        if (!isdigit(uint8_t(tag[i])))
        {
            i++;
            continue;
        }
        numbers.push_back(atoi(tag.c_str() + i));
        while (i < tag.size() && isdigit(uint8_t(tag[i])))
            i++;
    }
    return numbers;
}

bool Newer(const std::string& latest, const std::string& installed)
{
    std::vector<int> a = TagNumbers(latest), b = TagNumbers(installed);
    return a.empty() || b.empty() ? latest != installed : a > b;
}

void CheckForUpdate()
{
    std::string json;
    std::string error = HttpGet(LATEST_RELEASE, &json);
    if (error.empty())
    {
        g_update.latest = JsonString(json, "tag_name", 0);
        if (g_update.latest.empty())
            error = T({ "GitHub's answer has no release", "a resposta do GitHub não tem release" });
        // The release's files: CrashSetup-<tag>.exe and SHA256SUMS.txt.
        size_t at = 0;
        for (std::string url; !(url = JsonString(json, "browser_download_url", at, &at)).empty();)
        {
            std::string name = url.substr(url.rfind('/') + 1);
            if (name == "CrashSetup-" + g_update.latest + ".exe")
                g_update.setupUrl = url;
            else if (name == "SHA256SUMS.txt")
                g_update.sumsUrl = url;
        }
    }
    if (error.empty() && Newer(g_update.latest, g_update.installed) && (g_update.setupUrl.empty() || g_update.sumsUrl.empty()))
        error = g_update.latest + T({ " has no installer (CrashSetup-", " não tem instalador (CrashSetup-" }) + g_update.latest + ".exe)";
    g_update.error = error;
    Log("[launcher] update: installed %s, latest %s%s%s", g_update.installed.c_str(), g_update.latest.empty() ? "?" : g_update.latest.c_str(),
        error.empty() ? "" : ": ", error.c_str());
    g_update.state = !error.empty() ? UpdateState::CheckFailed : Newer(g_update.latest, g_update.installed) ? UpdateState::Available : UpdateState::UpToDate;
}

void DownloadUpdate()
{
    std::string sums, error = HttpGet(Widen(g_update.sumsUrl), &sums);
    std::string name = "CrashSetup-" + g_update.latest + ".exe", expected;
    for (size_t at = 0; error.empty() && at < sums.size();)
    {
        size_t end = sums.find('\n', at);
        std::string line = sums.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? sums.size() : end + 1;
        // "<sha256>  <name>" (sha256sum's format; "*" before the name in binary mode).
        size_t space = line.find(' ');
        std::string file = space == std::string::npos ? "" : line.substr(line.find_first_not_of(" *", space));
        while (!file.empty() && isspace(uint8_t(file.back())))
            file.pop_back();
        if (file == name)
            expected = line.substr(0, space);
    }
    std::transform(expected.begin(), expected.end(), expected.begin(), [](char c) { return char(tolower(uint8_t(c))); });
    if (error.empty() && expected.size() != 64)
        error = name + T({ " is not in the release's SHA256SUMS.txt", " não está no SHA256SUMS.txt da release" });
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    fs::path dir = fs::path(temp) / L"CrashRecomp-update", target = dir / Widen(name), part = target;
    part += L".part";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (error.empty())
    {
        HANDLE file = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            error = T({ "cannot write in ", "não dá para gravar em " }) + Narrow(dir.wstring());
        else
        {
            error = HttpGet(Widen(g_update.setupUrl), nullptr, file);
            CloseHandle(file);
        }
    }
    if (error.empty() && Sha256File(part) != expected)
        error = T({ "the download is damaged (SHA-256 differs): try again", "o download veio com defeito (SHA-256 diferente): tente de novo" });
    if (error.empty() && !MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        error = T({ "cannot write ", "não dá para gravar " }) + Narrow(target.wstring());
    if (!error.empty())
        fs::remove(part, ec);
    g_update.setup = target;
    g_update.error = error;
    Log("[launcher] update download %s: %s", name.c_str(), error.empty() ? "ok" : error.c_str());
    g_update.state = error.empty() ? UpdateState::Downloaded : UpdateState::DownloadFailed;
}

void RunUpdateThread(void (*work)(), UpdateState state)
{
    if (g_update.thread.joinable())
        g_update.thread.join();  // the last one has set its state: it is ending
    g_update.cancel = false;
    g_update.done = g_update.total = 0;
    g_update.state = state;
    g_update.thread = std::thread(work);
}

// At startup: this install's tag, and GitHub's latest release on a thread.
void StartUpdateCheck()
{
    if (FILE* f = _wfopen((paths::ExeDir() / VERSION_FILE).c_str(), L"rb"))
    {
        char line[64] = {};
        if (fgets(line, sizeof(line), f))
            g_update.installed = line;
        fclose(f);
        while (!g_update.installed.empty() && isspace(uint8_t(g_update.installed.back())))
            g_update.installed.pop_back();
    }
    if (!g_update.installed.empty())
        RunUpdateThread(CheckForUpdate, UpdateState::Checking);
}

void StopUpdater()
{
    g_update.cancel = true;
    if (g_update.thread.joinable())
        g_update.thread.detach();  // a request may wait for its timeout: the process ends anyway
}

bool UpdateOffered() { return !g_update.installed.empty(); }

// UPDATE in the menu: the window with the state (a check that failed is tried again).
void OpenUpdate()
{
    if (g_update.state == UpdateState::CheckFailed || g_update.state == UpdateState::Idle)
        RunUpdateThread(CheckForUpdate, UpdateState::Checking);
    else if (g_update.state == UpdateState::DownloadFailed)
        g_update.state = UpdateState::Available;
    g_modal = Modal::Update;
}

// The downloaded installer, on this install folder, with the extras this install has; then the
// launcher closes (the installer replaces its exe).
void StartInstaller()
{
    if (!(g_values == g_saved))
        Save();
    std::wstring command = L"\"" + g_update.setup.wstring() + L"\" --install-dir \"" + paths::ExeDir().wstring() + L"\" --autostart";
    command += Exists(paths::Data() / "subtitles" / "english.txt") ? L" --option subtitles" : L" --option no-subtitles";
    if (settings::HdRemasterAvailable())
        command += L" --option remaster";
    // The shortcuts the player kept (the installer makes both by default).
    auto shortcut = [](REFKNOWNFOLDERID id, const wchar_t* folder) {
        PWSTR path = nullptr;
        bool have = SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && Exists(fs::path(path) / folder / SHORTCUT_NAME);
        CoTaskMemFree(path);
        return have;
    };
    if (!shortcut(FOLDERID_Desktop, L""))
        command += L" --option no-desktop";
    if (!shortcut(FOLDERID_Programs, L"CrashRecomp"))
        command += L" --option no-startmenu";
    SetEnvironmentVariableW(L"CRASH_SETUP_LANGUAGE", Pt() ? L"pt" : L"en");  // the installer speaks the launcher's language
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    fs::path dir = g_update.setup.parent_path();
    if (!CreateProcessW(g_update.setup.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
    {
        g_update.error = T({ "cannot start ", "não dá para abrir " }) + Narrow(g_update.setup.wstring());
        g_update.state = UpdateState::DownloadFailed;
        return;
    }
    Log("[launcher] update: started %s", Narrow(command).c_str());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_update.state = UpdateState::Started;
    g_quit = true;
}

// Each frame: the download finished -> the installer.
void PollUpdate()
{
    if (g_update.state == UpdateState::Downloaded)
        StartInstaller();
}

// The menu item: UPDATE, or UPDATE (v1.06) in gold when there is one.
std::string UpdateLabel(bool& available)
{
    available = g_update.state == UpdateState::Available || g_update.state == UpdateState::DownloadFailed;
    std::string text = T({ "UPDATE", "ATUALIZAR" });
    return available ? text + " (" + g_update.latest + ")" : text;
}

// A row of (icon) TEXT buttons centred on `cx`; the index of the one clicked, or -1.
int DrawButtons(std::initializer_list<std::pair<char32_t, Text>> buttons, float cx, float y)
{
    float total = 0.0f;
    for (const auto& [icon, text] : buttons)
        total += TextWidth(1, IconText(icon), 32.0f) + 8.0f + TextWidth(0, T(text), 30.0f) + 40.0f;
    float x = cx - (total - 40.0f) * 0.5f;
    int clicked = -1, i = 0;
    for (const auto& [icon, text] : buttons)
    {
        float x0 = x;
        x = Icon(icon, x, y, 32.0f) + 8.0f;
        x = Draw(0, T(text), x, y + 2.0f, 30.0f, i == 0 ? SELECTED : NORMAL);
        if (Clicked(x0 - 10.0f, y - 6.0f, x + 10.0f, y + 40.0f))
            clicked = i;
        x += 40.0f, i++;
    }
    return clicked;
}

void DrawUpdateModal(float l, float t, float r, float cx)
{
    bool ok = KeyPressed({ ImGuiKey_GamepadFaceDown, ImGuiKey_Enter, ImGuiKey_KeypadEnter }, false);
    bool back = KeyPressed({ ImGuiKey_GamepadFaceRight, ImGuiKey_Escape, ImGuiKey_Backspace }, false);
    std::string line1, line2;
    float width = r - l - 80.0f, buttonsY = t + 150.0f;
    UpdateState state = g_update.state;
    switch (state)
    {
    case UpdateState::Idle:
    case UpdateState::Checking:
        line1 = T({ "Looking for a new version...", "Procurando uma versão nova..." });
        line2 = T({ "This one: ", "Esta: " }) + g_update.installed;
        break;
    case UpdateState::UpToDate:
        line1 = T({ "You have the latest version", "Você já tem a versão mais nova" });
        line2 = "(" + g_update.installed + ").";
        break;
    case UpdateState::Available:
        line1 = T({ "Version ", "A versão " }) + g_update.latest + T({ " is out (you have ", " saiu (você tem a " }) + g_update.installed + ").";
        line2 = T({ "Saves, options and mods stay.", "Saves, opções e mods continuam." });
        break;
    case UpdateState::CheckFailed:
    case UpdateState::DownloadFailed:
        line1 = state == UpdateState::CheckFailed ? T({ "Could not look for updates:", "Não deu para procurar atualizações:" })
                                                  : T({ "Could not download the update:", "Não deu para baixar a atualização:" });
        line2 = g_update.error;
        break;
    case UpdateState::Downloading:
    {
        uint64_t done = g_update.done, total = g_update.total;
        line1 = T({ "Downloading ", "Baixando " }) + g_update.latest + "...";
        char text[64];
        snprintf(text, sizeof(text), total ? "%.0f / %.0f MB" : "%.0f MB", double(done) / 1048576.0, double(total) / 1048576.0);
        line2 = text;
        // The bar, under the lines.
        float bx0 = l + 90.0f, bx1 = r - 90.0f, by = t + 128.0f, k = total ? std::min(1.0f, float(double(done) / double(total))) : 0.0f;
        g_draw->AddRectFilled(g_view.P(bx0, by), g_view.P(bx1, by + 10.0f), IM_COL32(84, 42, 2, 200), 5.0f * g_view.s);
        if (k > 0.0f)
            g_draw->AddRectFilled(g_view.P(bx0, by), g_view.P(bx0 + (bx1 - bx0) * k, by + 10.0f), SELECTED.fill, 5.0f * g_view.s);
        buttonsY = t + 160.0f;
        break;
    }
    case UpdateState::Downloaded:
    case UpdateState::Started:
        line1 = T({ "Starting the installer...", "Abrindo o instalador..." });
        break;
    }
    Draw(0, line1, cx, t + 46.0f, 30.0f, HELP, 0.5f, width);
    Draw(0, line2, cx, t + 90.0f, 28.0f, HELP, 0.5f, width);
    int clicked = -1;
    switch (state)
    {
    case UpdateState::Available:
        clicked = DrawButtons({ { ma::ICON_A, { "UPDATE", "ATUALIZAR" } }, { ma::ICON_B, { "LATER", "DEPOIS" } } }, cx, buttonsY);
        if (clicked == 0 || ok)
            RunUpdateThread(DownloadUpdate, UpdateState::Downloading);
        else if (clicked == 1 || back)
            g_modal = Modal::None;
        break;
    case UpdateState::Idle:
    case UpdateState::Checking:
    case UpdateState::Downloading:
        clicked = DrawButtons({ { ma::ICON_B, { state == UpdateState::Downloading ? Text{ "CANCEL", "CANCELAR" } : Text{ "BACK", "VOLTAR" } } } }, cx, buttonsY);
        if (clicked == 0 || back)
        {
            if (state == UpdateState::Downloading)
                g_update.cancel = true;  // the thread ends as DownloadFailed ("cancelled")
            g_modal = Modal::None;
        }
        break;
    case UpdateState::Downloaded:
    case UpdateState::Started:
        break;
    default:
        clicked = DrawButtons({ { ma::ICON_A, { "OK", "OK" } } }, cx, buttonsY);
        if (clicked == 0 || ok || back)
            g_modal = Modal::None;
        break;
    }
}
#else
// Linux: no UPDATE yet (it would download the next CrashSetup-<tag>-x86_64.AppImage and run it).
void StartUpdateCheck() {}
void StopUpdater() {}
bool UpdateOffered() { return false; }
void OpenUpdate() {}
void PollUpdate() {}
std::string UpdateLabel(bool& available)
{
    available = false;
    return T({ "UPDATE", "ATUALIZAR" });
}
void DrawUpdateModal(float, float, float, float) { g_modal = Modal::None; }
#endif
