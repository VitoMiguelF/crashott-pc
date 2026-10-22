// The frontend as a PC game (pc_frontend.h; runtime/NOTES.md, "Frontend de PC"). Built-in patches
// of default.rcf entries, all found by content (not by offsets) and left alone when the content is
// not what they expect (another version of the game, or a mod that changed that part):
//
// 1. package\cdd70a8c.p3d: a fifth item, QuitGameItem, at the end of the main menu's StartMenu
//    (page GameStart_Start_Xenon.pag), a copy of the CALIBRATION/SETTINGS item showing the
//    persistent bible's FE_Button_Exit_Game_Text (EXIT GAME / SAIR DO JOGO, translated by the game
//    itself); the five items move up so that the menu stays centred. The entry grows: a rewrite.
// 2. fighttrees\Frontend.bfig: the exit for that item. The fight tree's exits are numbered in
//    file order, and its conditions return the number of the exit to take. They are native code
//    (sub_820F9BA8, one case per screen; Frontendlua.blua has the same conditions in Lua, but the
//    game never reads it), so frontend_hooks.cpp makes "A on item 4" return QuitExit(). That exit
//    is the Wii menu's ExitLanguage (its fifth item; number = ExitWii + 5 = 86), never taken on
//    the Xbox: it now goes back to the main menu and its snippet becomes PlaySoundSelect(4343),
//    the marker frontend_hooks.cpp turns into settings::RequestQuit.
//    Also: the "Y STORAGE DEVICE" hints of the save slot screens (frontend and in game; blocks
//    ContentDeviceSelector) are hidden, like the main menu's in frontend_patches.cpp; Y itself is
//    dropped by the hook (IsStorageDeviceExit).
// 3. The text bibles: the save notices ("... do not turn off the Xbox 360 console"), the storage
//    device messages, the gamer profile message that sends the player to the Xbox Guide button
//    and "reconnect the Xbox 360 controller" get PC wording, in English (E and U), French,
//    German, Spanish, Italian and Dutch, over the original string (always shorter) and only when
//    the original still contains the word the table expects. The PT-BR pack has its own texts.

#include "pc_frontend.h"
#include "../diagnostics.h"
#include "../fs/rcf_overlay.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
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

        void StoreLE32(uint8_t* p, uint32_t value)
        {
            p[0] = uint8_t(value);
            p[1] = uint8_t(value >> 8);
            p[2] = uint8_t(value >> 16);
            p[3] = uint8_t(value >> 24);
        }

        std::vector<size_t> FindAll(const std::vector<uint8_t>& data, const std::string& needle, size_t begin = 0, size_t end = SIZE_MAX)
        {
            std::vector<size_t> found;
            auto last = data.begin() + std::min(end, data.size());
            auto it = data.begin() + std::min(begin, data.size());
            while ((it = std::search(it, last, needle.begin(), needle.end())) != last)
            {
                found.push_back(size_t(it - data.begin()));
                ++it;
            }
            return found;
        }

        // Shared between the patchers: the fight tree number of ExitWii when 2 prepared the
        // quit exit, and whether 1 added the item.
        std::atomic<uint32_t> g_quitExitParent{ 0 };
        std::atomic<bool> g_quitItemAdded{ false };
        constexpr uint32_t QUIT_ITEM = QUIT_MENU_INDEX;
        // ExitWii in this version's Frontend.bfig: its fifth exit, 86, is the quit exit.
        constexpr uint32_t KNOWN_WII_EXIT = 0x51;

        // ---- 2. Frontend.bfig ----------------------------------------------------------------

        // Exit record: [0xBC27359D][u32 name length][name, NUL-padded to 4 (no NUL when the
        // length is a multiple of 4)][u32 owner][u32 children][u32][f32 -1][u32][f32 -1][u32 target],
        // then its Lua snippets as [u32 length][text].
        constexpr uint32_t EXIT_RECORD = 0xBC27359D;
        constexpr size_t EXIT_TARGET = 0x18;

        struct Exit
        {
            std::string name;
            size_t fields;  // offset of the owner
            uint32_t owner, target;
        };

        std::vector<Exit> Exits(const std::vector<uint8_t>& data)
        {
            std::vector<Exit> exits;
            const uint8_t magic[4] = { 0xBC, 0x27, 0x35, 0x9D };
            for (size_t at = 0; at + 8 <= data.size(); at++)
            {
                if (memcmp(&data[at], magic, 4) != 0)
                    continue;
                uint32_t length = LoadBE32(&data[at + 4]);
                size_t fields = at + 8 + ((size_t(length) + 3) & ~size_t(3));
                if (length == 0 || length > 64 || fields + EXIT_TARGET + 4 > data.size())
                    continue;
                std::string name(reinterpret_cast<const char*>(&data[at + 8]), length);
                if (std::any_of(name.begin(), name.end(), [](char c) { return c < 0x20 || c > 0x7E; }))
                    continue;
                exits.push_back({ name, fields, LoadBE32(&data[fields]), LoadBE32(&data[fields + EXIT_TARGET]) });
            }
            return exits;
        }

        bool PatchFrontendBfig(std::vector<uint8_t>& data)
        {
            std::vector<Exit> exits = Exits(data);
            auto named = [&](const char* name, uint32_t owner) {
                return std::find_if(exits.begin(), exits.end(), [&](const Exit& e) { return e.name == name && e.owner == owner; }) != exits.end();
            };
            auto deviceSelect = std::find_if(exits.begin(), exits.end(), [](const Exit& e) { return e.name == "ExitShowXenonDeviceSelect"; });
            if (deviceSelect == exits.end())
                return false;
            uint32_t mainMenu = deviceSelect->target;

            // The Wii menu's fifth exit (its siblings: ExitCredits and ExitCalibration).
            const std::string snippet = "Managers.FrontendManager:PlaySoundSelect()\r\n\r\n";
            const std::string marked = "Managers.FrontendManager:PlaySoundSelect(4343)";
            static_assert(sizeof("Managers.FrontendManager:PlaySoundSelect()\r\n\r\n") == sizeof("Managers.FrontendManager:PlaySoundSelect(4343)"));
            bool quitExit = false;
            for (const Exit& exit : exits)
            {
                if (exit.name != "ExitLanguage" || !named("ExitCredits", exit.owner) || !named("ExitCalibration", exit.owner))
                    continue;
                std::vector<size_t> text = FindAll(data, snippet, exit.fields, exit.fields + 0x80);
                if (text.size() != 1 || LoadBE32(&data[text[0] - 4]) != snippet.size())
                    continue;
                StoreBE32(&data[exit.fields + EXIT_TARGET], mainMenu);
                memcpy(&data[text[0]], marked.data(), marked.size());
                g_quitExitParent = exit.owner;
                quitExit = true;
                Log("[pc] frontend: exit %u (Wii ExitLanguage, 0x%X -> main menu 0x%X) closes the game", exit.owner + 5, exit.target, mainMenu);
                break;
            }

            // Y hints of the save slot screens: [0x7B6567EE][page][group][u32 visible] in the
            // ContentDeviceSelector blocks.
            int hidden = 0;
            for (size_t block : FindAll(data, "ContentDeviceSelector"))
            {
                for (const char* group : { "GameStart_DeviceSelector", "InGame_DeviceSelector" })
                {
                    size_t length = strlen(group);
                    for (size_t at : FindAll(data, group, block, block + 0x140))
                    {
                        size_t visible = (at + length + 3) & ~size_t(3);
                        if (at >= 4 && LoadBE32(&data[at - 4]) == length && visible + 4 <= data.size() && LoadBE32(&data[visible]) == 1)
                        {
                            StoreBE32(&data[visible], 0);
                            hidden++;
                        }
                    }
                }
            }
            Log("[pc] frontend: %d storage device hint(s) of the save slot screens hidden", hidden);
            return quitExit || hidden > 0;
        }

        // ---- 1. The menu item ------------------------------------------------------------------

        // Pure3D chunks: u32 id, u32 header size, u32 total size (little-endian); named chunks
        // start with a length-prefixed name padded to 4 (frontend_patches.cpp).
        constexpr uint32_t FE_PAGE = 0x18002, FE_MENU = 0x18010, FE_MENU_ITEM = 0x18011, FE_TEXT = 0x18023, FE_TEXT_REF = 0x1800B;
        constexpr int MENU_STEP = 46;  // between the tops of the last two items (page units, y up)
        constexpr int MENU_SHIFT = 23; // up, to keep the five items centred where the four were

        std::string ChunkName(const std::vector<uint8_t>& data, size_t chunk)
        {
            uint8_t length = data[chunk + 12];
            if (chunk + 13 + length > data.size())
                return {};
            return std::string(reinterpret_cast<const char*>(&data[chunk + 13]), strnlen(reinterpret_cast<const char*>(&data[chunk + 13]), length));
        }

        // Offsets of the chunks from the file's root to the first chunk `id` named `name` under
        // `chunk` (included), or empty.
        bool FindChunk(const std::vector<uint8_t>& data, size_t chunk, uint32_t id, const std::string& name, std::vector<size_t>& path)
        {
            if (chunk + 12 > data.size())
                return false;
            uint32_t header = LoadLE32(&data[chunk + 4]), total = LoadLE32(&data[chunk + 8]);
            if (header < 12 || total < header || chunk + total > data.size())
                return false;
            path.push_back(chunk);
            if (LoadLE32(&data[chunk]) == id && header > 12 && ChunkName(data, chunk) == name)
                return true;
            for (size_t child = chunk + header; child + 12 <= chunk + total;)
            {
                if (FindChunk(data, child, id, name, path))
                    return true;
                uint32_t size = LoadLE32(&data[child + 8]);
                if (size < 12)
                    break;
                child += size;
            }
            path.pop_back();
            return false;
        }

        std::vector<size_t> Children(const std::vector<uint8_t>& data, size_t chunk)
        {
            std::vector<size_t> children;
            size_t end = chunk + LoadLE32(&data[chunk + 8]);
            for (size_t child = chunk + LoadLE32(&data[chunk + 4]); child + 12 <= end;)
            {
                uint32_t size = LoadLE32(&data[child + 8]);
                if (size < 12 || child + size > end)
                    return {};
                children.push_back(child);
                child += size;
            }
            return children;
        }

        std::vector<uint8_t> PascalName(const std::string& name)
        {
            std::vector<uint8_t> out(1 + ((name.size() + 3) & ~size_t(3)), 0);
            out[0] = uint8_t(out.size() - 1);
            memcpy(&out[1], name.data(), name.size());
            return out;
        }

        std::vector<uint8_t> MakeChunk(uint32_t id, const std::vector<uint8_t>& header, const std::vector<std::vector<uint8_t>>& children)
        {
            std::vector<uint8_t> chunk(12);
            chunk.insert(chunk.end(), header.begin(), header.end());
            StoreLE32(&chunk[0], id);
            StoreLE32(&chunk[4], uint32_t(chunk.size()));
            for (const auto& child : children)
                chunk.insert(chunk.end(), child.begin(), child.end());
            StoreLE32(&chunk[8], uint32_t(chunk.size()));
            return chunk;
        }

        // Header bytes after the chunk's name.
        std::vector<uint8_t> HeaderTail(const std::vector<uint8_t>& data, size_t chunk)
        {
            size_t begin = chunk + 13 + data[chunk + 12], end = chunk + LoadLE32(&data[chunk + 4]);
            return begin <= end ? std::vector<uint8_t>(data.begin() + begin, data.begin() + end) : std::vector<uint8_t>();
        }

        // The text element of a menu item: item (0x18011) > 0x18013 > text (0x18023), whose
        // header after the name is u32 18, x, y, width, height, ... (page of 640x480, y up).
        size_t ItemText(const std::vector<uint8_t>& data, size_t item)
        {
            std::vector<size_t> wrapper = Children(data, item);
            if (wrapper.size() != 1)
                return 0;
            std::vector<size_t> text = Children(data, wrapper[0]);
            if (text.size() != 1 || LoadLE32(&data[text[0]]) != FE_TEXT || HeaderTail(data, text[0]).size() < 20)
                return 0;
            return text[0];
        }

        bool AddQuitItem(std::vector<uint8_t>& data)
        {
            std::vector<size_t> path;
            if (data.size() < 12 || !FindChunk(data, 0, FE_PAGE, "GameStart_Start_Xenon.pag", path))
                return false;
            size_t page = path.back();
            std::vector<size_t> menuPath;
            if (!FindChunk(data, page, FE_MENU, "StartMenu", menuPath))
                return false;
            path.insert(path.end(), menuPath.begin() + 1, menuPath.end());
            size_t menu = path.back();
            std::vector<size_t> items = Children(data, menu);
            if (items.size() != QUIT_ITEM || ChunkName(data, items.back()) != "CalibrationItem")
            {
                Log("[pc] frontend: main menu has %zu item(s), not the 4 expected; EXIT GAME not added", items.size());
                return false;
            }
            std::vector<size_t> texts;
            for (size_t item : items)
            {
                if (LoadLE32(&data[item]) != FE_MENU_ITEM || !(texts.emplace_back(ItemText(data, item))))
                    return false;
            }
            size_t calibration = items.back(), calibrationText = texts.back();
            std::vector<size_t> textRef = Children(data, calibrationText);
            if (textRef.size() != 1 || LoadLE32(&data[textRef[0]]) != FE_TEXT_REF)
                return false;

            // The new item: the SETTINGS item's chunks with a new name, one step lower and the
            // EXIT GAME text.
            const std::string name = "QuitGameItem";
            std::vector<uint8_t> itemHeader = PascalName(name), textHeader = PascalName(name), tail;
            tail = HeaderTail(data, calibration);
            itemHeader.insert(itemHeader.end(), tail.begin(), tail.end());
            tail = HeaderTail(data, calibrationText);
            int32_t calibrationY = int32_t(LoadLE32(&tail[8]));
            StoreLE32(&tail[8], uint32_t(calibrationY - MENU_STEP + MENU_SHIFT));
            textHeader.insert(textHeader.end(), tail.begin(), tail.end());
            std::vector<uint8_t> refHeader = PascalName("persistent"), key = PascalName("FE_Button_Exit_Game_Text");
            refHeader.insert(refHeader.end(), key.begin(), key.end());
            size_t wrapper = Children(data, calibration)[0];
            std::vector<uint8_t> wrapperHeader(data.begin() + wrapper + 12, data.begin() + wrapper + LoadLE32(&data[wrapper + 4]));
            std::vector<uint8_t> item = MakeChunk(FE_MENU_ITEM, itemHeader, {
                MakeChunk(LoadLE32(&data[wrapper]), wrapperHeader, { MakeChunk(FE_TEXT, textHeader, { MakeChunk(FE_TEXT_REF, refHeader, {}) }) }) });

            // The four items move up (same size), then the new one goes after the last.
            for (size_t text : texts)
            {
                size_t y = text + 13 + data[text + 12] + 8;
                StoreLE32(&data[y], LoadLE32(&data[y]) + MENU_SHIFT);
            }
            size_t end = menu + LoadLE32(&data[menu + 8]);
            data.insert(data.begin() + end, item.begin(), item.end());
            for (size_t chunk : path)
                StoreLE32(&data[chunk + 8], LoadLE32(&data[chunk + 8]) + uint32_t(item.size()));
            g_quitItemAdded = true;
            Log("[pc] frontend: EXIT GAME added to the main menu (item %u, persistent FE_Button_Exit_Game_Text)", QUIT_ITEM);
            return true;
        }

        // ---- 3. Texts --------------------------------------------------------------------------

        struct PcText
        {
            const char* bible;
            uint32_t key;
            char letter;  // 'E' also covers 'U' (English, United Kingdom)
            const char16_t* marker;  // the original must contain it
            const char16_t* text;
        };

        const PcText PC_TEXTS[] = {
            { "frontend", 0x1737, 'E', u"Xbox", u"Accessing Crash of the Titans save game files. Please do not turn off your PC." },
            { "frontend", 0x1737, 'F', u"Xbox", u"Accès aux données Crash of the Titans. Ne pas éteindre le PC." },
            { "frontend", 0x1737, 'G', u"Xbox", u"Zugriff auf CRASH OF THE TITANS-Speicherdaten. Bitte den PC nicht ausschalten." },
            { "frontend", 0x1737, 'S', u"Xbox", u"Accediendo a los datos guardados de Crash of the Titans. No apagues el PC." },
            { "frontend", 0x1737, 'I', u"Xbox", u"Accesso ai file di gioco salvati di Crash of the Titans in corso. Non spegnere il PC." },
            { "frontend", 0x1737, 'D', u"console", u"Opgeslagen spelgegevens van Crash of the Titans worden geopend. Zet de pc niet uit." },
            { "frontend", 0x3B85, 'E', u"Xbox", u"Saving Crash of the Titans game, please do not turn off your PC." },
            { "frontend", 0x3B85, 'F', u"Xbox", u"Sauvegarde des données Crash of the Titans, ne pas éteindre le PC." },
            { "frontend", 0x3B85, 'G', u"Xbox", u"CRASH OF THE TITANS-Spiel wird gespeichert, bitte den PC nicht ausschalten." },
            { "frontend", 0x3B85, 'S', u"Xbox", u"Guardando partida de Crash of the Titans. No apagues el PC." },
            { "frontend", 0x3B85, 'I', u"Xbox", u"Salvataggio della partita di Crash of the Titans in corso. Non spegnere il PC." },
            { "frontend", 0x3B85, 'D', u"Xbox", u"Bezig met opslaan spel Crash of the Titans. Schakel de pc niet uit." },
            { "frontend", 0xAE13, 'E', u"Xbox", u"Loading Crash of the Titans save game, please do not turn off your PC." },
            { "frontend", 0xAE13, 'F', u"Xbox", u"Chargement des données Crash of the Titans, ne pas éteindre le PC." },
            { "frontend", 0xAE13, 'G', u"Xbox", u"CRASH OF THE TITANS-Spielstand wird geladen, bitte den PC nicht ausschalten." },
            { "frontend", 0xAE13, 'S', u"Xbox", u"Cargando partida guardada de Crash of the Titans. No apagues el PC." },
            { "frontend", 0xAE13, 'I', u"Xbox", u"Caricamento della partita salvata di Crash of the Titans in corso. Non spegnere il PC." },
            { "frontend", 0xAE13, 'D', u"Xbox", u"Bezig met laden opgeslagen spel Crash of the Titans. Schakel de pc niet uit." },
            { "frontend", 0x4820, 'E', u"storage device", u"No save folder is available. Without it you will be unable to save your game progress." },
            { "frontend", 0x4820, 'F', u"stockage", u"Aucun dossier de sauvegarde n'est disponible. Sans lui, vous ne pouvez pas sauvegarder vos progrès dans le jeu." },
            { "frontend", 0x4820, 'G', u"Speichergerät", u"Kein Speicherort verfügbar. Ohne ihn kannst du deinen Spielfortschritt nicht speichern." },
            { "frontend", 0x4820, 'S', u"unidad de memoria", u"No hay ninguna carpeta de guardado disponible. Sin ella, no podrás guardar tu progreso." },
            { "frontend", 0x4820, 'I', u"periferica", u"Nessuna cartella di salvataggio disponibile. Senza di essa non potrai salvare i progressi di gioco." },
            { "frontend", 0x4820, 'D', u"opslagapparaat", u"Er is geen opslagmap beschikbaar. Zonder opslagmap kun je je spelvoortgang niet opslaan." },
            { "frontend", 0x4AAE, 'E', u"Storage Device", u"The save folder is not available." },
            { "frontend", 0x4AAE, 'F', u"stockage", u"Le dossier de sauvegarde est indisponible." },
            { "frontend", 0x4AAE, 'G', u"Speichergerät", u"Speicherort nicht verfügbar." },
            { "frontend", 0x4AAE, 'S', u"unidad de memoria", u"No se encuentra la carpeta de guardado." },
            { "frontend", 0x4AAE, 'I', u"periferica", u"La cartella di salvataggio non è disponibile." },
            { "frontend", 0x4AAE, 'D', u"Opslagapparaat", u"Opslagmap niet beschikbaar." },
            { "frontend", 0xB5DF, 'E', u"Storage Device", u"The save folder is not available. Please Retry or Continue Without Saving." },
            { "frontend", 0xB5DF, 'F', u"stockage", u"Le dossier de sauvegarde est indisponible. Réessayer ou Continuer sans sauvegarder." },
            { "frontend", 0xB5DF, 'G', u"Speichergerät", u"Speicherort nicht verfügbar. Bitte erneut versuchen oder ohne zu speichern fortfahren." },
            { "frontend", 0xB5DF, 'S', u"unidad de memoria", u"No se encuentra la carpeta de guardado. Vuelve a intentarlo o continúa sin guardar." },
            { "frontend", 0xB5DF, 'I', u"periferica", u"La cartella di salvataggio non è disponibile. Riprova o continua senza salvare." },
            { "frontend", 0xB5DF, 'D', u"Opslagapparaat", u"Opslagmap niet beschikbaar. Probeer het opnieuw of ga door zonder op te slaan." },
            { "frontend", 0xB841, 'E', u"Xbox", u"You do not have an active player profile. You will be unable to save your game progress without an active profile." },
            { "frontend", 0xB841, 'F', u"Xbox", u"Vous n'avez pas de profil de joueur actif. Vous ne pourrez pas sauvegarder vos progrès sans profil de joueur actif." },
            { "frontend", 0xB841, 'G', u"Xbox", u"Du hast kein aktives Spielerprofil. Ohne aktives Spielerprofil kannst du deinen Spielfortschritt nicht speichern." },
            { "frontend", 0xB841, 'S', u"Xbox", u"No tienes un perfil de jugador activo. No podrás guardar la partida hasta que tengas un perfil de jugador activo." },
            { "frontend", 0xB841, 'I', u"Xbox", u"Non hai un profilo giocatore attivo. Senza profilo giocatore attivo, non puoi salvare i tuoi progressi di gioco." },
            { "frontend", 0xB841, 'D', u"Xbox", u"U hebt geen actief profiel. U kunt het spel niet opslaan zonder actief profiel." },
            { "ingame", 0x1737, 'E', u"Xbox", u"Accessing Crash of the Titans save game files. Please do not turn off your PC." },
            { "ingame", 0x1737, 'F', u"Xbox", u"Accès aux données Crash of the Titans. Ne pas éteindre le PC." },
            { "ingame", 0x1737, 'G', u"Xbox", u"Zugriff auf CRASH OF THE TITANS-Speicherdaten. Bitte den PC nicht ausschalten." },
            { "ingame", 0x1737, 'S', u"Xbox", u"Accediendo a los datos guardados de Crash of the Titans. No apagues el PC." },
            { "ingame", 0x1737, 'I', u"Xbox", u"Accesso ai file di gioco salvati di Crash of the Titans in corso. Non spegnere il PC." },
            { "ingame", 0x1737, 'D', u"console", u"Opgeslagen spelgegevens van Crash of the Titans worden geopend. Zet de pc niet uit." },
            { "ingame", 0x3266, 'E', u"Xbox", u"Saving Crash of the Titans game, please do not turn off your PC." },
            { "ingame", 0x3266, 'F', u"console", u"Enregistrement en cours. N'éteignez pas votre PC." },
            { "ingame", 0x3266, 'G', u"Xbox", u"CRASH OF THE TITANS-Spiel wird gespeichert, bitte den PC nicht ausschalten." },
            { "ingame", 0x3266, 'S', u"Xbox", u"Guardando partida de Crash of the Titans. No apagues el PC." },
            { "ingame", 0x3266, 'I', u"Xbox", u"Salvataggio della partita di Crash of the Titans in corso. Non spegnere il PC." },
            { "ingame", 0x3266, 'D', u"console", u"De content wordt opgeslagen. Zet de pc niet uit." },
            { "ingame", 0x4820, 'E', u"storage device", u"The save folder is not available. Without it you will be unable to save your game progress." },
            { "ingame", 0x4820, 'F', u"stockage", u"Le dossier de sauvegarde est indisponible. Sans lui, vous ne pouvez pas sauvegarder vos progrès dans le jeu." },
            { "ingame", 0x4820, 'G', u"Speichergerät", u"Speicherort nicht verfügbar. Ohne ihn kannst du deinen Spielfortschritt nicht speichern." },
            { "ingame", 0x4820, 'S', u"unidad de memoria", u"No se encuentra la carpeta de guardado. Sin ella, no podrás guardar tu progreso." },
            { "ingame", 0x4820, 'I', u"periferica", u"La cartella di salvataggio non è disponibile. Senza di essa non potrai salvare i progressi di gioco." },
            { "ingame", 0x4820, 'D', u"opslagapparaat", u"Opslagmap niet beschikbaar. Zonder opslagmap kun je je spelvoortgang niet opslaan." },
            { "ingame", 0x6C7E, 'E', u"Storage Device", u"Save folder not available.  Would you like to continue without saving?" },
            { "ingame", 0x6C7E, 'F', u"stockage", u"Dossier de sauvegarde indisponible. Continuer sans sauvegarder ?" },
            { "ingame", 0x6C7E, 'G', u"Speichergerät", u"Speicherort nicht verfügbar. Möchtest du ohne zu speichern fortfahren?" },
            { "ingame", 0x6C7E, 'S', u"unidad de memoria", u"No se encuentra la carpeta de guardado. ¿Seguro que quieres continuar sin guardar?" },
            { "ingame", 0x6C7E, 'I', u"eriferica", u"Cartella di salvataggio non disponibile. Continuare senza salvare?" },
            { "ingame", 0x6C7E, 'D', u"Opslagapparaat", u"Opslagmap niet beschikbaar. Wil je doorgaan zonder op te slaan?" },
            { "persistent", 0x5823, 'E', u"Xbox", u"Please reconnect the controller to continue." },
            { "persistent", 0x5823, 'F', u"Xbox", u"Reconnectez la manette pour continuer." },
            { "persistent", 0x5823, 'G', u"Xbox", u"Bitte den Controller wieder anschließen." },
            { "persistent", 0x5823, 'S', u"Xbox", u"Vuelve a conectar el mando para continuar." },
            { "persistent", 0x5823, 'I', u"Xbox", u"Riconnetti il controller per continuare." },
            { "persistent", 0x5823, 'D', u"Xbox", u"Verbind de controller opnieuw om verder te gaan." },
        };

        constexpr uint32_t BIBLE = 0x1800D, LANGUAGE = 0x1800E;

        std::u16string ReadString(const std::vector<uint8_t>& data, size_t at, size_t end)
        {
            std::u16string text;
            for (; at + 2 <= end && (data[at] | data[at + 1]); at += 2)
                text += char16_t(data[at] | (data[at + 1] << 8));
            return text;
        }

        void PatchLanguage(std::vector<uint8_t>& data, size_t offset, size_t end, const std::string& bible, int& patched)
        {
            size_t pos = offset + 12;
            if (pos >= end)
                return;
            pos += 1 + data[pos];  // pascal name
            if (pos + 13 > end)
                return;
            char letter = char(data[pos]);
            uint32_t count = LoadLE32(&data[pos + 1]);
            uint32_t bufferSize = LoadLE32(&data[pos + 9]);
            size_t hashes = pos + 13, offsets = hashes + size_t(count) * 4, buffer = offsets + size_t(count) * 4;
            if (buffer + bufferSize > end)
                return;
            for (const PcText& entry : PC_TEXTS)
            {
                if (bible != entry.bible || (entry.letter != letter && !(entry.letter == 'E' && letter == 'U')))
                    continue;
                for (uint32_t i = 0; i < count; i++)
                {
                    if (LoadLE32(&data[hashes + i * 4]) != entry.key)
                        continue;
                    uint32_t start = LoadLE32(&data[offsets + i * 4]);
                    if (start >= bufferSize)
                        break;
                    std::u16string original = ReadString(data, buffer + start, buffer + bufferSize);
                    std::u16string text = entry.text;
                    bool shared = false;
                    for (uint32_t j = 0; j < count; j++)
                    {
                        uint32_t other = LoadLE32(&data[offsets + j * 4]);
                        shared |= j != i && other >= start && other <= start + original.size() * 2;
                    }
                    if (original.find(entry.marker) == std::u16string::npos || text.size() > original.size() || shared)
                        break;
                    for (size_t c = 0; c < original.size(); c++)
                    {
                        char16_t ch = c < text.size() ? text[c] : 0;
                        data[buffer + start + c * 2] = uint8_t(ch);
                        data[buffer + start + c * 2 + 1] = uint8_t(ch >> 8);
                    }
                    patched++;
                    break;
                }
            }
        }

        void WalkTexts(std::vector<uint8_t>& data, size_t begin, size_t end, const std::string& bible, int& patched)
        {
            for (size_t offset = begin; offset + 12 <= end;)
            {
                uint32_t id = LoadLE32(&data[offset]), headerSize = LoadLE32(&data[offset + 4]), totalSize = LoadLE32(&data[offset + 8]);
                if (totalSize < 12 || headerSize < 12 || headerSize > totalSize || offset + totalSize > end)
                    return;
                if (id == LANGUAGE && !bible.empty())
                    PatchLanguage(data, offset, offset + totalSize, bible, patched);
                std::string child = id == BIBLE && offset + 13 <= end ? ChunkName(data, offset) : std::string();
                WalkTexts(data, offset + headerSize, offset + totalSize, child, patched);
                offset += totalSize;
            }
        }

        bool PatchTexts(std::vector<uint8_t>& data)
        {
            int patched = 0;
            WalkTexts(data, 12, data.size(), {}, patched);
            return patched > 0;
        }

        // The packages with the frontend, in-game and persistent bibles the Xbox version reads
        // (English and UK in 7a8185b0 / 7a88cba0, one package per language for the in-game ones).
        const char* const TEXT_PACKAGES[] = {
            "package\\7efdcd91.p3d", "package\\cdd70a8c.p3d", "package\\b4c85fe7.p3d", "package\\c1e387c7.p3d",
            "package\\97597d1b.p3d", "package\\7a8185b0.p3d", "package\\7a88cba0.p3d", "package\\7a81fa0f.p3d",
            "package\\7a826e6e.p3d", "package\\7a87e2e2.p3d", "package\\7a83572c.p3d", "package\\7a811151.p3d",
        };
    }

    int QuitExit()
    {
        uint32_t parent = g_quitExitParent;
        return parent == KNOWN_WII_EXIT && g_quitItemAdded ? int(parent + 5) : -1;
    }

    bool IsStorageDeviceExit(int exit)
    {
        // ExitShowXenonDeviceSelect of the main menu, the save slot screen and the one in game
        // (Y; the numbers of this version's Frontend.bfig, checked through the Wii exit above).
        return g_quitExitParent == KNOWN_WII_EXIT && (exit == 94 || exit == 265 || exit == 523);
    }

    void RegisterPcFrontendPatches()
    {
        rcf_overlay::RegisterBuiltinPatch("default", "fighttrees\\frontend.bfig", "SAIR DO JOGO e Y de armazenamento", PatchFrontendBfig);
        rcf_overlay::RegisterEntryRewrite("default", "package\\cdd70a8c.p3d", "item SAIR DO JOGO", AddQuitItem);
        for (const char* package : TEXT_PACKAGES)
            rcf_overlay::RegisterBuiltinPatch("default", package, "textos de PC (sem Xbox 360)", PatchTexts);
    }
}
