// See paths.h.

#include "paths.h"
#include "diagnostics.h"
#include "settings/settings.h"

#include <windows.h>
#include <shobjidl.h>

#include <cstring>

namespace fs = std::filesystem;

namespace paths
{
    namespace
    {
        fs::path g_exeDir, g_game, g_mods, g_userData, g_languages, g_data, g_xex, g_image;
        bool g_chosenNow = false;

        fs::path FindExeDir()
        {
            wchar_t path[MAX_PATH];
            DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
            return fs::path(std::wstring(path, length)).parent_path();
        }

        bool IsDir(const fs::path& path)
        {
            std::error_code error;
            return fs::is_directory(path, error);
        }

        // Command-line paths are in the ANSI code page (main's argv).
        fs::path Argument(const char* text)
        {
            return fs::absolute(fs::path(text)).make_preferred();
        }

        // Folder picker (first run without a game folder). Empty if cancelled.
        fs::path PickFolder(HWND owner)
        {
            fs::path chosen;
            HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            IFileOpenDialog* dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            {
                DWORD options = 0;
                dialog->GetOptions(&options);
                dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                dialog->SetTitle(L"CrashRecomp: escolha a pasta do jogo (com default.xex e default.rcf)");
                while (dialog->Show(owner) == S_OK)
                {
                    IShellItem* item = nullptr;
                    PWSTR name = nullptr;
                    if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)))
                    {
                        fs::path folder(name);
                        CoTaskMemFree(name);
                        item->Release();
                        if (IsGameFolder(folder))
                        {
                            chosen = folder;
                            break;
                        }
                        MessageBoxW(owner, L"Essa pasta não tem default.xex e default.rcf.\n\n"
                                    L"Escolha a pasta com os arquivos do seu Crash of the Titans (Xbox 360).",
                                    L"CrashRecomp", MB_OK | MB_ICONWARNING);
                        continue;
                    }
                    if (item)
                        item->Release();
                }
                dialog->Release();
            }
            if (SUCCEEDED(init))
                CoUninitialize();
            return chosen;
        }
    }

    fs::path PickGameFolder(void* owner)
    {
        return PickFolder(static_cast<HWND>(owner));
    }

    bool IsGameFolder(const fs::path& folder)
    {
        std::error_code error;
        return fs::is_regular_file(folder / "default.xex", error) && fs::is_regular_file(folder / "default.rcf", error);
    }

    bool Init(int argc, char** argv, bool askForGame)
    {
        g_exeDir = FindExeDir();

        // Development build: build\CrashRecomp.exe inside the project (see paths.h).
        fs::path root = g_exeDir;
        fs::path player = g_exeDir;
        fs::path data = g_exeDir / "data";
        fs::path project = g_exeDir.parent_path();
        bool dev = !IsDir(g_exeDir / "languages") && fs::exists(project / "CMakeLists.txt") && IsDir(project / "runtime");
        if (dev)
        {
            root = project;
            player = IsDir(project / "dist") ? project / "dist" : project;
            data = project / "private";
        }
        g_mods = player / "mods";
        g_userData = player / "userdata";
        g_languages = root / "languages";
        g_data = data;

        for (int i = 1; i + 1 < argc; i += 2)
        {
            if (strcmp(argv[i], "--xex") == 0)
                g_xex = Argument(argv[i + 1]);
            else if (strcmp(argv[i], "--image") == 0)
                g_image = Argument(argv[i + 1]);
            else if (strcmp(argv[i], "--game") == 0)
                g_game = Argument(argv[i + 1]);
            else if (strcmp(argv[i], "--mods") == 0)
                g_mods = Argument(argv[i + 1]);
            else if (strcmp(argv[i], "--userdata") == 0)
                g_userData = Argument(argv[i + 1]);
            else if (strcmp(argv[i], "--languages") == 0)
                g_languages = Argument(argv[i + 1]);
            else if (strcmp(argv[i], "--data") == 0)
                g_data = Argument(argv[i + 1]);
        }

        const char* gameSource = "--game";
        if (g_game.empty() && IsGameFolder(player / "game"))
        {
            g_game = player / "game";
            gameSource = "game\\ next to the exe";
        }
        if (g_game.empty())
        {
            std::string saved = settings::ReadGameFolder(g_userData);
            if (!saved.empty())
            {
                g_game = fs::u8path(saved).make_preferred();
                gameSource = "settings.toml [paths] game";
                if (!IsGameFolder(g_game))
                {
                    Log("[paths] %s (settings.toml) has no default.xex / default.rcf", g_game.string().c_str());
                    g_game.clear();
                }
            }
        }
        if (g_game.empty() && !askForGame)
        {
            Log("[paths] no game folder yet");
            Log("[paths] exe %s, mods %s, userdata %s, languages %s, data %s", g_exeDir.string().c_str(), g_mods.string().c_str(),
                g_userData.string().c_str(), g_languages.string().c_str(), g_data.string().c_str());
            return true;
        }
        if (g_game.empty())
        {
            Log("[paths] no game folder: asking for it");
            g_game = PickGameFolder();
            gameSource = "chosen now (saved to settings.toml)";
            g_chosenNow = !g_game.empty();
        }
        if (g_game.empty())
        {
            Log("[paths] no game folder: give --game <folder>, put the game in %s or set [paths] game in %s",
                (player / "game").string().c_str(), (g_userData / "settings.toml").string().c_str());
            MessageBoxW(nullptr, L"Sem a pasta do jogo não dá para abrir o CrashRecomp.\n\n"
                        L"Coloque os arquivos do jogo (default.xex, default.rcf...) na pasta game\\ ao lado do "
                        L"CrashRecomp.exe, ou abra de novo e escolha a pasta.", L"CrashRecomp", MB_OK | MB_ICONERROR);
            return false;
        }
        if (g_xex.empty())
            g_xex = g_game / "default.xex";

        Log("[paths] exe %s%s", g_exeDir.string().c_str(), dev ? " (development build: data from the project)" : "");
        Log("[paths] game %s (%s)", g_game.string().c_str(), gameSource);
        Log("[paths] mods %s, userdata %s", g_mods.string().c_str(), g_userData.string().c_str());
        Log("[paths] languages %s, data %s%s", g_languages.string().c_str(), g_data.string().c_str(),
            IsDir(g_data) ? "" : " (missing: no subtitles text, symbols or HD AI Remaster)");
        return true;
    }

    const fs::path& ExeDir() { return g_exeDir; }
    const fs::path& Game() { return g_game; }
    const fs::path& Mods() { return g_mods; }
    const fs::path& UserData() { return g_userData; }
    const fs::path& Languages() { return g_languages; }
    const fs::path& Data() { return g_data; }
    const fs::path& Xex() { return g_xex; }
    const fs::path& Image() { return g_image; }
    bool GameChosenNow() { return g_chosenNow; }
}
