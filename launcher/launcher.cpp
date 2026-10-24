// CrashLauncher: edits userdata\settings.toml before the game starts, with the game's own look
// (Dear ImGui on Direct3D 11), then starts CrashRecomp.exe.
//
// Nothing of the game is in the launcher: like the settings screen of the runtime it reads the
// fonts (Titans_Large / Titans_Small, with the button icons), the message frame, the stars and the
// main menu's backdrop and logo from the user's default.rcf at startup (runtime/settings/menu_art.h),
// and the options through the runtime's own settings code (settings.h, controls.h), so the values
// and limits are the game's. The folders come from runtime/paths.h, as in the game. Without the
// game folder it still opens (plain backdrop, Windows font) so the folder can be chosen in SETUP.
//
//   CrashLauncher.exe [--game ...] [--userdata ...] [...]   the same folder arguments as the game,
//                                                          passed on to CrashRecomp.exe
//   CrashLauncher.exe --screenshot <file.png> [--tab N] [--view simple|advanced]
//                                                          draws one frame to a PNG and exits (tests)

// windows.h first: menu_art.cpp is compiled with it, so DrawText is its DrawTextA macro there too.
#include <windows.h>

#include "../runtime/fs/vfs.h"
#include "../runtime/gpu/png_writer.h"
#include "../runtime/paths.h"
#include "../runtime/settings/controls.h"
#include "../runtime/settings/language_pack.h"
#include "../runtime/settings/menu_art.h"
#include "../runtime/settings/options.h"
#include "../runtime/settings/settings.h"
#include "../runtime/diagnostics.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef CRASH_INSTALLER
#include <mutex>
#include <thread>
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;
namespace ma = settings::menu_art;
using settings::Action;
using settings::Values;

namespace
{
    // Layout in virtual 1280x720 units, scaled to the window (centered, aspect kept).
    constexpr float VW = 1280.0f, VH = 720.0f;
    // The tabs and the button hints are on the cloth, the rows and the help in a message box.
    constexpr float PANEL_L = 462.0f, PANEL_R = 1256.0f, PANEL_T = 84.0f, PANEL_B = 642.0f, CORNER = 96.0f;
    constexpr float TABS_Y = 26.0f, TAB_SIZE = 32.0f;
    constexpr float ROWS_T = 112.0f, ROWS_B = 548.0f, ROW_PITCH = 44.0f, ROW_SIZE = 30.0f;
    constexpr float LABEL_LEFT = 528.0f, LABEL_RIGHT = 840.0f, VALUE_X = 866.0f, VALUE_R = 1192.0f;
    constexpr float HELP_Y = 560.0f, HELP_SIZE = 25.0f, FOOTER_Y = 662.0f, FOOTER_SIZE = 26.0f;
    constexpr float STAR_SIZE = 24.0f, STAR_PITCH = 25.0f;
    constexpr float LEFT_CX = 236.0f;
    constexpr int STARS = 10;

    // Atlas sizes (pixels): drawn scaled to the window, so a bit bigger than at 720p.
    constexpr float LARGE_PX = 60.0f, SMALL_PX = 44.0f, UI_PX = 30.0f;

    // The settings screen's colours on the cloth (settings_menu.cpp, CLOTH): fill and outline.
    struct Style
    {
        ImU32 fill, outline;
    };
    constexpr ImU32 RGBA(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
    const Style TITLE{ RGBA(236, 150, 20), RGBA(70, 28, 0) };
    const Style NORMAL{ RGBA(214, 120, 14), RGBA(70, 28, 0) };
    const Style SELECTED{ RGBA(150, 218, 212), RGBA(30, 104, 110) };
    const Style INFO{ RGBA(150, 104, 46), RGBA(84, 42, 2, 150) };
    const Style HELP{ RGBA(238, 226, 196), RGBA(52, 28, 4) };
    const Style WARNING{ RGBA(232, 64, 24), RGBA(80, 16, 0) };
    const Style GOLD{ RGBA(255, 180, 40), RGBA(80, 30, 0) };  // "Definitive Edition" on the title screen
    const Style MENU{ RGBA(250, 160, 30), RGBA(70, 28, 0) };
    const Style MENU_SELECTED{ RGBA(255, 236, 120), RGBA(120, 50, 0) };
    constexpr ImU32 FRAME_TINT = RGBA(236, 222, 190, 230);

    // Button icons of the game's fonts, as private-use glyphs of the fill fonts (drawn in colour).
    constexpr ImWchar ICON_BASE = 0xE000;

    // ---------------------------------------------------------------------------------------------
    // Options: the SETTINGS screen's (runtime/settings/options.h: the same tabs, rows, names, values
    // and help), plus the launcher's own SETUP tab and its simple view.

    namespace opt = settings::options;
    using opt::ActionOf;
    using opt::IsBinding;
    using opt::IsHeader;
    using opt::Item;
    using opt::Text;

    struct Tab
    {
        Text name;
        std::vector<Item> items;
    };

    // ADVANCED: the game's tabs (without its STORAGE row, a note about the main menu's Y; with a
    // heading over the key bindings) and SETUP.
    std::vector<Tab> MakeTabs()
    {
        std::vector<Tab> tabs;
        for (int i = 0; i < opt::TAB_COUNT; i++)
        {
            const opt::Tab& tab = opt::Tabs()[i];
            Tab copy{ tab.name, {} };
            for (Item item : tab.items)
            {
                if (item == Item::Storage)
                    continue;
                if (IsBinding(item) && (copy.items.empty() || !IsBinding(copy.items.back())))
                    copy.items.push_back(Item::HeaderKeys);
                copy.items.push_back(item);
            }
            tabs.push_back(std::move(copy));
        }
        tabs.push_back({ { "SETUP", "INSTALAÇÃO" }, { Item::GameFolder, Item::ModsFolder, Item::SavesFolder, Item::ExtraData } });
        return tabs;
    }
    const std::vector<Tab> TABS = MakeTabs();
    const int SETUP_TAB = int(TABS.size()) - 1;
    // The simple view (the default): the main options on one page; LIGHTING sets all of the
    // lighting remaster's at once (opt::LightingPreset).
    const Tab SIMPLE{ { "OPTIONS", "OPÇÕES" }, { Item::Language, Item::Resolution, Item::FpsLimit, Item::HdRemaster, Item::Lighting } };

    // State.
    Values g_values, g_saved;
    int g_tab = 0;
    Item g_selected = Item::Language;  // the row under the cursor or with the keyboard / pad focus
    bool g_capturing = false;          // CONTROLS: waiting for a key
    uint16_t g_captured = 0;           // set by the window procedure
    bool g_captureCancelled = false;
    enum class Modal { None, Exit, Message } g_modal = Modal::None;
    std::string g_message[2];
    bool g_quit = false;
    bool g_artLoaded = false, g_haveFonts = false;
    bool g_dlss = false, g_fsr = false;
    int g_menuFocus = -1;  // left menu item under the mouse
    std::vector<std::string> g_passArgs;  // folder arguments passed on to the game
    bool g_focusFirstRow = true;    // select the first row of the tab (opening it)
    bool g_scrollToSelected = false;  // scroll the rows to show g_selected (moved by the keys)
    bool g_mouseMode = false;  // the selection follows the mouse until a key or the pad moves it
    HWND g_hwnd;

    bool Pt() { return opt::Portuguese(g_values); }
    const char* T(const Text& text) { return Pt() ? text.pt : text.en; }
    std::string OnOff(bool on) { return on ? T({ "ON", "LIGADO" }) : T({ "OFF", "DESLIGADO" }); }  // installer.inc

    bool Advanced() { return g_values.launcherAdvanced; }
    const Tab& CurrentTab() { return Advanced() ? TABS[g_tab] : SIMPLE; }

    std::string Upper(std::string text)
    {
        // ASCII and the Latin-1 letters of UTF-8 (C3 xx: lower case = upper case + 0x20).
        for (size_t i = 0; i < text.size(); i++)
        {
            unsigned char c = text[i];
            if (c >= 'a' && c <= 'z')
                text[i] = char(c - 32);
            else if (c == 0xC3 && i + 1 < text.size() && uint8_t(text[i + 1]) >= 0xA0 && uint8_t(text[i + 1]) <= 0xBE &&
                     uint8_t(text[i + 1]) != 0xB7)
            {
                ++i;
                text[i] = char(uint8_t(text[i]) - 0x20);
            }
        }
        return text;
    }

    std::string Narrow(const std::wstring& text)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    bool Exists(const fs::path& path)
    {
        std::error_code error;
        return fs::exists(path, error);
    }

    fs::path GameFolder()
    {
        if (!paths::Game().empty())
            return paths::Game();
        return g_values.gamePath.empty() ? fs::path() : fs::u8path(g_values.gamePath).make_preferred();
    }

    bool HaveSubtitles()
    {
        const settings::Language& language = settings::LanguageById(g_values.language);
        if (language.pack)
            return Exists(paths::Languages() / language.pack / "subtitles.txt");
        return language.letter == 'E' && (Exists(paths::Data() / "subtitles" / "english.txt") || Exists(paths::UserData() / "subtitles" / "english.txt"));
    }

    int ModCount()
    {
        int count = 0;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(paths::Mods(), error))
            count += entry.is_directory() ? 1 : 0;
        return count;
    }

    // What the options' texts depend on here: everything applies when the game starts.
    opt::Context OptionContext()
    {
        opt::Context context;
        context.pt = Pt();
        context.launcher = true;
        context.dlss = g_dlss;
        context.fsr = g_fsr;
        context.haveSubtitles = HaveSubtitles();
        return context;
    }

    std::string Label(Item item)
    {
        return opt::Label(item, Pt());
    }

    // Sliders: value, minimum, maximum and step (the settings screen's).
    int* Percent(Item item, int* minimum = nullptr, int* maximum = nullptr, int* step = nullptr)
    {
        return opt::Percent(item, g_values, minimum, maximum, step);
    }

    bool Selectable(Item item)
    {
        return !IsHeader(item) && item != Item::ExtraData && !(item == Item::HdRemaster && !settings::HdRemasterAvailable());
    }

    std::string Value(Item item)
    {
        if (IsBinding(item) && g_capturing && item == g_selected)
            return T({ "PRESS A KEY...", "PRESSIONE UMA TECLA..." });
        switch (item)
        {
        case Item::Language: return Upper(opt::Value(item, g_values, OptionContext()));
        case Item::GameFolder: return GameFolder().empty() ? T({ "CHOOSE...", "ESCOLHER..." }) : T({ "CHANGE...", "TROCAR..." });
        case Item::ModsFolder:
        {
            int count = ModCount();
            return (count == 1 ? std::string("1 MOD") : std::to_string(count) + " MODS") + T({ " (OPEN)", " (ABRIR)" });
        }
        case Item::SavesFolder: return T({ "OPEN FOLDER", "ABRIR PASTA" });
        case Item::ExtraData:
        {
            std::string list;
            auto add = [&](bool have, const char* name) { if (have) list += (list.empty() ? "" : ", ") + std::string(name); };
            add(Exists(paths::Data() / "subtitles" / "english.txt"), T({ "SUBTITLES", "LEGENDAS" }));
            add(Exists(paths::Data() / "symbols" / "functions.csv"), T({ "SYMBOLS", "SÍMBOLOS" }));
            add(settings::HdRemasterAvailable(), "REMASTER");
            return list.empty() ? T({ "NONE", "NENHUM" }) : list;
        }
        default: return opt::Value(item, g_values, OptionContext());
        }
    }

    // Two lines about the selected item (the settings screen's, plus the launcher's own rows).
    std::pair<std::string, std::string> Help(Item item)
    {
        if (IsBinding(item))
        {
            if (g_capturing)
                return { T({ "Press the new key or mouse button.", "Pressione a nova tecla ou botão do mouse." }), T({ "Esc cancels.", "Esc cancela." }) };
            return { T({ "A / Enter / click: choose a key (it takes over the first one).", "A / Enter / clique: escolher a tecla (substitui a primeira)." }),
                     T({ "X / Backspace: back to the default.", "X / Backspace: volta ao padrão." }) };
        }
        switch (item)
        {
        case Item::GameFolder:
        {
            fs::path game = GameFolder();
            if (game.empty())
                return { T({ "Choose the folder of your Crash of the Titans (Xbox 360):", "Escolha a pasta do seu Crash of the Titans (Xbox 360):" }),
                         T({ "the one with default.xex and default.rcf.", "a que tem default.xex e default.rcf." }) };
            return { Narrow(game.wstring()), paths::IsGameFolder(game) ? T({ "The game files are never changed.", "Os arquivos do jogo nunca são alterados." })
                                                                       : T({ "default.xex / default.rcf not found there!", "default.xex / default.rcf não estão lá!" }) };
        }
        case Item::ModsFolder: return { Narrow(paths::Mods().wstring()), T({ "One folder per mod (README, \"Mods\").", "Uma pasta por mod (README, \"Mods\")." }) };
        case Item::SavesFolder: return { Narrow(paths::UserData().wstring()), T({ "Your saves (content) and settings.toml.", "Seus saves (content) e o settings.toml." }) };
        case Item::ExtraData: return { Narrow(paths::Data().wstring()), T({ "Made from your own game; never share it.", "Feitos a partir do seu jogo; não compartilhe." }) };
        default: return opt::Help(item, g_values, OptionContext());
        }
    }

    void OpenFolder(const fs::path& folder)
    {
        std::error_code error;
        fs::create_directories(folder, error);
        ShellExecuteW(g_hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void Relaunch()
    {
        // The game's art is read at startup: start again to show it with the new game folder.
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(exe) + L"\"";
        for (const std::string& arg : g_passArgs)
        {
            int n = MultiByteToWideChar(CP_ACP, 0, arg.c_str(), -1, nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_ACP, 0, arg.c_str(), -1, w.data(), n);
            command += L" \"" + std::wstring(w.c_str()) + L"\"";
        }
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            g_quit = true;
        }
    }

    void Save()
    {
        settings::Set(g_values);
        settings::Save();
        g_saved = g_values;
    }

    // SIMPLE <-> ADVANCED, written to settings.toml at once ([launcher] view) without the other
    // changes, which stay unsaved.
    void SetAdvanced(bool advanced)
    {
        if (advanced == Advanced())
            return;
        g_values.launcherAdvanced = g_saved.launcherAdvanced = advanced;
        settings::Set(g_saved);
        settings::Save();
        g_focusFirstRow = true;
        g_capturing = false;
        Log("[launcher] view -> %s", advanced ? "advanced" : "simple");
    }

    void ChooseGameFolder()
    {
        fs::path folder = paths::PickGameFolder(g_hwnd);
        if (folder.empty())
            return;
        g_values.gamePath = reinterpret_cast<const char*>(folder.generic_u8string().c_str());
        Save();
        if (!g_artLoaded)
            Relaunch();
    }

    void Change(Item item, int direction)
    {
        opt::Change(item, g_values, direction);
    }

    // A (Enter, click): change forward, or the row's own action.
    void Activate(Item item)
    {
        if (IsBinding(item))
        {
            g_capturing = true;
            g_captured = 0;
            g_captureCancelled = false;
            return;
        }
        switch (item)
        {
        case Item::GameFolder: ChooseGameFolder(); break;
        case Item::ModsFolder: OpenFolder(paths::Mods()); break;
        case Item::SavesFolder: OpenFolder(paths::UserData()); break;
        default: Change(item, +1); break;
        }
    }

    // `key` becomes the first key of `action` and leaves the other actions (as the settings screen).
    void Bind(Action action, uint16_t key)
    {
        for (size_t a = 0; a < settings::ACTION_COUNT; a++)
        {
            auto& keys = g_values.controls.keys[a];
            if (a == size_t(action))
                continue;
            for (uint16_t& k : keys)
                if (k == key)
                    k = 0;
            if (!keys[0] && keys[1])
                keys[0] = keys[1], keys[1] = 0;
        }
        auto& keys = g_values.controls.keys[size_t(action)];
        if (keys[0] != key)
            keys[1] = keys[0], keys[0] = key;
    }

    void ResetBinding(Action action)
    {
        g_values.controls.keys[size_t(action)] = settings::Controls().keys[size_t(action)];
    }

    void Defaults()
    {
        Values defaults;
        defaults.gamePath = g_values.gamePath;
        defaults.language = g_values.language;  // the language stays: the screen follows it
        defaults.launcherAdvanced = g_values.launcherAdvanced;
        g_values = defaults;
    }

    bool StartGame()
    {
        if (!paths::IsGameFolder(GameFolder()))
        {
            g_message[0] = T({ "Choose the game folder first", "Escolha a pasta do jogo primeiro" });
            g_message[1] = T({ "(SETUP tab, GAME FOLDER).", "(aba INSTALAÇÃO, PASTA DO JOGO)." });
            g_modal = Modal::Message;
            SetAdvanced(true);
            g_tab = SETUP_TAB;
            g_focusFirstRow = true;
            return false;
        }
        if (!(g_values == g_saved))
            Save();
        fs::path exe = paths::ExeDir() / "CrashRecomp.exe";
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        for (const std::string& arg : g_passArgs)
        {
            int n = MultiByteToWideChar(CP_ACP, 0, arg.c_str(), -1, nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_ACP, 0, arg.c_str(), -1, w.data(), n);
            command += L" \"" + std::wstring(w.c_str()) + L"\"";
        }
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, paths::ExeDir().c_str(), &si, &pi))
        {
            g_message[0] = T({ "Could not start CrashRecomp.exe:", "Não foi possível abrir o CrashRecomp.exe:" });
            g_message[1] = Narrow(exe.wstring());
            g_modal = Modal::Message;
            return false;
        }
        Log("[launcher] started %s (pid %lu)", exe.string().c_str(), pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        g_quit = true;
        return true;
    }

    void RequestExit()
    {
        if (g_values == g_saved)
            g_quit = true;
        else
            g_modal = Modal::Exit;
    }

    // ---------------------------------------------------------------------------------------------
    // Direct3D 11

    ID3D11Device* g_device;
    ID3D11DeviceContext* g_context;
    IDXGISwapChain* g_swapChain;
    ID3D11RenderTargetView* g_rtv;
    UINT g_resizeW, g_resizeH;

    void CreateRenderTarget()
    {
        ID3D11Texture2D* back = nullptr;
        g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }

    void ReleaseRenderTarget()
    {
        if (g_rtv)
            g_rtv->Release(), g_rtv = nullptr;
    }

    bool CreateDevice(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL level;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                                   &g_swapChain, &g_device, &level, &g_context);
        if (hr == DXGI_ERROR_UNSUPPORTED)
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain,
                                               &g_device, &level, &g_context);
        if (FAILED(hr))
            return false;
        CreateRenderTarget();
        // DLSS needs an NVIDIA card (and its DLL); FSR 3.1 runs anywhere (with its DLLs).
        IDXGIDevice* dxgi = nullptr;
        IDXGIAdapter* adapter = nullptr;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
            adapter->GetDesc(&desc);
        if (adapter) adapter->Release();
        if (dxgi) dxgi->Release();
        g_dlss = desc.VendorId == 0x10DE && Exists(paths::ExeDir() / "nvngx_dlss.dll");
        g_fsr = Exists(paths::ExeDir() / "amd_fidelityfx_vk.dll");
        Log("[launcher] GPU %s: DLSS %s, FSR %s", Narrow(desc.Description).c_str(), g_dlss ? "yes" : "no", g_fsr ? "yes" : "no");
        return true;
    }

    // Straight-alpha RGBA8 (R in the low byte) -> texture.
    ID3D11ShaderResourceView* MakeTexture(const uint32_t* px, int w, int h)
    {
        if (!px || w <= 0 || h <= 0)
            return nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w, desc.Height = h, desc.MipLevels = 1, desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{ px, UINT(w * 4), 0 };
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        if (SUCCEEDED(g_device->CreateTexture2D(&desc, &data, &texture)))
        {
            g_device->CreateShaderResourceView(texture, nullptr, &view);
            texture->Release();
        }
        return view;
    }

    std::vector<uint32_t> Straight(const ma::Image& image)
    {
        std::vector<uint32_t> out(image.px.size());
        for (size_t i = 0; i < out.size(); i++)
        {
            uint32_t p = image.px[i], a = p >> 24;
            if (!a)
                continue;
            uint32_t r = std::min(255u, (p & 0xFF) * 255 / a), g = std::min(255u, ((p >> 8) & 0xFF) * 255 / a),
                     b = std::min(255u, ((p >> 16) & 0xFF) * 255 / a);
            out[i] = r | (g << 8) | (b << 16) | (a << 24);
        }
        return out;
    }

    struct Texture
    {
        ID3D11ShaderResourceView* view = nullptr;
        int w = 0, h = 0;
        ImTextureID Id() const { return ImTextureID(reinterpret_cast<uintptr_t>(view)); }
    };

    Texture ArtTexture(ma::Art art)
    {
        Texture t;
        if (const ma::Image* image = ma::GetArt(art))
        {
            std::vector<uint32_t> px = Straight(*image);
            t.view = MakeTexture(px.data(), image->w, image->h);
            t.w = image->w, t.h = image->h;
        }
        return t;
    }

    Texture g_logo, g_star, g_frame, g_dividerL, g_dividerR, g_backdrop;
    int g_backdropW, g_backdropH;

    // ---------------------------------------------------------------------------------------------
    // Fonts: the game's Titans fonts in two passes (outline, then fill) so each text gets the
    // game's two colours; the Windows font for anything the game's fonts lack and for paths.

    ImFont* g_fill[2];
    ImFont* g_outline[2];
    ImFont* g_ui;

    struct PendingGlyph
    {
        int rect;
        std::vector<uint32_t> px;  // straight RGBA, rect-sized
    };

    void AddGameGlyphs(ImFontAtlas* atlas, ma::FontId id, float px, std::vector<PendingGlyph>& pending)
    {
        ImFont* fill = g_fill[int(id)];
        ImFont* outline = g_outline[int(id)];
        const int pad = 3;
        auto render = [&](char32_t c, bool icon, float& advance, int& w, int& h, std::vector<uint32_t>& rgba) {
            ma::Canvas canvas;
            float width = icon ? ma::IconWidth(c, px * 0.92f) : ma::TextWidth(id, std::u32string(1, c), px);
            if (width <= 0.0f)
                return false;
            w = int(std::ceil(width)) + pad * 2, h = int(std::ceil(px)) + pad * 2;
            canvas.Reset(w, h);
            if (icon)
                ma::DrawIcon(canvas, c, float(pad), pad + px * 0.04f, px * 0.92f);
            else
                ma::DrawText(canvas, id, std::u32string(1, c), float(pad), float(pad), px, { { 255, 255, 255, 255 }, { 0, 0, 0, 255 } });
            canvas.Export(rgba);
            advance = width;
            return true;
        };
        std::vector<char32_t> codes;
        for (char32_t c = 0x21; c < 0x7F; c++)
            codes.push_back(c);
        for (char32_t c = 0xC0; c <= 0xFF; c++)
            codes.push_back(c);
        for (char32_t c : codes)
        {
            if (!ma::HasGlyph(id, c))
                continue;
            float advance;
            int w, h;
            std::vector<uint32_t> rgba;
            if (!render(c, false, advance, w, h, rgba))
                continue;
            // Outline pass: the whole glyph; fill pass: only its bright (fill) part.
            std::vector<uint32_t> outlinePx(rgba.size()), fillPx(rgba.size());
            for (size_t i = 0; i < rgba.size(); i++)
            {
                uint32_t p = rgba[i], a = p >> 24, l = p & 0xFF;
                outlinePx[i] = 0x00FFFFFFu | (a << 24);
                fillPx[i] = 0x00FFFFFFu | ((a * l / 255) << 24);
            }
            pending.push_back({ atlas->AddCustomRectFontGlyph(outline, ImWchar(c), w, h, advance, ImVec2(-pad, -pad)), std::move(outlinePx) });
            pending.push_back({ atlas->AddCustomRectFontGlyph(fill, ImWchar(c), w, h, advance, ImVec2(-pad, -pad)), std::move(fillPx) });
        }
        // Button icons (in colour, drawn with white).
        for (char32_t icon = ma::ICON_A; icon <= ma::ICON_LB; icon++)
        {
            float advance;
            int w, h;
            std::vector<uint32_t> rgba;
            if (!render(icon, true, advance, w, h, rgba))
                continue;
            pending.push_back({ atlas->AddCustomRectFontGlyph(fill, ImWchar(ICON_BASE + icon), w, h, advance, ImVec2(-pad, -pad)), std::move(rgba) });
            pending.push_back({ atlas->AddCustomRectFontGlyph(outline, ImWchar(ICON_BASE + icon), w, h, advance, ImVec2(-pad, -pad)),
                                std::vector<uint32_t>(size_t(w) * h, 0) });
        }
    }

    void BuildFonts()
    {
        ImGuiIO& io = ImGui::GetIO();
        ImFontAtlas* atlas = io.Fonts;
        atlas->Clear();
        wchar_t windows[MAX_PATH];
        GetWindowsDirectoryW(windows, MAX_PATH);
        std::string fontDir = Narrow(windows) + "\\Fonts\\";
        std::string bold = fontDir + "segoeuib.ttf", regular = fontDir + "segoeui.ttf";
        auto add = [&](const std::string& file, float px) {
            ImFontConfig config;
            config.OversampleH = 2;
            ImFont* font = Exists(fs::u8path(file)) ? atlas->AddFontFromFileTTF(file.c_str(), px, &config, atlas->GetGlyphRangesDefault()) : nullptr;
            if (!font)
            {
                config.SizePixels = px;
                font = atlas->AddFontDefault(&config);
            }
            return font;
        };
        g_ui = add(regular, UI_PX);
        const float sizes[2] = { SMALL_PX, LARGE_PX };
        for (int i = 0; i < 2; i++)
        {
            g_outline[i] = add(bold, sizes[i]);
            g_fill[i] = add(bold, sizes[i]);
        }
        io.FontDefault = g_ui;
        std::vector<PendingGlyph> pending;
        if (g_haveFonts)
        {
            AddGameGlyphs(atlas, ma::FontId::Small, SMALL_PX, pending);
            AddGameGlyphs(atlas, ma::FontId::Large, LARGE_PX, pending);
        }
        atlas->Build();
        unsigned char* pixels;
        int width, height;
        atlas->GetTexDataAsRGBA32(&pixels, &width, &height);
        for (const PendingGlyph& glyph : pending)
        {
            const ImFontAtlasCustomRect* rect = atlas->GetCustomRectByIndex(glyph.rect);
            if (!rect || !rect->IsPacked())
                continue;
            for (int y = 0; y < rect->Height; y++)
                memcpy(pixels + (size_t(rect->Y + y) * width + rect->X) * 4, glyph.px.data() + size_t(y) * rect->Width, rect->Width * 4);
        }
        Log("[launcher] fonts: %s, atlas %dx%d, %zu game glyph images", g_haveFonts ? "the game's Titans fonts" : "Segoe UI only", width, height,
            pending.size());
    }

    // ---------------------------------------------------------------------------------------------
    // Backdrop: the main menu's cloth (FE_Background_*: weave, folds, light map, foliage), drawn
    // on the CPU at the window's size.

    std::vector<uint32_t> ComposeBackdrop(int W, int H, float s)
    {
        std::vector<float> rgb(size_t(W) * H * 3);
        for (int y = 0; y < H; y++)
        {
            float t = float(y) / std::max(1, H - 1);
            float r = 255 - 14 * t, g = 204 - 58 * t, b = 66 - 34 * t;
            for (int x = 0; x < W; x++)
            {
                float* p = &rgb[(size_t(y) * W + x) * 3];
                p[0] = r, p[1] = g, p[2] = b;
            }
        }
        auto luminance = [](uint32_t p) { return float(p & 0xFF) / 255.0f; };  // grey images: R = G = B
        if (const ma::Image* weave = ma::GetArt(ma::Art::BackdropWeave))
        {
            int tile = std::max(16, int(std::lround(256 * s)));
            const ma::Image& w = ma::Scaled(*weave, tile, tile);
            double mean = 0;
            for (uint32_t p : w.px) mean += luminance(p);
            mean /= std::max<size_t>(1, w.px.size());
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    float k = 1.0f + 1.1f * (luminance(w.px[size_t(y % tile) * tile + x % tile]) - float(mean));
                    float* p = &rgb[(size_t(y) * W + x) * 3];
                    p[0] *= k, p[1] *= k, p[2] *= k;
                }
        }
        if (const ma::Image* fold = ma::GetArt(ma::Art::BackdropFold))
        {
            const ma::Image& f = ma::Scaled(*fold, W, H);
            for (size_t i = 0; i < f.px.size(); i++)
            {
                float v = luminance(f.px[i]);
                float* p = &rgb[i * 3];
                p[0] = p[0] * (0.86f + 0.3f * v) + 40 * v, p[1] = p[1] * (0.86f + 0.3f * v) + 30 * v, p[2] = p[2] * (0.86f + 0.3f * v);
            }
        }
        if (const ma::Image* light = ma::GetArt(ma::Art::LightMap))
        {
            const ma::Image& l = ma::Scaled(*light, W, H);
            for (size_t i = 0; i < l.px.size(); i++)
            {
                float k = 1.1f - 0.34f * luminance(l.px[i]);
                float* p = &rgb[i * 3];
                p[0] *= k, p[1] *= k, p[2] *= k;
            }
        }
        // Foliage silhouettes in the corners, in a darker orange.
        auto foliage = [&](ma::Art art, bool bottom) {
            const ma::Image* leaf = ma::GetArt(art);
            if (!leaf)
                return;
            int size = int(std::lround(H * 0.62f));
            for (int side = 0; side < 2; side++)
            {
                const ma::Image& img = ma::Scaled(*leaf, size, size, side == 1);
                int ox = side ? W - size : 0, oy = bottom ? H - size : 0;
                for (int y = 0; y < size; y++)
                    for (int x = 0; x < size; x++)
                    {
                        int px = ox + x, py = oy + y;
                        if (px < 0 || py < 0 || px >= W || py >= H)
                            continue;
                        float a = float(img.px[size_t(y) * size + x] >> 24) / 255.0f * 0.36f;
                        float* p = &rgb[(size_t(py) * W + px) * 3];
                        p[0] += (196 - p[0]) * a, p[1] += (92 - p[1]) * a, p[2] += (12 - p[2]) * a;
                    }
            }
        };
        foliage(ma::Art::FoliageTop, false);
        foliage(ma::Art::FoliageBottom, true);
        std::vector<uint32_t> out(size_t(W) * H);
        for (size_t i = 0; i < out.size(); i++)
        {
            auto c = [&](int k) { return uint32_t(std::clamp(rgb[i * 3 + k], 0.0f, 255.0f)); };
            out[i] = c(0) | (c(1) << 8) | (c(2) << 16) | 0xFF000000u;
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------------
    // Drawing helpers (virtual units)

    struct View
    {
        float s = 1, ox = 0, oy = 0;
        ImVec2 P(float x, float y) const { return ImVec2(ox + x * s, oy + y * s); }
    } g_view;

    ImDrawList* g_draw;

    float TextWidth(int font, const std::string& text, float size)
    {
        return g_fill[font]->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    }

    // Text in the game's style: `align` 0 left, 0.5 centre, 1 right; shrunk to `maxWidth` when wider.
    // Returns the virtual x after it.
    float Draw(int font, const std::string& text, float x, float y, float size, const Style& style, float align = 0.0f,
               float maxWidth = 1e9f, float alpha = 1.0f)
    {
        float width = TextWidth(font, text, size);
        if (width > maxWidth && width > 0.0f)
        {
            float fitted = size * maxWidth / width;
            y += (size - fitted) * 0.5f;
            size = fitted, width = maxWidth;
        }
        float left = x - width * align;
        auto fade = [&](ImU32 c) { return (c & 0x00FFFFFFu) | (ImU32(((c >> 24) & 0xFF) * alpha) << 24); };
        g_draw->AddText(g_outline[font], size * g_view.s, g_view.P(left, y), fade(style.outline), text.c_str());
        g_draw->AddText(g_fill[font], size * g_view.s, g_view.P(left, y), fade(style.fill), text.c_str());
        return left + width;
    }

    std::string IconText(char32_t icon)
    {
        ImWchar c = ImWchar(ICON_BASE + icon);
        char utf8[4] = { char(0xE0 | (c >> 12)), char(0x80 | ((c >> 6) & 0x3F)), char(0x80 | (c & 0x3F)), 0 };
        return utf8;
    }

    // A button icon of the game (in colour); returns the virtual x after it.
    float Icon(char32_t icon, float x, float y, float size, float alpha = 1.0f)
    {
        std::string text = IconText(icon);
        if (!g_haveFonts)
            return x;
        g_draw->AddText(g_fill[1], size * g_view.s, g_view.P(x, y), IM_COL32(255, 255, 255, int(255 * alpha)), text.c_str());
        return x + TextWidth(1, text, size);
    }

    // Windows font (paths, messages that may have any character).
    void DrawUi(const std::string& text, float x, float y, float size, ImU32 color, float align = 0.0f, float maxWidth = 1e9f)
    {
        float width = g_ui->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
        if (width > maxWidth)
            size *= maxWidth / width, width = maxWidth;
        g_draw->AddText(g_ui, size * g_view.s, g_view.P(x - width * align + 1.5f, y + 1.5f), IM_COL32(40, 16, 0, 200), text.c_str());
        g_draw->AddText(g_ui, size * g_view.s, g_view.P(x - width * align, y), color, text.c_str());
    }

    // MSG_Background (the top-left quarter of the game's message boxes: curl, edge and the black
    // inside) mirrored into the four corners at a fixed size (`corner` virtual pixels high); the
    // straight parts are stretched: the edge sideways, the quarter's last rows down the sides.
    void DrawFrame(float x0, float y0, float x1, float y1, float corner = CORNER)
    {
        ImVec2 a = g_view.P(x0, y0), b = g_view.P(x1, y1);
        if (!g_frame.view)
        {
            g_draw->AddRectFilled(a, b, IM_COL32(20, 8, 0, 225), 18.0f * g_view.s);
            g_draw->AddRect(a, b, IM_COL32(236, 222, 190, 200), 18.0f * g_view.s, 0, 3.0f * g_view.s);
            return;
        }
        float halfW = (b.x - a.x) * 0.5f;
        float qh = std::min(corner * g_view.s, (b.y - a.y) * 0.5f);
        float k = qh / g_frame.h;
        const float curlU = 1.0f / 3.0f;
        float curlW = std::min(g_frame.w * curlU * k, halfW);
        float lastRow = 1.0f - 2.0f / g_frame.h;  // the quarter's bottom rows: the straight side
        for (int q = 0; q < 4; q++)
        {
            bool flipX = q & 1, flipY = q & 2;
            float qx = flipX ? b.x - halfW : a.x;
            float cx0 = flipX ? qx + halfW - curlW : qx, cx1 = cx0 + curlW;
            float ex0 = flipX ? qx : qx + curlW, ex1 = ex0 + (halfW - curlW);
            ImVec2 cu = flipX ? ImVec2(curlU, 0.0f) : ImVec2(0.0f, curlU);
            ImVec2 eu = flipX ? ImVec2(1.0f, curlU) : ImVec2(curlU, 1.0f);
            // The corner quarter.
            float qy = flipY ? b.y - qh : a.y;
            float v0 = flipY ? 1.0f : 0.0f, v1 = flipY ? 0.0f : 1.0f;
            g_draw->AddImage(g_frame.Id(), ImVec2(cx0, qy), ImVec2(cx1, qy + qh), ImVec2(cu.x, v0), ImVec2(cu.y, v1), FRAME_TINT);
            g_draw->AddImage(g_frame.Id(), ImVec2(ex0, qy), ImVec2(ex1, qy + qh), ImVec2(eu.x, v0), ImVec2(eu.y, v1), FRAME_TINT);
            // The side between the top and bottom corners (once per side).
            if (!flipY && b.y - a.y > 2.0f * qh)
            {
                float my0 = a.y + qh, my1 = b.y - qh;
                g_draw->AddImage(g_frame.Id(), ImVec2(cx0, my0), ImVec2(cx1, my1), ImVec2(cu.x, lastRow), ImVec2(cu.y, 1.0f), FRAME_TINT);
                g_draw->AddImage(g_frame.Id(), ImVec2(ex0, my0), ImVec2(ex1, my1), ImVec2(eu.x, lastRow), ImVec2(eu.y, 1.0f), FRAME_TINT);
            }
        }
    }

    // Ten stars, as the pause menu's volume bars (settings_menu.cpp, DrawStars).
    void DrawStars(float x, float y, int value, int maximum, const Style& style)
    {
        float size = STAR_SIZE, pitch = STAR_PITCH;
        float filled = float(value) / float(maximum) * STARS;
        for (int i = 0; i < STARS; i++)
        {
            float sx = x + i * pitch, sy = y + (ROW_SIZE - size) * 0.5f;
            if (!g_star.view)
            {
                g_draw->AddRectFilled(g_view.P(sx, sy + 9), g_view.P(sx + size - 3, sy + 17), IM_COL32(84, 42, 2, 120));
                if (filled > i)
                    g_draw->AddRectFilled(g_view.P(sx, sy + 9), g_view.P(sx + (size - 3) * std::min(1.0f, filled - i), sy + 17), style.fill);
                continue;
            }
            float e = size * 0.7f, eo = (size - e) * 0.5f;
            g_draw->AddImage(g_star.Id(), g_view.P(sx + eo, sy + eo), g_view.P(sx + eo + e, sy + eo + e), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(84, 42, 2, 110));
            if (filled > i)
            {
                float part = std::min(1.0f, filled - i), rim = 1.5f;
                g_draw->AddImage(g_star.Id(), g_view.P(sx - rim, sy - rim), g_view.P(sx - rim + (size + rim * 2) * part, sy + size + rim),
                                 ImVec2(0, 0), ImVec2(part, 1), style.outline);
                g_draw->AddImage(g_star.Id(), g_view.P(sx, sy), g_view.P(sx + size * part, sy + size), ImVec2(0, 0), ImVec2(part, 1), style.fill);
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // The screen

    bool KeyPressed(std::initializer_list<ImGuiKey> keys, bool repeat = true)
    {
        for (ImGuiKey key : keys)
            if (ImGui::IsKeyPressed(key, repeat))
                return true;
        return false;
    }

    void SwitchTab(int direction)
    {
        g_tab = (g_tab + int(TABS.size()) + direction) % int(TABS.size());
        g_focusFirstRow = true;
        g_capturing = false;
    }

    void DrawLeft()
    {
        // The title screen: logo, "Definitive Edition" in gold under it.
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

        // The menu, as the main menu's items.
        struct Entry { Text text; int id; };
        const Entry entries[] = { { { "PLAY", "JOGAR" }, 0 }, { { "SAVE", "SALVAR" }, 1 }, { { "DEFAULTS", "PADRÕES" }, 2 }, { { "EXIT", "SAIR" }, 3 } };
        float my = 384.0f;
        g_menuFocus = -1;
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        for (const Entry& entry : entries)
        {
            std::string text = T(entry.text);
            float size = entry.id == 0 ? 54.0f : 42.0f, width = TextWidth(1, text, size);
            ImGui::SetCursorScreenPos(g_view.P(LEFT_CX - width * 0.5f - 20.0f, my - 4.0f));
            ImGui::PushID(entry.id);
            bool pressed = ImGui::InvisibleButton("menu", ImVec2((width + 40.0f) * g_view.s, (size + 8.0f) * g_view.s));
            bool hovered = ImGui::IsItemHovered() && g_modal == Modal::None;
            ImGui::PopID();
            if (hovered)
                g_menuFocus = entry.id;
            bool disabled = entry.id == 1 && g_values == g_saved;
            Style style = hovered ? MENU_SELECTED : disabled ? INFO : MENU;
            Draw(1, text, LEFT_CX, my, size, style, 0.5f);
            if (pressed && g_modal == Modal::None)
            {
                switch (entry.id)
                {
                case 0: StartGame(); break;
                case 1: Save(); break;
                case 2: Defaults(); break;
                case 3: RequestExit(); break;
                }
            }
            my += size + 18.0f;
        }
        ImGui::PopItemFlag();

        // Where the game is, and whether the options are saved.
        fs::path game = GameFolder();
        std::string where = game.empty() ? T({ "Game folder: not chosen", "Pasta do jogo: não escolhida" })
                                         : std::string(T({ "Game: ", "Jogo: " })) + Narrow(game.wstring());
        DrawUi(where, LEFT_CX, 650.0f, 17.0f, paths::IsGameFolder(game) ? IM_COL32(255, 245, 220, 235) : IM_COL32(255, 120, 80, 255), 0.5f, 430.0f);
        DrawUi(g_values == g_saved ? T({ "Options saved", "Opções salvas" }) : T({ "Unsaved changes", "Mudanças não salvas" }), LEFT_CX, 674.0f, 17.0f,
               g_values == g_saved ? IM_COL32(255, 245, 220, 200) : IM_COL32(255, 230, 120, 255), 0.5f);
    }

    void DrawTabs()
    {
        // Dividers under the tabs, as the name screen's.
        float dy = TABS_Y + TAB_SIZE + 10.0f;
        if (g_dividerL.view && g_dividerR.view)
        {
            float h = 18.0f, wl = h * g_dividerL.w / g_dividerL.h, wr = h * g_dividerR.w / g_dividerR.h, cx = (PANEL_L + PANEL_R) * 0.5f;
            g_draw->AddImage(g_dividerL.Id(), g_view.P(cx - 8.0f - wl, dy), g_view.P(cx - 8.0f, dy + h), ImVec2(0, 0), ImVec2(1, 1), TITLE.outline);
            g_draw->AddImage(g_dividerR.Id(), g_view.P(cx + 8.0f, dy), g_view.P(cx + 8.0f + wr, dy + h), ImVec2(0, 0), ImVec2(1, 1), TITLE.outline);
        }
        if (!Advanced())
        {
            Draw(1, T(SIMPLE.name), (PANEL_L + PANEL_R) * 0.5f, TABS_Y, TAB_SIZE, SELECTED, 0.5f);
            return;
        }
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        float left = PANEL_L + 6.0f, right = PANEL_R - 6.0f;
        float lbW = g_haveFonts ? TextWidth(1, IconText(ma::ICON_LB), 34.0f) : 0.0f;
        Icon(ma::ICON_LB, left, TABS_Y - 2.0f, 34.0f);
        Icon(ma::ICON_RB, right - lbW, TABS_Y - 2.0f, 34.0f);
        float x0 = left + lbW + 14.0f, x1 = right - lbW - 14.0f;
        float total = 0.0f, gap = 16.0f;
        std::vector<float> widths;
        for (const Tab& tab : TABS)
            widths.push_back(TextWidth(1, T(tab.name), TAB_SIZE)), total += widths.back();
        float size = TAB_SIZE;
        float room = x1 - x0 - gap * (TABS.size() - 1);
        if (total > room)
        {
            size *= room / total;
            for (float& w : widths) w *= room / total;
            total = room;
        }
        float x = x0 + (x1 - x0 - total - gap * (TABS.size() - 1)) * 0.5f;
        for (int i = 0; i < int(TABS.size()); i++)
        {
            ImGui::SetCursorScreenPos(g_view.P(x - gap * 0.4f, TABS_Y - 6.0f));
            ImGui::PushID(1000 + i);
            if (ImGui::InvisibleButton("tab", ImVec2((widths[i] + gap * 0.8f) * g_view.s, (TAB_SIZE + 12.0f) * g_view.s)) && g_modal == Modal::None)
            {
                g_tab = i;
                g_focusFirstRow = true;
                g_capturing = false;
            }
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            Draw(1, T(TABS[i].name), x, TABS_Y + (TAB_SIZE - size) * 0.5f, size, i == g_tab ? SELECTED : hovered ? TITLE : NORMAL);
            if (i == g_tab)
                g_draw->AddLine(g_view.P(x, TABS_Y + TAB_SIZE + 4.0f), g_view.P(x + widths[i], TABS_Y + TAB_SIZE + 4.0f), SELECTED.fill, 3.0f * g_view.s);
            x += widths[i] + gap;
        }
        ImGui::PopItemFlag();
    }

    void DrawRows()
    {
        const Tab& tab = CurrentTab();
        ImGui::SetCursorScreenPos(g_view.P(PANEL_L + 20.0f, ROWS_T));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar;
        ImGui::BeginChild("rows", ImVec2((PANEL_R - PANEL_L - 40.0f) * g_view.s, (ROWS_B - ROWS_T) * g_view.s), ImGuiChildFlags_NavFlattened, flags);
        ImDrawList* outer = g_draw;
        g_draw = ImGui::GetWindowDrawList();
        float scroll = ImGui::GetScrollY() / g_view.s;
        bool anyHovered = false;
        Item focused = g_selected;
        bool haveFocus = false;
        for (size_t i = 0; i < tab.items.size(); i++)
        {
            Item item = tab.items[i];
            float y = ROWS_T + (ROW_PITCH - ROW_SIZE) * 0.5f + i * ROW_PITCH;  // virtual, before scrolling
            float vy = y - scroll;
            ImGui::SetCursorScreenPos(g_view.P(PANEL_L + 20.0f, vy - (ROW_PITCH - ROW_SIZE) * 0.5f));
            if (IsHeader(item))
            {
                ImGui::Dummy(ImVec2(10.0f, ROW_PITCH * g_view.s));
                std::string text = Label(item);
                Draw(1, text, (PANEL_L + PANEL_R) * 0.5f, vy + 2.0f, ROW_SIZE - 4.0f, TITLE, 0.5f);
                float w = TextWidth(1, text, ROW_SIZE - 4.0f) * 0.5f + 16.0f, cx = (PANEL_L + PANEL_R) * 0.5f, ly = vy + ROW_SIZE * 0.5f + 2.0f;
                g_draw->AddLine(g_view.P(PANEL_L + 70.0f, ly), g_view.P(cx - w, ly), IM_COL32(214, 120, 14, 140), 2.0f * g_view.s);
                g_draw->AddLine(g_view.P(cx + w, ly), g_view.P(PANEL_R - 70.0f, ly), IM_COL32(214, 120, 14, 140), 2.0f * g_view.s);
                continue;
            }
            bool selectable = Selectable(item);
            ImGui::PushID(int(item));
            ImGui::BeginDisabled(!selectable || g_modal != Modal::None || (g_capturing && item != g_selected));
            if (g_focusFirstRow && selectable)
            {
                g_focusFirstRow = false;
                g_selected = item;
                g_scrollToSelected = true;
            }
            bool pressed = ImGui::InvisibleButton("row", ImVec2((PANEL_R - PANEL_L - 40.0f) * g_view.s, ROW_PITCH * g_view.s));
            bool hovered = ImGui::IsItemHovered();
            bool active = ImGui::IsItemActive();
            if (g_scrollToSelected && item == g_selected)
            {
                ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
                g_scrollToSelected = false;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            if (g_mouseMode && hovered)
                focused = item, haveFocus = true;
            anyHovered |= hovered;
            bool selected = item == g_selected && selectable;

            const Style& style = !selectable ? INFO : selected ? SELECTED : NORMAL;
            Draw(0, Label(item), LABEL_RIGHT, vy, ROW_SIZE, style, 1.0f, LABEL_RIGHT - LABEL_LEFT);
            int minimum, maximum, step;
            int* percent = Percent(item, &minimum, &maximum, &step);
            ImVec2 mouse = ImGui::GetIO().MousePos;
            float mx = (mouse.x - g_view.ox) / g_view.s;
            if (percent)
            {
                DrawStars(VALUE_X, vy, *percent - minimum, maximum - minimum, style);
                Draw(0, Value(item), VALUE_R, vy, ROW_SIZE - 4.0f, style, 1.0f);
                float starsR = VALUE_X + STARS * STAR_PITCH;
                // Click / drag on the stars sets the value; keyboard / pad A steps it.
                if (active && ImGui::IsMouseDown(0) && mx >= VALUE_X - 10.0f && mx <= starsR + 10.0f)
                {
                    float t = std::clamp((mx - VALUE_X) / (starsR - VALUE_X), 0.0f, 1.0f);
                    *percent = std::clamp(minimum + int(std::lround(t * (maximum - minimum) / step)) * step, minimum, maximum);
                }
                else if (pressed && !ImGui::IsMouseReleased(0))
                    Change(item, +1);
            }
            else
            {
                std::string value = Value(item);
                bool arrows = selected && !IsBinding(item) && item != Item::GameFolder && item != Item::ModsFolder && item != Item::SavesFolder &&
                              item != Item::ExtraData;
                float iconSize = ROW_SIZE * 0.8f;
                float left = VALUE_X;
                if (arrows)
                    left = Icon(ma::ICON_LEFT, VALUE_X, vy + 3.0f, iconSize) + 6.0f;
                float maxW = VALUE_R - left - (arrows ? 34.0f : 0.0f);
                float end = Draw(0, value, left, vy, ROW_SIZE, g_capturing && selected ? WARNING : style, 0.0f, maxW);
                if (arrows)
                    Icon(ma::ICON_RIGHT, end + 6.0f, vy + 3.0f, iconSize);
                if (pressed)
                {
                    bool mouseClick = ImGui::IsMouseReleased(0);
                    if (mouseClick && arrows && mx < left)
                        Change(item, -1);
                    else
                        Activate(item);
                }
            }
        }
        if (!g_capturing && haveFocus)
            g_selected = focused;
        (void)anyHovered;
        ImGui::EndChild();
        g_draw = outer;
    }

    void DrawHelpAndFooter()
    {
        auto [line1, line2] = Help(g_selected);
        const std::vector<Item>& items = CurrentTab().items;
        if (items.end() == std::find(items.begin(), items.end(), g_selected))
            line1.clear(), line2.clear();
        // Paths go in the Windows font (any character); the rest in the game's.
        bool plain = g_selected == Item::GameFolder || g_selected == Item::ModsFolder || g_selected == Item::SavesFolder || g_selected == Item::ExtraData;
        float maxW = PANEL_R - PANEL_L - 160.0f, cx = (PANEL_L + PANEL_R) * 0.5f;
        g_draw->AddLine(g_view.P(PANEL_L + 90.0f, HELP_Y - 8.0f), g_view.P(PANEL_R - 90.0f, HELP_Y - 8.0f), IM_COL32(236, 222, 190, 70), 2.0f * g_view.s);
        if (plain)
            DrawUi(line1, cx, HELP_Y, 21.0f, IM_COL32(238, 226, 196, 255), 0.5f, maxW);
        else
            Draw(0, line1, cx, HELP_Y, HELP_SIZE, HELP, 0.5f, maxW);
        Draw(0, line2, cx, HELP_Y + HELP_SIZE + 4.0f, HELP_SIZE, HELP, 0.5f, maxW);

        // (Y) ADVANCED / SIMPLE in the corner (Y or F2, or a click), and centred in the room left of
        // it: (A) CHANGE  LB RB TABS (advanced)  START PLAY  (B) EXIT.
        float iconSize = FOOTER_SIZE * 1.05f;
        std::string view = Advanced() ? T({ "SIMPLE", "SIMPLES" }) : T({ "ADVANCED", "AVANÇADO" });
        float viewR = PANEL_R - 6.0f;
        float viewL = viewR - TextWidth(1, IconText(ma::ICON_Y), iconSize) - 6.0f - TextWidth(0, view, FOOTER_SIZE);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::SetCursorScreenPos(g_view.P(viewL - 12.0f, FOOTER_Y - 8.0f));
        bool toggle = ImGui::InvisibleButton("view", ImVec2((viewR - viewL + 24.0f) * g_view.s, (FOOTER_SIZE + 16.0f) * g_view.s));
        bool hovered = ImGui::IsItemHovered() && g_modal == Modal::None;
        ImGui::PopItemFlag();
        float vx = Icon(ma::ICON_Y, viewL, FOOTER_Y, iconSize) + 6.0f;
        Draw(0, view, vx, FOOTER_Y + 1.0f, FOOTER_SIZE, hovered ? MENU_SELECTED : TITLE);
        if (toggle && g_modal == Modal::None)
            SetAdvanced(!Advanced());

        struct Hint { char32_t icon; Text text; };
        std::vector<Hint> hints = { { ma::ICON_A, { "CHANGE", "MUDAR" } } };
        if (Advanced())
            hints.push_back({ ma::ICON_LB, { "", "" } }), hints.push_back({ ma::ICON_RB, { "TABS", "ABAS" } });
        hints.push_back({ ma::ICON_START, { "PLAY", "JOGAR" } });
        hints.push_back({ ma::ICON_B, { "EXIT", "SAIR" } });
        float total = 0.0f;
        for (const Hint& hint : hints)
            total += TextWidth(1, IconText(hint.icon), iconSize) + 6.0f + (*T(hint.text) ? TextWidth(0, T(hint.text), FOOTER_SIZE) + 26.0f : 0.0f);
        float x = (PANEL_L + viewL - 30.0f) * 0.5f - total * 0.5f;
        for (const Hint& hint : hints)
        {
            x = Icon(hint.icon, x, FOOTER_Y, iconSize) + 6.0f;
            if (*T(hint.text))
                x = Draw(0, T(hint.text), x, FOOTER_Y + 1.0f, FOOTER_SIZE, TITLE) + 26.0f;
        }
    }

    bool Clicked(float x0, float y0, float x1, float y1)
    {
        ImVec2 m = ImGui::GetIO().MousePos;
        float x = (m.x - g_view.ox) / g_view.s, y = (m.y - g_view.oy) / g_view.s;
        return ImGui::IsMouseClicked(0) && x >= x0 && x < x1 && y >= y0 && y < y1;
    }

    void DrawModal()
    {
        g_draw->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, IM_COL32(10, 4, 0, 150));
        float l = 300.0f, r = 980.0f, t = 250.0f, b = 470.0f, cx = VW * 0.5f;
        DrawFrame(l, t, r, b);
        if (g_modal == Modal::Exit)
        {
            Draw(0, T({ "Save the changes before leaving?", "Salvar as mudanças antes de sair?" }), cx, t + 50.0f, 30.0f, HELP, 0.5f, r - l - 80.0f);
            float y = t + 120.0f;
            float x = cx - 270.0f, x0 = x;
            x = Icon(ma::ICON_A, x, y, 32.0f) + 8.0f;
            x = Draw(0, T({ "SAVE", "SALVAR" }), x, y + 2.0f, 30.0f, SELECTED) + 40.0f;
            bool save = Clicked(x0, y - 6.0f, x - 20.0f, y + 40.0f);
            x0 = x;
            x = Icon(ma::ICON_X, x, y, 32.0f) + 8.0f;
            x = Draw(0, T({ "DON'T SAVE", "NÃO SALVAR" }), x, y + 2.0f, 30.0f, NORMAL) + 40.0f;
            bool discard = Clicked(x0, y - 6.0f, x - 20.0f, y + 40.0f);
            x0 = x;
            x = Icon(ma::ICON_B, x, y, 32.0f) + 8.0f;
            x = Draw(0, T({ "BACK", "VOLTAR" }), x, y + 2.0f, 30.0f, NORMAL);
            bool back = Clicked(x0, y - 6.0f, x + 20.0f, y + 40.0f);
            if (save || KeyPressed({ ImGuiKey_GamepadFaceDown, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_S }, false))
                Save(), g_quit = true;
            else if (discard || KeyPressed({ ImGuiKey_GamepadFaceLeft, ImGuiKey_N, ImGuiKey_Backspace }, false))
                g_quit = true;
            else if (back || KeyPressed({ ImGuiKey_GamepadFaceRight, ImGuiKey_Escape }, false))
                g_modal = Modal::None;
        }
        else
        {
            Draw(0, g_message[0], cx, t + 46.0f, 30.0f, HELP, 0.5f, r - l - 80.0f);
            Draw(0, g_message[1], cx, t + 90.0f, 28.0f, HELP, 0.5f, r - l - 80.0f);
            float x = cx - 40.0f;
            x = Icon(ma::ICON_A, x, t + 150.0f, 32.0f) + 8.0f;
            Draw(0, "OK", x, t + 152.0f, 30.0f, SELECTED);
            if (KeyPressed({ ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape }, false) ||
                ImGui::IsMouseClicked(0))
                g_modal = Modal::None;
        }
    }

    // The next / previous selectable row of the tab (round the end, as the game's screen).
    void MoveSelection(int direction)
    {
        const std::vector<Item>& items = CurrentTab().items;
        int count = int(items.size());
        int at = int(std::find(items.begin(), items.end(), g_selected) - items.begin());
        if (at == count)
            at = direction > 0 ? -1 : count;
        for (int i = 0; i < count; i++)
        {
            at = (at + count + direction) % count;
            if (Selectable(items[size_t(at)]))
            {
                g_selected = items[size_t(at)];
                g_scrollToSelected = true;
                return;
            }
        }
    }

    void HandleKeys()
    {
        if (g_modal != Modal::None)
            return;
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
            g_mouseMode = true;
        bool down = KeyPressed({ ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown });
        bool up = KeyPressed({ ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp });
        if (up || down)
            g_mouseMode = false;
        if (g_capturing)
        {
            if (KeyPressed({ ImGuiKey_GamepadFaceRight }, false))
                g_captureCancelled = true;
            if (g_captured)
            {
                Bind(ActionOf(g_selected), g_captured);
                g_capturing = false;
            }
            else if (g_captureCancelled)
                g_capturing = false;
            return;
        }
        if (KeyPressed({ ImGuiKey_GamepadFaceUp, ImGuiKey_Y, ImGuiKey_F2 }, false))
            SetAdvanced(!Advanced());
        if (Advanced() && KeyPressed({ ImGuiKey_GamepadL1, ImGuiKey_Q, ImGuiKey_PageUp }))
            SwitchTab(-1);
        if (Advanced() && KeyPressed({ ImGuiKey_GamepadR1, ImGuiKey_E, ImGuiKey_PageDown }))
            SwitchTab(+1);
        if (KeyPressed({ ImGuiKey_GamepadStart, ImGuiKey_F5 }, false))
            StartGame();
        if (KeyPressed({ ImGuiKey_GamepadFaceRight, ImGuiKey_Escape }, false))
            RequestExit();
        // UP / DOWN and A / Enter here, not by ImGui's navigation (which loses its focused item
        // when the window opens behind another one), as the game's settings screen.
        if (up || down)
            MoveSelection(down ? +1 : -1);
        const std::vector<Item>& items = CurrentTab().items;
        bool inTab = std::find(items.begin(), items.end(), g_selected) != items.end();
        if (!inTab || !Selectable(g_selected))
            return;
        if (KeyPressed({ ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown }, false))
            Activate(g_selected);
        if (KeyPressed({ ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft }) && !IsBinding(g_selected))
            Change(g_selected, -1);
        if (KeyPressed({ ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight }) && !IsBinding(g_selected))
            Change(g_selected, +1);
        if (IsBinding(g_selected) && KeyPressed({ ImGuiKey_GamepadFaceLeft, ImGuiKey_Backspace, ImGuiKey_Delete }, false))
            ResetBinding(ActionOf(g_selected));
    }

    void DrawFrameContents()
    {
        ImGuiIO& io = ImGui::GetIO();
        float s = std::min(io.DisplaySize.x / VW, io.DisplaySize.y / VH);
        g_view = { s, (io.DisplaySize.x - VW * s) * 0.5f, (io.DisplaySize.y - VH * s) * 0.5f };

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoMove);
        g_draw = ImGui::GetWindowDrawList();
        if (g_backdrop.view)
            ImGui::GetBackgroundDrawList()->AddImage(g_backdrop.Id(), ImVec2(0, 0), io.DisplaySize);
        HandleKeys();
        DrawFrame(PANEL_L, PANEL_T, PANEL_R, PANEL_B);
        DrawLeft();
        DrawTabs();
        DrawRows();
        DrawHelpAndFooter();
        if (g_modal != Modal::None)
        {
            g_draw = ImGui::GetForegroundDrawList();
            DrawModal();
        }
        ImGui::End();
    }

    // Key capture for the CONTROLS rows: the next key or mouse button, as the game names them.
    bool CaptureMessage(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (!g_capturing || g_captured || g_captureCancelled)
            return false;
        uint16_t key = 0;
        switch (msg)
        {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        {
            UINT vk = UINT(wParam), scan = (lParam >> 16) & 0xFF;
            bool extended = (lParam >> 24) & 1;
            if (vk == VK_ESCAPE)
            {
                g_captureCancelled = true;
                return true;
            }
            if (vk == VK_SHIFT)
                vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
            else if (vk == VK_CONTROL)
                vk = extended ? VK_RCONTROL : VK_LCONTROL;
            else if (vk == VK_MENU)
                vk = extended ? VK_RMENU : VK_LMENU;
            key = uint16_t(vk);
            break;
        }
        case WM_LBUTTONDOWN: key = VK_LBUTTON; break;
        case WM_RBUTTONDOWN: key = VK_RBUTTON; break;
        case WM_MBUTTONDOWN: key = VK_MBUTTON; break;
        case WM_XBUTTONDOWN: key = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2; break;
        case WM_MOUSEWHEEL: key = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? settings::KEY_WHEEL_UP : settings::KEY_WHEEL_DOWN; break;
        default: return false;
        }
        if (!settings::KeyName(key).empty())
            g_captured = key;
        return true;
    }

#ifdef CRASH_INSTALLER
    void InstallerExit();  // installer.inc
#endif

    LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (CaptureMessage(msg, wParam, lParam))
            return 0;
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            return true;
        switch (msg)
        {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED)
                g_resizeW = LOWORD(lParam), g_resizeH = HIWORD(lParam);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = { 800, 480 };
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_KEYMENU)  // Alt opens no window menu
                return 0;
            break;
        case WM_CLOSE:
#ifdef CRASH_INSTALLER
            InstallerExit();
#else
            RequestExit();
#endif
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool SaveScreenshot(const fs::path& file)
    {
        ID3D11Texture2D* back = nullptr;
        g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
        D3D11_TEXTURE2D_DESC desc;
        back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING, desc.BindFlags = 0, desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ, desc.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        bool ok = false;
        if (SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &staging)))
        {
            g_context->CopyResource(staging, back);
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
            {
                std::vector<uint8_t> rgba(size_t(desc.Width) * desc.Height * 4);
                for (UINT y = 0; y < desc.Height; y++)
                    memcpy(&rgba[size_t(y) * desc.Width * 4], static_cast<uint8_t*>(map.pData) + size_t(y) * map.RowPitch, desc.Width * 4);
                for (size_t i = 3; i < rgba.size(); i += 4)
                    rgba[i] = 255;
                g_context->Unmap(staging, 0);
                ok = gpu::WritePng(file, rgba.data(), desc.Width, desc.Height, desc.Width * 4);
            }
            staging->Release();
        }
        back->Release();
        return ok;
    }

#ifdef CRASH_INSTALLER
#include "installer.inc"
#endif
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    int argc = __argc;
    char** argv = __argv;
    fs::path screenshot;
    int screenshotTab = -1;
    const char* screenshotView = nullptr;
    std::vector<char*> pathArgs = { argv[0] };
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshot = argv[++i];
        else if (strcmp(argv[i], "--tab") == 0 && i + 1 < argc)
            screenshotTab = atoi(argv[++i]);
        else if (strcmp(argv[i], "--view") == 0 && i + 1 < argc)  // simple | advanced, for this run only
            screenshotView = argv[++i];
#ifdef CRASH_INSTALLER
        else if (strcmp(argv[i], "--autostart") == 0)  // starts installing with the default options (scripts, tests)
            g_inst.autoStart = true;
        else if (strcmp(argv[i], "--option") == 0 && i + 1 < argc)  // --option subtitles|remaster|assets|copy (with --autostart)
        {
            std::string option = argv[++i];
            g_inst.subtitles |= option == "subtitles", g_inst.remaster |= option == "remaster";
            g_inst.extractAssets |= option == "assets", g_inst.copyGame |= option == "copy";
            if (option == "no-subtitles")
                g_inst.subtitles = false;
        }
#endif
        else
        {
            pathArgs.push_back(argv[i]);
            g_passArgs.push_back(argv[i]);
        }
    }

#ifdef CRASH_INSTALLER
    // The installer sits next to tools\setup.py: a dump already in dist\game (a second run) is used
    // without asking.
    std::string distGame;
    if (std::none_of(pathArgs.begin(), pathArgs.end(), [](const char* a) { return strcmp(a, "--game") == 0; }))
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        fs::path candidate = fs::path(self).parent_path() / "dist" / "game";
        if (paths::IsGameFolder(candidate))
        {
            distGame = Narrow(candidate.wstring());
            pathArgs.push_back(const_cast<char*>("--game"));
            pathArgs.push_back(distGame.data());
        }
    }
#endif
    ImGui_ImplWin32_EnableDpiAwareness();
    paths::Init(int(pathArgs.size()), pathArgs.data(), false);
    if (!paths::Game().empty())
        vfs::Init({ paths::Game(), paths::Mods(), paths::UserData() });
#ifndef CRASH_INSTALLER  // the installer has no options of the game (settings::Load would write userdata\settings.toml)
    settings::LoadLanguagePacks(paths::Languages());
    settings::Load(paths::UserData());
    g_values = g_saved = settings::Get();
#endif
    if (!paths::Game().empty())
        ma::StartLoading();
    if (screenshotTab >= 0)
        g_tab = std::clamp(screenshotTab, 0, int(TABS.size()) - 1);
    // --view / --tab change the view of this run only (not saved).
    if (screenshotView || screenshotTab >= 0)
        g_values.launcherAdvanced = g_saved.launcherAdvanced = screenshotView ? strcmp(screenshotView, "advanced") == 0 : true;

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance, LoadIconW(instance, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */),
                    nullptr, nullptr, L"CrashLauncher", nullptr };
    if (!wc.hIcon)
        wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512) /* IDI_APPLICATION */);
    RegisterClassExW(&wc);
    // 1280x720 client area times the monitor's scale, at most 90% of the work area.
    POINT origin{ 0, 0 };
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
    MONITORINFO info{ sizeof(info) };
    GetMonitorInfoW(monitor, &info);
    int workW = info.rcWork.right - info.rcWork.left, workH = info.rcWork.bottom - info.rcWork.top;
    float scale = std::min({ dpi, workW * 0.9f / VW, workH * 0.9f / VH });
    if (!screenshot.empty())
        scale = 1.0f;
    RECT rect{ 0, 0, LONG(VW * scale), LONG(VH * scale) };
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int ww = rect.right - rect.left, wh = rect.bottom - rect.top;
#ifdef CRASH_INSTALLER
    std::wstring title = L"Crash of the Titans: Definitive Edition - Setup";
#else
    std::wstring title = L"Crash of the Titans: Definitive Edition";
#endif
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, info.rcWork.left + (workW - ww) / 2,
                             info.rcWork.top + (workH - wh) / 2, ww, wh, nullptr, nullptr, instance, nullptr);
    if (!CreateDevice(g_hwnd))
    {
        MessageBoxW(nullptr, L"Direct3D 11 não está disponível.", L"CrashLauncher", MB_OK | MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_hwnd, screenshot.empty() ? SW_SHOWDEFAULT : SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // No ImGui navigation: the keys and the pad move the selection themselves (HandleKeys); the
    // Win32 backend still feeds the pad's buttons.
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
    style.WindowPadding = ImVec2(0, 0);
    style.ItemSpacing = ImVec2(0, 0);
    style.DisabledAlpha = 1.0f;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // The game's art (read on a thread since startup) and the fonts made from it.
    g_artLoaded = !paths::Game().empty() && ma::Ready(8000);
    g_haveFonts = g_artLoaded;
    BuildFonts();
    if (g_artLoaded)
    {
        g_logo = ArtTexture(ma::Art::TitleLogo);
        g_star = ArtTexture(ma::Art::StarFull);
        g_frame = ArtTexture(ma::Art::MessageFrame);
        g_dividerL = ArtTexture(ma::Art::DividerLeft);
        g_dividerR = ArtTexture(ma::Art::DividerRight);
    }
    else
        Log("[launcher] no game art (game folder %s)", paths::Game().empty() ? "not found" : "without the fonts");
    if (paths::Game().empty())  // SETUP first (shown in the advanced view; not saved)
        g_tab = SETUP_TAB, g_values.launcherAdvanced = g_saved.launcherAdvanced = true;
#ifdef CRASH_INSTALLER
    InitInstaller();
#endif

    int frames = 0;
    while (!g_quit)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                g_quit = true;
        }
        if (g_quit)
            break;
        if (IsIconic(g_hwnd))
        {
            Sleep(30);
            continue;
        }
        if (g_resizeW && g_resizeH)
        {
            ReleaseRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        int dw = int(io.DisplaySize.x), dh = int(io.DisplaySize.y);
        if (dw > 0 && dh > 0 && (dw != g_backdropW || dh != g_backdropH))
        {
            if (g_backdrop.view)
                g_backdrop.view->Release(), g_backdrop.view = nullptr;
            std::vector<uint32_t> px = ComposeBackdrop(dw, dh, std::min(dw / VW, dh / VH));
            ma::ClearCache();  // the scaled copies are only needed for this backdrop
            g_backdrop.view = MakeTexture(px.data(), dw, dh);
            g_backdropW = dw, g_backdropH = dh;
        }
        ImGui::NewFrame();
#ifdef CRASH_INSTALLER
        DrawInstallerContents();
#else
        DrawFrameContents();
#endif
        ImGui::Render();
        const float clear[4] = { 0.85f, 0.55f, 0.12f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        bool waitForShot = false;
#ifdef CRASH_INSTALLER
        // The screenshot waits for the prerequisite check, and with --autostart for the end of the install.
        waitForShot = frames == 3 && (g_inst.checking || (g_inst.autoStart && (g_inst.phase == Phase::Setup || g_inst.phase == Phase::Running)));
#endif
        if (!screenshot.empty() && !waitForShot && ++frames == 4)
        {
            bool ok = SaveScreenshot(screenshot);
            Log("[launcher] screenshot %s: %s", screenshot.string().c_str(), ok ? "ok" : "failed");
            g_quit = true;
        }
        g_swapChain->Present(1, 0);
    }
#ifdef CRASH_INSTALLER
    ShutdownInstaller();
#endif

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
