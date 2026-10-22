#include "controls.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace settings
{
    namespace
    {
        struct ActionInfo
        {
            const char* id;
            const char* english;
            const char* portuguese;
            uint16_t keys[KEYS_PER_ACTION];
        };
        // Same order as Action.
        constexpr ActionInfo ACTIONS[] = {
            { "move_forward", "MOVE FORWARD", "ANDAR PARA FRENTE", { 'W', 0 } },
            { "move_back", "MOVE BACK", "ANDAR PARA TRÁS", { 'S', 0 } },
            { "move_left", "MOVE LEFT", "ANDAR PARA A ESQUERDA", { 'A', 0 } },
            { "move_right", "MOVE RIGHT", "ANDAR PARA A DIREITA", { 'D', 0 } },
            { "walk", "WALK", "ANDAR DEVAGAR", { VK_LMENU, 0 } },
            { "jump", "JUMP", "PULAR", { VK_SPACE, 0 } },
            { "light_attack", "LIGHT ATTACK", "ATAQUE LEVE", { VK_LBUTTON, 'J' } },
            { "heavy_attack", "HEAVY ATTACK", "ATAQUE PESADO", { VK_RBUTTON, 'K' } },
            { "interact", "ACTION / JACK", "AÇÃO / MONTAR", { 'E', 'L' } },
            { "block", "BLOCK / DODGE", "BLOQUEAR / ESQUIVAR", { VK_LSHIFT, VK_LCONTROL } },
            { "rotate_stick", "ROTATE STICK", "GIRAR ANALÓGICO", { 'Q', 0 } },
            { "spin_attack", "SPIN ATTACK", "ATAQUE GIRATÓRIO", { 'R', VK_MBUTTON } },
            { "finisher", "FINISHING MOVE", "GOLPE FINAL", { 'F', 0 } },
            { "modifier", "MODIFIER (LB)", "MODIFICADOR (LB)", { 'C', 0 } },
            { "left_trigger", "LEFT TRIGGER (LT)", "GATILHO ESQUERDO (LT)", { 'Z', 0 } },
            { "camera_up", "CAMERA UP", "CÂMERA PARA CIMA", { VK_UP, 0 } },
            { "camera_down", "CAMERA DOWN", "CÂMERA PARA BAIXO", { VK_DOWN, 0 } },
            { "camera_left", "CAMERA LEFT", "CÂMERA PARA A ESQUERDA", { VK_LEFT, 0 } },
            { "camera_right", "CAMERA RIGHT", "CÂMERA PARA A DIREITA", { VK_RIGHT, 0 } },
            { "pause", "PAUSE", "PAUSA", { VK_ESCAPE, 0 } },
            { "map", "BACK / MAP", "VOLTAR / MAPA", { VK_TAB, 0 } },
        };
        static_assert(std::size(ACTIONS) == ACTION_COUNT);

        struct KeyNameEntry
        {
            uint16_t key;
            const char* name;
        };
        constexpr KeyNameEntry KEY_NAMES[] = {
            { VK_LBUTTON, "MOUSE1" }, { VK_RBUTTON, "MOUSE2" }, { VK_MBUTTON, "MOUSE3" }, { VK_XBUTTON1, "MOUSE4" },
            { VK_XBUTTON2, "MOUSE5" }, { KEY_WHEEL_UP, "WHEELUP" }, { KEY_WHEEL_DOWN, "WHEELDOWN" },
            { VK_SPACE, "SPACE" }, { VK_RETURN, "ENTER" }, { VK_ESCAPE, "ESC" }, { VK_TAB, "TAB" }, { VK_BACK, "BACKSPACE" },
            { VK_LSHIFT, "LSHIFT" }, { VK_RSHIFT, "RSHIFT" }, { VK_LCONTROL, "LCTRL" }, { VK_RCONTROL, "RCTRL" },
            { VK_LMENU, "LALT" }, { VK_RMENU, "RALT" }, { VK_CAPITAL, "CAPSLOCK" },
            { VK_UP, "UP" }, { VK_DOWN, "DOWN" }, { VK_LEFT, "LEFT" }, { VK_RIGHT, "RIGHT" },
            { VK_INSERT, "INSERT" }, { VK_DELETE, "DELETE" }, { VK_HOME, "HOME" }, { VK_END, "END" },
            { VK_PRIOR, "PAGEUP" }, { VK_NEXT, "PAGEDOWN" },
            { VK_OEM_1, "SEMICOLON" }, { VK_OEM_PLUS, "EQUALS" }, { VK_OEM_COMMA, "COMMA" }, { VK_OEM_MINUS, "MINUS" },
            { VK_OEM_PERIOD, "PERIOD" }, { VK_OEM_2, "SLASH" }, { VK_OEM_3, "GRAVE" }, { VK_OEM_4, "LBRACKET" },
            { VK_OEM_5, "BACKSLASH" }, { VK_OEM_6, "RBRACKET" }, { VK_OEM_7, "APOSTROPHE" }, { VK_OEM_102, "OEM102" },
            { VK_ADD, "NUMPLUS" }, { VK_SUBTRACT, "NUMMINUS" }, { VK_MULTIPLY, "NUMSTAR" }, { VK_DIVIDE, "NUMSLASH" },
            { VK_DECIMAL, "NUMPERIOD" },
        };

        std::string Upper(std::string text)
        {
            for (char& c : text)
                c = char(std::toupper(uint8_t(c)));
            return text;
        }

        std::string Trim(const std::string& text)
        {
            size_t begin = text.find_first_not_of(" \t\r\n");
            size_t end = text.find_last_not_of(" \t\r\n");
            return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
        }

        bool ParseBool(const std::string& text, bool& out)
        {
            if (text == "true")
                out = true;
            else if (text == "false")
                out = false;
            else
                return false;
            return true;
        }

        bool ParseInt(const std::string& text, int& out, int minimum, int maximum)
        {
            char* end;
            long value = strtol(text.c_str(), &end, 10);
            if (end == text.c_str() || *end)
                return false;
            out = int(std::clamp(value, long(minimum), long(maximum)));
            return true;
        }
    }

    Controls::Controls()
    {
        for (size_t i = 0; i < ACTION_COUNT; i++)
            for (size_t k = 0; k < KEYS_PER_ACTION; k++)
                keys[i][k] = ACTIONS[i].keys[k];
    }

    const char* ActionId(Action action)
    {
        return ACTIONS[size_t(action)].id;
    }

    const char* ActionLabel(Action action, bool portuguese)
    {
        return portuguese ? ACTIONS[size_t(action)].portuguese : ACTIONS[size_t(action)].english;
    }

    std::string KeyName(uint16_t key)
    {
        if (!key)
            return {};
        if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
            return std::string(1, char(key));
        if (key >= VK_F1 && key <= VK_F24)
            return "F" + std::to_string(key - VK_F1 + 1);
        if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
            return "NUM" + std::to_string(key - VK_NUMPAD0);
        for (const KeyNameEntry& entry : KEY_NAMES)
        {
            if (entry.key == key)
                return entry.name;
        }
        char text[16];
        snprintf(text, sizeof(text), "VK_%02X", key);
        return text;
    }

    bool ParseKey(const std::string& raw, uint16_t& key)
    {
        std::string name = Upper(Trim(raw));
        if (name.empty() || name == "NONE")
        {
            key = 0;
            return true;
        }
        if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
        {
            key = uint16_t(name[0]);
            return true;
        }
        // Aliases.
        if (name == "SHIFT") name = "LSHIFT";
        else if (name == "CTRL" || name == "CONTROL") name = "LCTRL";
        else if (name == "ALT") name = "LALT";
        else if (name == "ESCAPE") name = "ESC";
        else if (name == "RETURN") name = "ENTER";
        else if (name == "LMB") name = "MOUSE1";
        else if (name == "RMB") name = "MOUSE2";
        else if (name == "MMB") name = "MOUSE3";
        for (const KeyNameEntry& entry : KEY_NAMES)
        {
            if (name == entry.name)
            {
                key = entry.key;
                return true;
            }
        }
        if (name.size() >= 2 && name[0] == 'F' && isdigit(uint8_t(name[1])))
        {
            int number = atoi(name.c_str() + 1);
            if (number >= 1 && number <= 24)
            {
                key = uint16_t(VK_F1 + number - 1);
                return true;
            }
        }
        if (name.size() == 4 && name.compare(0, 3, "NUM") == 0 && isdigit(uint8_t(name[3])))
        {
            key = uint16_t(VK_NUMPAD0 + name[3] - '0');
            return true;
        }
        if (name.size() == 5 && name.compare(0, 3, "VK_") == 0)
        {
            key = uint16_t(strtoul(name.c_str() + 3, nullptr, 16));
            return key != 0;
        }
        return false;
    }

    const char* PromptStyleName(PromptStyle style)
    {
        switch (style)
        {
        case PromptStyle::Xbox: return "xbox";
        case PromptStyle::PlayStation: return "playstation";
        case PromptStyle::Keyboard: return "keyboard";
        default: return "auto";
        }
    }

    bool ApplyControl(Controls& controls, const std::string& key, const std::string& value, bool& valid)
    {
        valid = true;
        std::string lower = value;
        for (char& c : lower)
            c = char(std::tolower(uint8_t(c)));
        for (size_t i = 0; i < ACTION_COUNT; i++)
        {
            if (key != ACTIONS[i].id)
                continue;
            std::array<uint16_t, KEYS_PER_ACTION> keys{};
            std::istringstream list(value);
            std::string item;
            size_t count = 0;
            while (std::getline(list, item, ','))
            {
                uint16_t code;
                if (!ParseKey(item, code))
                {
                    valid = false;
                    return true;
                }
                if (code && count < KEYS_PER_ACTION)
                    keys[count++] = code;
            }
            controls.keys[i] = keys;
            return true;
        }
        if (key == "mouse_sensitivity")
            valid = ParseInt(lower, controls.mouseSensitivity, 10, 400);
        else if (key == "mouse_camera")
            valid = ParseBool(lower, controls.mouseCamera);
        else if (key == "invert_y")
            valid = ParseBool(lower, controls.invertY);
        else if (key == "walk_speed")
            valid = ParseInt(lower, controls.walkSpeed, 10, 90);
        else if (key == "mouse_menus")
            valid = ParseBool(lower, controls.mouseMenus);
        else if (key == "player1" || key == "player2")
        {
            valid = ValidPlayerDevice(lower);
            if (valid)
                controls.players[key == "player2"] = lower;
        }
        else if (key == "prompts")
        {
            valid = false;
            for (PromptStyle style : { PromptStyle::Auto, PromptStyle::Xbox, PromptStyle::PlayStation, PromptStyle::Keyboard })
            {
                if (lower == PromptStyleName(style))
                {
                    controls.prompts = style;
                    valid = true;
                }
            }
        }
        else
            return false;
        return true;
    }

    bool ValidPlayerDevice(const std::string& key)
    {
        if (key == "auto")
            return true;
        for (std::string prefix : { "xbox", "playstation" })
        {
            if (key.size() == prefix.size() + 1 && key.compare(0, prefix.size(), prefix) == 0 && key.back() >= '1' && key.back() <= '4')
                return true;
        }
        return false;
    }

    std::string SerializeControls(const Controls& c)
    {
        std::ostringstream out;
        out << "[controls]\n"
               "# Teclado e mouse na gameplay. Cada acao: ate duas teclas separadas por virgula (\"none\" = nenhuma).\n"
               "# Nomes: A-Z, 0-9, F1-F24, SPACE, ENTER, ESC, TAB, BACKSPACE, LSHIFT, RSHIFT, LCTRL, RCTRL, LALT, RALT,\n"
               "# UP, DOWN, LEFT, RIGHT, MOUSE1 (esquerdo), MOUSE2 (direito), MOUSE3 (meio), MOUSE4, MOUSE5, WHEELUP, WHEELDOWN...\n"
               "# Nos menus as teclas sao fixas: setas/WASD navegam, Enter/Espaco = A, Esc = B (voltar), Backspace = X, Tab = Y, P = START, Q/E = LB/RB.\n";
        for (size_t i = 0; i < ACTION_COUNT; i++)
        {
            std::string keys;
            for (uint16_t key : c.keys[i])
            {
                if (key)
                    keys += (keys.empty() ? "" : ", ") + KeyName(key);
            }
            out << ACTIONS[i].id << " = \"" << (keys.empty() ? "none" : keys) << "\"\n";
        }
        out << "mouse_camera = " << (c.mouseCamera ? "true" : "false") << "   # o mouse gira a camera na gameplay\n"
            << "mouse_sensitivity = " << c.mouseSensitivity << "   # 10-400 %, mouse -> camera\n"
            << "invert_y = " << (c.invertY ? "true" : "false") << "   # inverte o eixo vertical da camera (mouse e teclas)\n"
            << "walk_speed = " << c.walkSpeed << "   # 10-90 %, analogico enquanto 'walk' esta apertado (andar em vez de correr)\n"
            << "mouse_menus = " << (c.mouseMenus ? "true" : "false") << "   # menus do jogo pelo mouse (passar por cima seleciona, clique confirma, botao direito volta)\n"
            << "prompts = \"" << PromptStyleName(c.prompts) << "\"   # icones dos botoes: auto (o ultimo usado) | xbox | playstation | keyboard\n"
            << "player1 = \"" << c.players[0] << "\"   # co-op local, controle do jogador 1: auto | xbox1..xbox4 (XInput) | playstation1..playstation4\n"
            << "player2 = \"" << c.players[1] << "\"   # controle do jogador 2 (com um controle so, ele e o jogador 1, junto com o teclado)\n";
        return out.str();
    }
}
