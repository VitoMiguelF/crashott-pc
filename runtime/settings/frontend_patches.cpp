// Built-in data patches behind the settings screen (registered with rcf_overlay before the game
// opens its archives; see runtime/NOTES.md, "Configuracoes"):
//
// 1. fighttrees\Frontend.bfig (default.rcf), the frontend's state machine. Each "Exit" record is
//    [0xBC27359D][name length][name, NUL, padding to 4][u32 owner?][u32][u32][f32 -1][u32][f32 -1]
//    [u32 target node]..., followed by the Lua snippets it runs as [u32 length][text]. The main
//    menu's ExitCalibration goes to node 0x23B (the TV calibration screen) and runs
//    "Managers.FrontendManager:PlaySoundSelect()\r\n\r\n". It now goes to the options screen of the
//    PS2 version (OptionsScreen, node 0x63, the target of the PS2 main menu's ExitOptions; still in
//    the Xbox data), which, unlike the calibration screen (black, without the frontend's cloth),
//    is a frontend screen like LOAD GAME's: the game plays its own transition to it and back,
//    shows the cloth and its (B) BACK / SELECT (A) hints. The snippet becomes
//    PlaySoundSelect(4242): the extra argument is ignored by the game and tells frontend_hooks.cpp
//    to open the settings. The options screen's ExitBack (B) returns to the main menu; the
//    settings screen presses B for the game when it closes. Its A (ExitAccept, into the PS2
//    options) never reaches the game while the settings are open.
// 2. Same file: the main menu's "Y STORAGE DEVICE" hint is a visibility record
//    [0x7B6567EE]["GameStart_Start"]["GameStart_DeviceSelector"][u32 visible] in the
//    ContentXenonWithProfile block; it is set to hidden. Y itself still works as before.
// 3. The frontend text bibles of package\7efdcd91.p3d and package\cdd70a8c.p3d (the ones the
//    frontend loads): keys 0x1E96 and 0xE1F3 (both CALIBRATION in English; in Russian
//    "brightness" and "picture adjustment") become the language's word for settings, written over
//    the original string (same or shorter).
// 5. package\cdd70a8c.p3d (Fe_Frontend_fe), the frontend's pages: the PS2 options page
//    (GameStart_OptionsPS2.pag: the WIDESCREEN / SOUND / VIBRATION / CREDITS menu and the Dolby
//    Pro Logic logo) made transparent, so that only the cloth and the hints are left, and the
//    settings are drawn over them.
// 4. (gone) sound\zscripts\mixers.p3d used to carry the music / effects / voice volumes, read at
//    boot; they are now applied live through the game's own volume categories
//    (audio/game_volumes.cpp).

#include "pc_frontend.h"
#include "settings.h"
#include "title_logo.h"
#include "../diagnostics.h"
#include "../fs/rcf_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace settings
{
    namespace
    {
        uint32_t LoadBE32(const uint8_t* p)
        {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }

        void StoreBE32(uint8_t* p, uint32_t value)
        {
            p[0] = uint8_t(value >> 24);
            p[1] = uint8_t(value >> 16);
            p[2] = uint8_t(value >> 8);
            p[3] = uint8_t(value);
        }

        uint32_t LoadLE32(const uint8_t* p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        // Every offset where `needle` occurs.
        std::vector<size_t> FindAll(const std::vector<uint8_t>& data, const std::string& needle)
        {
            std::vector<size_t> found;
            auto it = data.begin();
            while ((it = std::search(it, data.end(), needle.begin(), needle.end())) != data.end())
            {
                found.push_back(size_t(it - data.begin()));
                ++it;
            }
            return found;
        }

        // bfig Exit record: offset of the first u32 after the name ("fields"), or 0.
        constexpr uint32_t EXIT_RECORD = 0xBC27359D;
        constexpr uint32_t VISIBILITY_RECORD = 0x7B6567EE;
        constexpr size_t EXIT_TARGET = 0x18;  // from the fields

        size_t NameEnd(size_t nameOffset, size_t length)
        {
            return (nameOffset + length + 1 + 3) & ~size_t(3);
        }

        std::vector<size_t> ExitFields(const std::vector<uint8_t>& data, const char* name)
        {
            std::vector<size_t> fields;
            size_t length = strlen(name);
            for (size_t at : FindAll(data, std::string(name) + '\0'))
            {
                if (at < 8 || LoadBE32(&data[at - 8]) != EXIT_RECORD || LoadBE32(&data[at - 4]) != length)
                    continue;
                size_t end = NameEnd(at, length);
                if (end + EXIT_TARGET + 4 <= data.size())
                    fields.push_back(end);
            }
            return fields;
        }

        bool PatchFrontendBfig(std::vector<uint8_t>& data)
        {
            // Main menu node: where "Y" (ExitShowXenonDeviceSelect) returns to.
            std::vector<size_t> deviceSelect = ExitFields(data, "ExitShowXenonDeviceSelect");
            if (deviceSelect.empty())
                return false;
            uint32_t mainMenu = LoadBE32(&data[deviceSelect[0] + EXIT_TARGET]);

            // The PS2 options screen: where the PS2 main menu's ExitOptions goes.
            std::vector<size_t> options = ExitFields(data, "ExitOptions");
            if (options.empty())
                return false;
            uint32_t optionsScreen = LoadBE32(&data[options[0] + EXIT_TARGET]);

            // 1. ExitCalibration -> the options screen, with the marker in its sound snippet.
            const std::string snippet = "Managers.FrontendManager:PlaySoundSelect()\r\n\r\n";
            const std::string marked = "Managers.FrontendManager:PlaySoundSelect(4242)";
            static_assert(sizeof("Managers.FrontendManager:PlaySoundSelect()\r\n\r\n") == sizeof("Managers.FrontendManager:PlaySoundSelect(4242)"));
            int calibrations = 0;
            uint32_t calibrationTarget = 0;
            for (size_t fields : ExitFields(data, "ExitCalibration"))
            {
                // The snippet follows within the record.
                auto end = data.begin() + std::min(data.size(), fields + 0x80);
                auto text = std::search(data.begin() + fields, end, snippet.begin(), snippet.end());
                size_t textOffset = size_t(text - data.begin());
                if (text == end || LoadBE32(&data[textOffset - 4]) != snippet.size())
                    continue;
                calibrationTarget = LoadBE32(&data[fields + EXIT_TARGET]);
                StoreBE32(&data[fields + EXIT_TARGET], optionsScreen);
                memcpy(&data[textOffset], marked.data(), marked.size());
                calibrations++;
            }
            if (!calibrations)
                return false;

            // 2. Hide the Y hint shown with a signed-in profile.
            int hidden = 0;
            for (size_t block : FindAll(data, std::string("ContentXenonWithProfile") + '\0'))
            {
                const std::string group = std::string("GameStart_DeviceSelector");
                size_t limit = std::min(data.size(), block + 0x140);
                auto it = std::search(data.begin() + block, data.begin() + limit, group.begin(), group.end());
                size_t at = size_t(it - data.begin());
                if (it == data.begin() + limit || at < 4 || LoadBE32(&data[at - 4]) != group.size())
                    continue;
                size_t visible = (at + group.size() + 3) & ~size_t(3);
                if (visible + 4 <= data.size() && LoadBE32(&data[visible]) == 1)
                {
                    StoreBE32(&data[visible], 0);
                    hidden++;
                }
            }
            Log("[settings] frontend: %d ExitCalibration exit(s) 0x%X -> options screen 0x%X with the settings marker (main menu 0x%X); %d Y hint(s) hidden",
                calibrations, calibrationTarget, optionsScreen, mainMenu, hidden);
            return true;
        }

        // Text bibles (Pure3D, little-endian; see tools/texts.py).
        constexpr uint32_t BIBLE = 0x1800D, LANGUAGE = 0x1800E;
        constexpr uint32_t CALIBRATION_KEYS[] = { 0x1E96, 0xE1F3 };

        // The word for "settings" per bible letter; it must not be longer than the original.
        const char16_t* SettingsWord(char letter)
        {
            switch (letter)
            {
            case 'E': case 'U': return u"SETTINGS";
            case 'F': return u"PARAMETRES";
            case 'G': return u"OPTIONEN";
            case 'S': return u"AJUSTES";
            case 'I': return u"IMPOSTAZIONI";
            case 'D': return u"OPTIES";
            case 'A': return u"INDSTILLING";
            case 'N': return u"INNSTILLING";
            case 'W': return u"INSTÄLLNING";
            case 'H': return u"ASETUKSET";
            case 'R': return u"ОПЦИИ";  // OPTSII
            default: return nullptr;
            }
        }

        bool PatchLanguage(std::vector<uint8_t>& data, size_t offset, size_t end, int& patched)
        {
            size_t pos = offset + 12;
            if (pos >= end)
                return false;
            pos += 1 + data[pos];  // pascal name
            if (pos + 13 > end)
                return false;
            char letter = char(data[pos]);
            uint32_t count = LoadLE32(&data[pos + 1]);
            uint32_t bufferSize = LoadLE32(&data[pos + 9]);
            size_t hashes = pos + 13, offsets = hashes + size_t(count) * 4, buffer = offsets + size_t(count) * 4;
            if (buffer + bufferSize > end)
                return false;
            const char16_t* word = SettingsWord(letter);
            for (uint32_t i = 0; word && i < count; i++)
            {
                uint32_t key = LoadLE32(&data[hashes + i * 4]);
                if (std::find(std::begin(CALIBRATION_KEYS), std::end(CALIBRATION_KEYS), key) == std::end(CALIBRATION_KEYS))
                    continue;
                uint32_t start = LoadLE32(&data[offsets + i * 4]);
                size_t length = 0;
                while (start + (length + 1) * 2 <= bufferSize && (data[buffer + start + length * 2] | data[buffer + start + length * 2 + 1]))
                    length++;
                size_t wordLength = std::char_traits<char16_t>::length(word);
                // Not when another key points into this string (shared storage).
                bool shared = false;
                for (uint32_t j = 0; j < count; j++)
                {
                    uint32_t other = LoadLE32(&data[offsets + j * 4]);
                    shared |= j != i && other >= start && other <= start + length * 2;
                }
                if (shared || wordLength > length)
                {
                    Log("[settings] frontend text %c: CALIBRATION label not replaced (%s)", letter, shared ? "shared string" : "too long");
                    continue;
                }
                for (size_t c = 0; c < length; c++)
                {
                    char16_t ch = c < wordLength ? word[c] : 0;
                    data[buffer + start + c * 2] = uint8_t(ch);
                    data[buffer + start + c * 2 + 1] = uint8_t(ch >> 8);
                }
                patched++;
            }
            return true;
        }

        void WalkChunks(std::vector<uint8_t>& data, size_t begin, size_t end, bool inFrontendBible, int& patched)
        {
            size_t offset = begin;
            while (offset + 12 <= end)
            {
                uint32_t id = LoadLE32(&data[offset]), headerSize = LoadLE32(&data[offset + 4]), totalSize = LoadLE32(&data[offset + 8]);
                if (totalSize < 12 || headerSize < 12 || headerSize > totalSize || offset + totalSize > end)
                    return;
                if (id == LANGUAGE && inFrontendBible)
                    PatchLanguage(data, offset, offset + totalSize, patched);
                bool frontend = false;
                if (id == BIBLE && offset + 13 <= end)
                {
                    uint8_t length = data[offset + 12];
                    std::string name(reinterpret_cast<const char*>(&data[offset + 13]), strnlen(reinterpret_cast<const char*>(&data[offset + 13]), length));
                    frontend = name == "frontend";
                }
                WalkChunks(data, offset + headerSize, offset + totalSize, frontend, patched);
                offset += totalSize;
            }
        }

        bool PatchFrontendTexts(std::vector<uint8_t>& data)
        {
            int patched = 0;
            WalkChunks(data, 12, data.size(), false, patched);
            return patched > 0;
        }

        // Pure3D frontend ("Scrooby") pages (0x18002: name, u32 0, u32 width, u32 height) hold
        // layers (0x18020) of elements: texts (0x18023: name, position, size, ..., colour, font,
        // ..., shadow colour), sprites (0x18022: name, ..., colour, image name) and menus (0x18010:
        // name, ..., normal and highlight colours; items 0x18011 holding texts). Colours are
        // B, G, R, A. Names are length-prefixed and NUL-padded. (Hiding a layer through its flag
        // did nothing: the game shows its layers itself.)
        constexpr uint32_t FE_PAGE = 0x18002, FE_MENU = 0x18010, FE_SPRITE = 0x18022, FE_TEXT = 0x18023;

        // Offset of the chunk `id` named `name`, or 0.
        size_t FindNamedChunk(const std::vector<uint8_t>& data, uint32_t id, const std::string& name)
        {
            for (size_t at : FindAll(data, name))
            {
                if (at < 13)
                    continue;
                size_t chunk = at - 13;
                if (LoadLE32(&data[chunk]) == id && data[chunk + 12] >= name.size() &&
                    (data[chunk + 12] == name.size() || data[at + name.size()] == 0))
                    return chunk;
            }
            return 0;
        }

        // Sets the alpha of the colours `colours` (B, G, R, A) in the headers of the `element`
        // chunks under `page` to 0; returns how many were found.
        int ClearColours(std::vector<uint8_t>& data, size_t page, uint32_t element, const std::vector<std::array<uint8_t, 4>>& colours)
        {
            int cleared = 0;
            size_t end = page + LoadLE32(&data[page + 8]);
            for (size_t chunk = page + LoadLE32(&data[page + 4]); chunk + 12 <= end;)
            {
                uint32_t header = LoadLE32(&data[chunk + 4]), size = LoadLE32(&data[chunk + 8]);
                if (header < 12 || size < header || chunk + size > end)
                    break;
                // Every chunk is visited: stepping by the header size enters the children.
                for (size_t at = chunk + 13 + data[chunk + 12]; LoadLE32(&data[chunk]) == element && at + 4 <= chunk + header; at++)
                    for (const auto& colour : colours)
                        if (memcmp(&data[at], colour.data(), 4) == 0)
                        {
                            data[at + 3] = 0;
                            cleared++;
                            at += 3;
                            break;
                        }
                chunk += header == size ? size : header;  // into the children, or past a leaf
            }
            return cleared;
        }

        std::atomic<bool> g_optionsScreenEmptied{ false };

        // The PS2 options page made invisible: its texts (dark brown, and their shadows), the
        // menu's normal / highlight colours and the Pro Logic logo get alpha 0.
        bool PatchOptionsScreen(std::vector<uint8_t>& data)
        {
            size_t page = FindNamedChunk(data, FE_PAGE, "GameStart_OptionsPS2.pag");
            if (!page)
                return false;
            std::vector<uint8_t> copy = data;
            int texts = ClearColours(copy, page, FE_TEXT, { { 0x00, 0x0F, 0x37, 0xE1 }, { 0x00, 0x40, 0x80, 0x32 } });
            int menu = ClearColours(copy, page, FE_MENU, { { 0xEB, 0xDE, 0x94, 0xFF }, { 0xA0, 0x7D, 0x1E, 0xFF } });
            int sprites = ClearColours(copy, page, FE_SPRITE, { { 0xFF, 0xFF, 0xFF, 0xFF } });
            // 7 texts (colour + shadow), the menu's 2 colours, the logo.
            if (texts != 14 || menu != 2 || sprites != 1)
            {
                Log("[settings] frontend: options screen not emptied (%d text, %d menu and %d sprite colours found, expected 14, 2 and 1)",
                    texts, menu, sprites);
                return false;
            }
            data.swap(copy);
            g_optionsScreenEmptied = true;
            Log("[settings] frontend: PS2 options screen emptied for the settings (texts, menu and logo made transparent)");
            return true;
        }
    }

    bool SettingsScreenEmptied()
    {
        return g_optionsScreenEmptied;
    }

    void RegisterFrontendPatches()
    {
        rcf_overlay::RegisterBuiltinPatch("default", "fighttrees\\frontend.bfig", "CALIBRATION abre as configuracoes", PatchFrontendBfig);
        rcf_overlay::RegisterBuiltinPatch("default", "package\\7efdcd91.p3d", "rotulo SETTINGS", PatchFrontendTexts);
        rcf_overlay::RegisterBuiltinPatch("default", "package\\cdd70a8c.p3d", "rotulo SETTINGS", PatchFrontendTexts);
        rcf_overlay::RegisterBuiltinPatch("default", "package\\cdd70a8c.p3d", "tela de opcoes do PS2 vazia", PatchOptionsScreen);
        RegisterPcFrontendPatches();  // EXIT GAME, no storage device button, PC texts
        RegisterTitleLogoPatches();   // the logo with "Definitive Edition" on the title and main menu
    }
}
