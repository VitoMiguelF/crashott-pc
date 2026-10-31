// Physical controllers -> game controllers, for local co-op (pad_slots.h).
//
// The game polls the four XamInput users every tick (sub_8228A7C8, one controller object per user,
// connection flag at +40). A second connected user makes the HUD offer "JOIN GAME (B)": B joins
// player 2, who rides in player 1's backpack until Y (EInputMapEvent_DETACH) jumps off; START on
// that pad pauses as "P2 PAUSED" (DROP OUT leaves). Player 1 is the controller that pressed START
// on the title screen: sub_821DB680 checks the four and calls sub_821FC498(managers, index), which
// keeps the index at managers+348 (and the profile of that user at +328/+332).

#include "pad_slots.h"
#include "hid_pads.h"
#include "xinput_backend.h"
#include "kernel.h"
#include "pc_input.h"
#include "../settings/settings.h"
#include "../platform/platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace input::slots
{
    namespace
    {
        constexpr int USERS = 4;
        constexpr uint64_t UPDATE_MS = 3;            // reads closer than this share one update
        constexpr uint64_t PROBE_MS = 1000;          // an empty XInput slot is probed this often
        // A PlayStation pad's press and an XInput pad's press of as many buttons count as the same
        // press (a virtual pad of DS4Windows/DSX) when the XInput one comes at most this much
        // before / after it.
        constexpr uint64_t MATCH_BEFORE_MS = 40, MATCH_AFTER_MS = 150;

        using Choices = std::array<std::string, 2>;

        // Merges `from` into `into`: buttons OR'd, the trigger or stick pushed further wins (sticks as
        // pairs, so a centred pad does not cut the other one's diagonal).
        void Merge(XINPUT_GAMEPAD& into, const XINPUT_GAMEPAD& from)
        {
            into.wButtons |= from.wButtons;
            into.bLeftTrigger = std::max(into.bLeftTrigger, from.bLeftTrigger);
            into.bRightTrigger = std::max(into.bRightTrigger, from.bRightTrigger);
            if (std::hypot(from.sThumbLX, from.sThumbLY) > std::hypot(into.sThumbLX, into.sThumbLY))
                into.sThumbLX = from.sThumbLX, into.sThumbLY = from.sThumbLY;
            if (std::hypot(from.sThumbRX, from.sThumbRY) > std::hypot(into.sThumbRX, into.sThumbRY))
                into.sThumbRX = from.sThumbRX, into.sThumbRY = from.sThumbRY;
        }

        // What one update sees: the XInput slots and the PlayStation pads.
        struct Snapshot
        {
            uint64_t now = 0;
            bool connected[USERS] = {};
            XINPUT_GAMEPAD xinput[USERS] = {};
            std::vector<std::pair<hid::PadInfo, XINPUT_GAMEPAD>> pads;
        };

        struct Press
        {
            uint64_t time;
            uint16_t buttons;
        };

        // A PlayStation pad: on its own (Separate), the same pad as an XInput one (Mirror), or not
        // known yet (Unknown: it drives nothing until its presses tell).
        enum class Mode : uint8_t { Unknown, Separate, Mirror };

        struct Hid
        {
            uint32_t id;
            std::string name;
            Mode mode = Mode::Unknown;
            uint32_t recheck = 0;  // XInput slots that appeared after it was found separate: is it one of them?
            XINPUT_GAMEPAD state{};
            uint16_t lastButtons = 0;
            std::vector<Press> pending;  // its presses not compared yet
            int matches[USERS] = {};
            int misses = 0;
        };

        // A device: an XInput slot, a PlayStation pad, or both (a pad and its virtual Xbox pad).
        struct Dev
        {
            int slot = -1;
            uint32_t hid = 0;
            int player = -1;
        };

        class Table
        {
        public:
            uint32_t playerOne = 0;  // the game controller of player 1

            void Update(const Snapshot& s, const Choices& choices)
            {
                choices_ = choices;
                uint32_t connectedMask = 0;
                for (int k = 0; k < USERS; k++)
                {
                    if (s.connected[k])
                        connectedMask |= 1u << k;
                    uint16_t buttons = s.connected[k] ? s.xinput[k].wButtons : 0;
                    if (uint16_t down = buttons & ~xLast_[k])
                        xPresses_[k].push_back({ s.now, down });
                    std::erase_if(xPresses_[k], [&](const Press& p) { return p.time + MATCH_BEFORE_MS + MATCH_AFTER_MS + 100 < s.now; });
                    xLast_[k] = buttons;
                    xState_[k] = s.connected[k] ? s.xinput[k] : XINPUT_GAMEPAD{};
                    if (s.connected[k] == xConnected_[k])
                        continue;
                    xConnected_[k] = s.connected[k];
                    if (s.connected[k])
                    {
                        Log("[input] controle XInput %d conectado", k + 1);
                        devs_.push_back({ k });
                        for (Hid& h : hids_)
                        {
                            if (h.mode == Mode::Separate)
                                h.recheck |= 1u << k;
                        }
                    }
                    else
                    {
                        Log("[input] controle XInput %d desconectado", k + 1);
                        xPresses_[k].clear();
                        for (Hid& h : hids_)
                            h.recheck &= ~(1u << k);
                        if (Dev* d = BySlot(k); d && d->hid)
                        {
                            // DS4Windows/DSX closed: the PlayStation pad carries on alone.
                            d->slot = -1;
                            if (Hid* h = HidOf(d->hid))
                                h->mode = Mode::Separate;
                        }
                        else
                            std::erase_if(devs_, [&](const Dev& d) { return d.slot == k; });
                    }
                }
                // PlayStation pads that left, then the new ones.
                for (auto it = hids_.begin(); it != hids_.end();)
                {
                    bool present = std::any_of(s.pads.begin(), s.pads.end(), [&](const auto& p) { return p.first.id == it->id; });
                    if (present)
                    {
                        ++it;
                        continue;
                    }
                    if (Dev* d = ByHid(it->id); d && d->slot >= 0)
                        d->hid = 0;
                    else
                        std::erase_if(devs_, [&](const Dev& d) { return d.hid == it->id; });
                    it = hids_.erase(it);
                }
                for (const auto& [info, state] : s.pads)
                {
                    Hid* h = HidOf(info.id);
                    if (!h)
                    {
                        hids_.push_back({ info.id, info.name });
                        h = &hids_.back();
                        h->lastButtons = state.wButtons;  // held while plugged in: not a press
                    }
                    h->state = state;
                    uint16_t down = state.wButtons & ~h->lastButtons;
                    h->lastButtons = state.wButtons;
                    if (h->mode == Mode::Unknown && !connectedMask)
                    {
                        Decide(*h, Mode::Separate, -1);
                        continue;
                    }
                    uint32_t candidates = (h->mode == Mode::Unknown ? 0xFu : h->recheck) & connectedMask;
                    if (h->mode == Mode::Mirror || (h->mode == Mode::Separate && !candidates))
                    {
                        h->recheck = 0;
                        continue;
                    }
                    if (down)
                        h->pending.push_back({ s.now, down });
                    for (auto ev = h->pending.begin(); ev != h->pending.end();)
                    {
                        int matched = -1;
                        for (int k = 0; k < USERS && matched < 0; k++)
                        {
                            if (!(candidates >> k & 1))
                                continue;
                            auto& presses = xPresses_[k];
                            auto xp = std::find_if(presses.begin(), presses.end(), [&](const Press& p) {
                                return p.time + MATCH_BEFORE_MS >= ev->time && p.time <= ev->time + MATCH_AFTER_MS &&
                                       std::popcount(p.buttons) == std::popcount(ev->buttons);
                            });
                            if (xp != presses.end())
                            {
                                presses.erase(xp);  // one XInput press answers one PlayStation press
                                matched = k;
                            }
                        }
                        if (matched >= 0)
                            h->matches[matched]++;
                        else if (s.now > ev->time + MATCH_AFTER_MS)
                            h->misses++;
                        else
                        {
                            ++ev;
                            continue;
                        }
                        ev = h->pending.erase(ev);
                    }
                    int best = int(std::max_element(h->matches, h->matches + USERS) - h->matches);
                    if (h->matches[best] >= 2)
                        Decide(*h, Mode::Mirror, best);
                    else if (h->misses >= 2 || (h->misses && !h->matches[best]))
                        Decide(*h, Mode::Separate, -1);
                }
                Assign();
            }

            // The game took `user` as player 1. With player1 = "auto" the device there becomes
            // player 1 (nothing moves, only the keyboard follows); with a chosen device, that device
            // moves onto `user`.
            void SetPlayerOne(uint32_t user)
            {
                if (user >= USERS || user == playerOne)
                    return;
                if (choices_[0] == "auto")
                {
                    int q = PlayerAt(user);
                    for (Dev& d : devs_)
                    {
                        if (d.player == q)
                            d.player = 0;
                        else if (d.player == 0)
                            d.player = q;
                    }
                }
                playerOne = user;
            }

            const Dev* DeviceOn(uint32_t user) const
            {
                int p = PlayerAt(user);
                auto it = std::find_if(devs_.begin(), devs_.end(), [&](const Dev& d) { return d.player == p; });
                return it == devs_.end() ? nullptr : &*it;
            }

            std::string NameOn(uint32_t user) const
            {
                const Dev* d = DeviceOn(user);
                return d ? NameOf(*d) : "?";
            }

            bool Read(uint32_t user, XINPUT_GAMEPAD& pad) const
            {
                const Dev* d = DeviceOn(user);
                if (!d)
                    return false;
                pad = d->slot >= 0 ? xState_[d->slot] : XINPUT_GAMEPAD{};
                if (d->hid)
                {
                    for (const Hid& h : hids_)
                    {
                        if (h.id == d->hid)
                            Merge(pad, h.state);
                    }
                }
                return true;
            }

            std::vector<Device> Devices() const
            {
                std::vector<Device> list;
                for (const Dev& d : devs_)
                    list.push_back({ KeyOf(d), NameOf(d), d.player });
                return list;
            }

            // "P1 = Controle 1 (jogo 0), P2 = DualSense (USB) (jogo 1)" for the log.
            std::string Describe() const
            {
                std::string text;
                for (int p = 0; p < USERS; p++)
                {
                    for (const Dev& d : devs_)
                    {
                        if (d.player != p)
                            continue;
                        char line[160];
                        snprintf(line, sizeof(line), "%sP%d = %s (controle %d do jogo)", text.empty() ? "" : ", ", p + 1, NameOf(d).c_str(), UserOf(p));
                        text += line;
                    }
                }
                if (KeyboardOnly() >= 0)
                    text += (text.empty() ? "" : ", ") + std::string("P") + std::to_string(KeyboardOnly() + 1) + " = teclado (controle " +
                            std::to_string(UserOf(KeyboardOnly())) + " do jogo)";
                return text.empty() ? "nenhum controle (só o teclado, no controle " + std::to_string(playerOne) + " do jogo)" : text;
            }

            // The player the keyboard drives: the one whose row is "keyboard", else player 1 (with its pad).
            int KeyboardPlayer() const { return choices_[1] == "keyboard" && choices_[0] != "keyboard" ? 1 : 0; }
            // The player kept for the keyboard alone (no pad there), -1 when it rides with player 1's pad.
            int KeyboardOnly() const { return choices_[0] == "keyboard" ? 0 : choices_[1] == "keyboard" ? 1 : -1; }

            int UserOf(int player) const
            {
                int users[USERS] = { int(playerOne) }, n = 1;
                for (int u = 0; u < USERS; u++)
                {
                    if (u != int(playerOne))
                        users[n++] = u;
                }
                return users[player];
            }

        private:
            Choices choices_{ "auto", "auto" };
            bool xConnected_[USERS] = {};
            uint16_t xLast_[USERS] = {};
            XINPUT_GAMEPAD xState_[USERS] = {};
            std::vector<Press> xPresses_[USERS];
            std::vector<Hid> hids_;
            std::vector<Dev> devs_;  // connection order

            int PlayerAt(uint32_t user) const
            {
                for (int p = 0; p < USERS; p++)
                {
                    if (UserOf(p) == int(user))
                        return p;
                }
                return -1;
            }

            Hid* HidOf(uint32_t id)
            {
                auto it = std::find_if(hids_.begin(), hids_.end(), [&](const Hid& h) { return h.id == id; });
                return it == hids_.end() ? nullptr : &*it;
            }

            Dev* BySlot(int slot)
            {
                auto it = std::find_if(devs_.begin(), devs_.end(), [&](const Dev& d) { return d.slot == slot; });
                return it == devs_.end() ? nullptr : &*it;
            }

            Dev* ByHid(uint32_t id)
            {
                auto it = std::find_if(devs_.begin(), devs_.end(), [&](const Dev& d) { return d.hid == id; });
                return it == devs_.end() ? nullptr : &*it;
            }

            std::string KeyOf(const Dev& d) const
            {
                if (d.slot >= 0)
                    return "xbox" + std::to_string(d.slot + 1);
                // The n-th PlayStation pad on its own, in connection order.
                int n = 1;
                for (const Hid& h : hids_)
                {
                    if (h.id < d.hid && h.mode == Mode::Separate)
                        n++;
                }
                return "playstation" + std::to_string(n);
            }

            // "Controle 1" (XInput), "DualSense (USB)", or "DualSense 2 (USB)" when two pads share a name.
            std::string NameOf(const Dev& d) const
            {
                const Hid* pad = nullptr;
                int same = 0, rank = 0;
                for (const Hid& h : hids_)
                {
                    if (h.id == d.hid)
                        pad = &h;
                }
                if (!pad)
                    return "Controle " + std::to_string(d.slot + 1);
                for (const Hid& h : hids_)
                {
                    if (h.name == pad->name && h.mode != Mode::Unknown)
                    {
                        same++;
                        rank += h.id <= pad->id;
                    }
                }
                if (same < 2)
                    return pad->name;
                size_t space = pad->name.find(" (");
                return pad->name.substr(0, space) + " " + std::to_string(rank) + (space == std::string::npos ? "" : pad->name.substr(space));
            }

            void Decide(Hid& h, Mode mode, int slot)
            {
                bool recheck = h.mode == Mode::Separate;
                h.pending.clear();
                std::fill(std::begin(h.matches), std::end(h.matches), 0);
                h.misses = 0;
                h.recheck = 0;
                if (mode == Mode::Separate)
                {
                    if (!recheck)
                    {
                        Log("[input] %s: controle próprio", h.name.c_str());
                        devs_.push_back({ -1, h.id });
                    }
                    h.mode = Mode::Separate;
                    return;
                }
                Log("[input] %s: é o mesmo controle que o XInput %d (DS4Windows/DSX); viram um só", h.name.c_str(), slot + 1);
                h.mode = Mode::Mirror;
                Dev* pad = recheck ? ByHid(h.id) : nullptr;
                if (pad)
                {
                    // It already had a player: it keeps it, and the virtual pad joins it.
                    std::erase_if(devs_, [&](const Dev& d) { return d.slot == slot && !d.hid; });
                    pad = ByHid(h.id);
                    pad->slot = slot;
                }
                else if (Dev* x = BySlot(slot))
                    x->hid = h.id;
            }

            Dev* ByKey(const std::string& key)
            {
                for (Dev& d : devs_)
                {
                    if (KeyOf(d) == key)
                        return &d;
                }
                return nullptr;
            }

            void Assign()
            {
                // A player kept for the keyboard has no pad.
                int keyboard = KeyboardOnly();
                for (Dev& d : devs_)
                {
                    if (d.player >= 0 && d.player == keyboard)
                        d.player = -1;
                }
                // The devices chosen in the settings, when there are two or more (a single pad is
                // always player 1, with the keyboard), or one with the keyboard on its own player.
                if (devs_.size() >= 2 || keyboard >= 0)
                {
                    for (int p = 0; p < 2; p++)
                    {
                        if (choices_[p] == "auto" || choices_[p] == "keyboard" || (p == 1 && choices_[1] == choices_[0]))
                            continue;
                        Dev* chosen = ByKey(choices_[p]);
                        if (!chosen || chosen->player == p)
                            continue;
                        for (Dev& d : devs_)
                        {
                            if (d.player == p)
                                d.player = -1;
                        }
                        chosen->player = p;
                    }
                }
                // Everyone else keeps their player; the new ones take the first free one.
                for (Dev& d : devs_)
                {
                    if (d.player >= 0)
                        continue;
                    for (int p = 0; p < USERS; p++)
                    {
                        if (p != keyboard && std::none_of(devs_.begin(), devs_.end(), [&](const Dev& o) { return o.player == p; }))
                        {
                            d.player = p;
                            break;
                        }
                    }
                }
            }
        };

        std::mutex g_mutex;
        Table g_table;
        uint64_t g_lastUpdate = 0;
        uint64_t g_nextProbe[USERS] = {};
        bool g_connected[USERS] = {};
        std::string g_described;
        std::atomic<uint32_t> g_playerOne{ 0 };

        // An XInput pad that moved past the dead zones makes "xbox" the last device used (pc_input.h).
        void NoteXInputActivity(int slot, const XINPUT_GAMEPAD& pad)
        {
            static XINPUT_GAMEPAD s_last[USERS];
            auto moved = [](int16_t a, int16_t b) { return std::abs(int(a) - int(b)) > 8000 && (std::abs(int(a)) > 12000 || std::abs(int(b)) > 12000); };
            XINPUT_GAMEPAD& last = s_last[slot];
            bool active = pad.wButtons != last.wButtons || std::abs(int(pad.bLeftTrigger) - int(last.bLeftTrigger)) > 60 ||
                          std::abs(int(pad.bRightTrigger) - int(last.bRightTrigger)) > 60 || moved(pad.sThumbLX, last.sThumbLX) ||
                          moved(pad.sThumbLY, last.sThumbLY) || moved(pad.sThumbRX, last.sThumbRX) || moved(pad.sThumbRY, last.sThumbRY);
            if (active)
            {
                last = pad;
                // A DualSense read directly and also seen through DS4Windows/DSX's virtual pad, or one
                // those programs hide (HidHide): still a DualSense.
                if (hid::HiddenPads())
                    pc::NoteDevice(pc::Device::PlayStation);
                else if (!hid::RecentlyActive())
                    pc::NoteDevice(pc::Device::Xbox);
            }
        }

        void LogChanges()
        {
            std::string described = g_table.Describe();
            if (described != g_described)
            {
                g_described = described;
                Log("[input] jogadores: %s", described.c_str());
            }
        }

        // Reads every controller and updates the table (at most once per UPDATE_MS).
        void Refresh()
        {
            std::lock_guard lock(g_mutex);
            uint64_t now = plat::TickCountMs();
            if (g_lastUpdate && now - g_lastUpdate < UPDATE_MS)
                return;
            g_lastUpdate = now;
            Snapshot s;
            s.now = now;
            for (int k = 0; k < USERS; k++)
            {
                if (!g_connected[k] && now < g_nextProbe[k])
                    continue;
                XINPUT_STATE state{};
                g_connected[k] = xinput::GetState(uint32_t(k), state);
                if (!g_connected[k])
                {
                    g_nextProbe[k] = now + PROBE_MS;
                    continue;
                }
                s.connected[k] = true;
                s.xinput[k] = state.Gamepad;
                NoteXInputActivity(k, state.Gamepad);
            }
            for (hid::PadInfo& info : hid::Pads())
            {
                XINPUT_GAMEPAD state{};
                if (hid::ReadPad(info.id, state))
                    s.pads.push_back({ std::move(info), state });
            }
            g_table.Update(s, settings::Get().controls.players);
            LogChanges();
        }
    }

    bool Read(uint32_t user, XINPUT_GAMEPAD& pad)
    {
        Refresh();
        std::lock_guard lock(g_mutex);
        bool connected = g_table.Read(user, pad);
        // The first presses on the other players' controllers go to the log (co-op reports).
        static uint16_t s_last[USERS];
        static int s_logged = 0;
        if (user < USERS && connected && user != g_table.playerOne && s_logged < 40)
        {
            if (uint16_t down = pad.wButtons & ~s_last[user])
            {
                s_logged++;
                Log("[input] controle %u do jogo (%s): botões 0x%04X", user, g_table.NameOn(user).c_str(), down);
            }
        }
        if (user < USERS)
            s_last[user] = connected ? pad.wButtons : 0;
        return connected;
    }

    void SetVibration(uint32_t user, uint16_t left, uint16_t right)
    {
        // [controls] vibration off: the motors stay still, for every kind of pad (XInput, the
        // PlayStation pads read directly, SDL's on Linux / Android).
        if (!settings::Get().controls.vibration)
            left = right = 0;
        int slot = -1;
        uint32_t id = 0;
        {
            std::lock_guard lock(g_mutex);
            if (const Dev* d = g_table.DeviceOn(user))
                slot = d->slot, id = d->hid;
        }
        // An XInput pad takes it (DS4Windows/DSX pass a virtual pad's rumble on); otherwise the PlayStation pad.
        if (slot >= 0)
            xinput::SetState(uint32_t(slot), left, right);
        else if (id)
            hid::SetPadVibration(id, left, right);
    }

    void SetLight(uint32_t user, uint32_t rgb, uint8_t player)
    {
        uint32_t id = 0;
        {
            std::lock_guard lock(g_mutex);
            // A PlayStation pad on its own only: one that is also an XInput pad is DS4Windows/DSX's.
            if (const Dev* d = g_table.DeviceOn(user); d && d->slot < 0)
                id = d->hid;
        }
        if (id)
            hid::SetPadLight(id, rgb, player);
    }

    uint32_t UserOfPlayer(int player)
    {
        std::lock_guard lock(g_mutex);
        return uint32_t(g_table.UserOf(std::clamp(player, 0, USERS - 1)));
    }

    uint32_t PlayerOneUser()
    {
        return g_playerOne.load();
    }

    uint32_t KeyboardUser()
    {
        std::lock_guard lock(g_mutex);
        return uint32_t(g_table.UserOf(g_table.KeyboardPlayer()));
    }

    std::vector<Device> Devices()
    {
        Refresh();
        std::lock_guard lock(g_mutex);
        return g_table.Devices();
    }

    int SelfTest()
    {
        int failures = 0;
        auto check = [&](const char* what, bool ok) {
            printf("%-64s %s\n", what, ok ? "ok" : "FALHOU");
            failures += !ok;
        };
        auto pad = [](uint16_t buttons) {
            XINPUT_GAMEPAD p{};
            p.wButtons = buttons;
            return p;
        };
        auto player = [](const Table& t, const char* key) {
            for (const Device& d : t.Devices())
            {
                if (d.key == key)
                    return d.player;
            }
            return -9;
        };
        const Choices autoChoices{ "auto", "auto" };
        const hid::PadInfo ds{ 7, "DualSense (USB)" };
        constexpr uint16_t A = XINPUT_GAMEPAD_A, B = XINPUT_GAMEPAD_B;
        // One update every 16 ms; `x` = the XInput slot 0 pad (nullptr: none), `ps` = the DualSense (nullptr: none).
        auto step = [&](Table& t, uint64_t& now, const XINPUT_GAMEPAD* x, const XINPUT_GAMEPAD* ps, const Choices& c, const XINPUT_GAMEPAD* x1 = nullptr) {
            Snapshot s;
            s.now = now += 16;
            if (x)
                s.connected[0] = true, s.xinput[0] = *x;
            if (x1)
                s.connected[1] = true, s.xinput[1] = *x1;
            if (ps)
                s.pads.push_back({ ds, *ps });
            t.Update(s, c);
        };
        const XINPUT_GAMEPAD idle{}, pressA = pad(A), pressB = pad(B);

        {
            // An Xbox pad and a DualSense: two players.
            Table t;
            uint64_t now = 0;
            step(t, now, &idle, &idle, autoChoices);
            check("Xbox + DualSense: DualSense parado ainda nao joga", player(t, "playstation1") == -9);
            step(t, now, &idle, &pressA, autoChoices);
            for (int i = 0; i < 12; i++)
                step(t, now, &idle, &pressA, autoChoices);
            XINPUT_GAMEPAD read{};
            check("Xbox + DualSense: Xbox = P1, DualSense = P2", player(t, "xbox1") == 0 && player(t, "playstation1") == 1);
            check("Xbox + DualSense: o A do DualSense chega no controle 1 do jogo", t.Read(1, read) && (read.wButtons & A));
            check("Xbox + DualSense: o controle 0 do jogo nao ve o A", t.Read(0, read) && !(read.wButtons & A));
        }
        {
            // DS4Windows without HidHide: the same DualSense twice.
            Table t;
            uint64_t now = 0;
            step(t, now, &idle, &idle, autoChoices);
            for (uint16_t button : { A, B })
            {
                XINPUT_GAMEPAD down = pad(button);
                step(t, now, &idle, &down, autoChoices);
                step(t, now, &down, &down, autoChoices);
                step(t, now, &down, &down, autoChoices);
                step(t, now, &idle, &idle, autoChoices);
                step(t, now, &idle, &idle, autoChoices);
            }
            XINPUT_GAMEPAD read{};
            check("DS4Windows: DualSense e o pad virtual viram um controle so", t.Devices().size() == 1 && player(t, "xbox1") == 0);
            check("DS4Windows: nenhum controle no P2", !t.Read(1, read));
            step(t, now, &idle, &pressA, autoChoices);
            check("DS4Windows: o A do DualSense chega no P1 mesmo antes do virtual", t.Read(0, read) && (read.wButtons & A));
        }
        {
            // A DualSense alone, then DS4Windows starts: still one player 1.
            Table t;
            uint64_t now = 0;
            step(t, now, nullptr, &idle, autoChoices);
            check("DualSense sozinho: P1 na hora", player(t, "playstation1") == 0);
            step(t, now, &idle, &idle, autoChoices);
            check("DS4Windows abre depois: o virtual fica com o P2 enquanto confere", player(t, "xbox1") == 1);
            for (uint16_t button : { A, B })
            {
                XINPUT_GAMEPAD down = pad(button);
                step(t, now, &down, &down, autoChoices);
                step(t, now, &idle, &idle, autoChoices);
            }
            XINPUT_GAMEPAD read{};
            check("DS4Windows abre depois: viram um so, ainda P1", t.Devices().size() == 1 && t.Devices()[0].player == 0 && !t.Read(1, read));
        }
        {
            // A DualSense alone, then an Xbox pad: the DualSense stays player 1.
            Table t;
            uint64_t now = 0;
            step(t, now, nullptr, &idle, autoChoices);
            step(t, now, &idle, &idle, autoChoices);
            step(t, now, &idle, &pressB, autoChoices);
            for (int i = 0; i < 12; i++)
                step(t, now, &idle, &pressB, autoChoices);
            check("DualSense e depois Xbox: DualSense P1, Xbox P2", player(t, "playstation1") == 0 && player(t, "xbox1") == 1 && t.Devices().size() == 2);
            // Player 1's pad leaves: player 2 keeps its controller; a new pad takes player 1.
            step(t, now, &idle, nullptr, autoChoices);
            check("P1 desconectado: o P2 continua P2", player(t, "xbox1") == 1);
            step(t, now, &idle, nullptr, autoChoices, &idle);
            check("um controle novo assume o P1 vago", player(t, "xbox2") == 0);
        }
        {
            // Settings: player 1 = the second Xbox pad; a single pad is always player 1.
            Table t;
            uint64_t now = 0;
            const Choices second{ "xbox2", "auto" }, p2first{ "auto", "xbox1" };
            step(t, now, &idle, nullptr, p2first);
            check("so um controle, mesmo escolhido para P2: ele e o P1", player(t, "xbox1") == 0);
            step(t, now, &idle, nullptr, second, &idle);
            check("player1 = xbox2: Controle 2 = P1, Controle 1 = P2", player(t, "xbox2") == 0 && player(t, "xbox1") == 1);
            check("player1 = xbox2: o P1 fica no controle 0 do jogo", t.UserOf(0) == 0 && t.DeviceOn(0)->slot == 1);
            // The game takes controller 1 as player 1 (START pressed there on the title screen).
            t.SetPlayerOne(1);
            check("START no titulo pelo controle 1 do jogo, P1 escolhido: o Controle 2 vai para la", t.DeviceOn(1)->slot == 1 && t.DeviceOn(0)->slot == 0);
            Table a;
            now = 0;
            step(a, now, &idle, nullptr, autoChoices, &idle);
            a.SetPlayerOne(1);
            check("START no titulo pelo controle 1 do jogo, auto: ele vira o P1 sem trocar", player(a, "xbox2") == 0 && a.DeviceOn(1)->slot == 1 && a.DeviceOn(0)->slot == 0);
        }
        {
            // The keyboard on a player of its own: the pads go to the others.
            Table t;
            uint64_t now = 0;
            const Choices keyboardP1{ "keyboard", "auto" }, keyboardP2{ "auto", "keyboard" };
            step(t, now, &idle, nullptr, autoChoices);
            check("auto: Controle 1 = P1 e o teclado junto", player(t, "xbox1") == 0 && t.KeyboardPlayer() == 0 && t.KeyboardOnly() < 0);
            step(t, now, &idle, nullptr, keyboardP1);
            check("player1 = keyboard: o Controle 1 sozinho vira o P2", player(t, "xbox1") == 1 && t.KeyboardPlayer() == 0 && !t.DeviceOn(t.UserOf(0)));
            step(t, now, &idle, nullptr, keyboardP1, &idle);
            check("player1 = keyboard: o Controle 2 fica com o P3", player(t, "xbox1") == 1 && player(t, "xbox2") == 2);
            Table b;
            now = 0;
            step(b, now, &idle, nullptr, keyboardP2, &idle);
            check("player2 = keyboard: Controle 1 = P1, Controle 2 = P3, teclado = P2",
                  player(b, "xbox1") == 0 && player(b, "xbox2") == 2 && b.KeyboardPlayer() == 1 && b.UserOf(1) == 1);
            step(b, now, &idle, nullptr, autoChoices, &idle);
            check("de volta a auto: o teclado volta para o P1", b.KeyboardPlayer() == 0 && b.KeyboardOnly() < 0);
        }
        printf("%s\n", failures ? "FALHOU" : "tudo ok");
        return failures != 0;
    }
}

extern "C" PPC_FUNC(__imp__sub_821FC498);

// sub_821FC498(managers, index): the game takes controller `index` as player 1 (title screen START,
// sub_821DB680). The keyboard follows it (pad_slots.h).
PPC_FUNC(sub_821FC498)
{
    uint32_t user = ctx.r4.u32;
    __imp__sub_821FC498(ctx, base);
    if (user >= 4)
        return;
    std::lock_guard lock(input::slots::g_mutex);
    input::slots::g_table.SetPlayerOne(user);
    input::slots::g_playerOne = input::slots::g_table.playerOne;
    Log("[input] o jogo pegou o controle %u como jogador 1", user);
    input::slots::LogChanges();
}
