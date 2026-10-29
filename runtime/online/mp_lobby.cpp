// MULTIPLAYER sessions: the lobby protocol (mp_lobby.h).

#include "mp_lobby.h"
#include "mp.h"
#include "mp_colors.h"
#include "mp_nat.h"
#include "../diagnostics.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mp::lobby
{
    namespace
    {
        constexpr uint32_t VERSION = 5;  // 2: the password goes hashed (PasswordHash); 3: cutscene skips in the roster; 4: positions in the pads;
                                         // 5: the players' colours (HELLO / PING: asked for, ROSTER: given)
        constexpr uint64_t RESEND_MS = 500, TIMEOUT_MS = 6000, DISCOVER_MS = 1000, SERVER_TTL_MS = 3500, JOIN_TIMEOUT_MS = 6000;
        constexpr uint64_t SKIP_KEEP_MS = 20000;  // a client still not at the cutscene the host skipped: forgotten after this

        enum Type : uint8_t { DISCOVER = 1, ANNOUNCE, HELLO, WELCOME, DENY, ROSTER, PING, PONG, LEAVE };

        struct Member
        {
            bool used = false;
            uint32_t token = 0;
            std::string name;
            uint32_t ip = 0;
            int port = 0;
            uint64_t seenAt = 0;
            uint32_t pingMs = 0;
            uint8_t wanted = 0, colour = colors::NONE;  // the colour the player asked for, and the one it has
        };

        struct Session
        {
            State state = State::Off;
            Reason reason = Reason::None;
            // host
            uint32_t sessionId = 0;
            Member members[MAX_PLAYERS];  // by slot; slot 0 = the host
            std::string password;
            Options options;
            bool inGame = false;
            int32_t startLevel = -1;
            uint32_t startId = 0;
            uint32_t skipId = 0, skipNode = 0;  // the cutscenes the host skipped (a counter) and the last one's node
            uint64_t lastRoster = 0;
            // client
            uint32_t token = 0, hostIp = 0;
            int hostPort = 0;
            std::string joinPassword, hostName;
            uint32_t slot = 0, rosterPlayers = 0;
            std::vector<Player> roster;
            uint64_t joinedAt = 0, lastHello = 0, lastPing = 0, heardAt = 0;
            uint32_t pingMs = 0, seenStartId = 0;
            bool firstRoster = true, inGameHost = false, hostLeftLevel = false;
            int32_t pendingStart = -1;
            uint32_t seenSkipId = 0, pendingSkipNode = 0;  // a skip of the host's not done here yet
            uint64_t pendingSkipAt = 0;
            // searching
            struct Found
            {
                Server server;
                uint64_t seenAt;
                uint32_t sessionId;  // a host answers on the LAN and on 127.0.0.1: once in the list
            };
            std::vector<Found> servers;
            uint64_t lastDiscover = 0;
            uint32_t localIp = 0;  // host: its LAN address (the code)
        };

        std::mutex g_mutex;
        Session g_s;

        // ---- wire --------------------------------------------------------------------------------

        struct Writer
        {
            std::vector<uint8_t> b;
            explicit Writer(Type type, uint32_t session)
            {
                Put32(MAGIC);
                Put8(type);
                Put32(session);
            }
            void Put8(uint32_t v) { b.push_back(uint8_t(v)); }
            void Put16(uint32_t v) { Put8(v >> 8), Put8(v); }
            void Put32(uint32_t v) { Put16(v >> 16), Put16(v); }
            void PutStr(const std::string& s)
            {
                size_t n = std::min<size_t>(s.size(), 64);
                Put8(uint32_t(n));
                b.insert(b.end(), s.begin(), s.begin() + n);
            }
        };

        struct Reader
        {
            const uint8_t* p;
            const uint8_t* end;
            bool ok = true;
            uint32_t Get8()
            {
                if (p + 1 > end)
                    return ok = false, 0;
                return *p++;
            }
            uint32_t Get16() { uint32_t h = Get8(); return (h << 8) | Get8(); }
            uint32_t Get32() { uint32_t h = Get16(); return (h << 16) | Get16(); }
            std::string GetStr()
            {
                uint32_t n = Get8();
                if (p + n > end)
                    return ok = false, std::string();
                std::string s(reinterpret_cast<const char*>(p), n);
                p += n;
                return s;
            }
        };

        void Send(uint32_t ip, int port, const Writer& w) { net::SendTo(ip, port, w.b.data(), w.b.size()); }

        uint32_t Random32()
        {
            static std::mt19937 rng{ std::random_device{}() ^ uint32_t(NowMs()) };
            uint32_t v;
            do
                v = rng();
            while (!v);
            return v;
        }

        uint32_t Count(const Session& s)
        {
            uint32_t n = 0;
            for (const Member& m : s.members)
                n += m.used;
            return n;
        }

        // This machine's LAN address: the source address a datagram to a private network would get
        // (a UDP connect sends nothing).
        uint32_t LocalIp()
        {
            uint32_t found = 0x7F000001;
            for (uint32_t probe : { 0x0A0A0A0Au, 0xC0A80001u, 0x08080808u })
            {
                int sock = int(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
                if (sock < 0)
                    continue;
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_addr.s_addr = htonl(probe);
                to.sin_port = htons(9);
                sockaddr_in local{};
                socklen_t size = sizeof(local);
                bool ok = connect(sock, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0
                          && getsockname(sock, reinterpret_cast<sockaddr*>(&local), &size) == 0;
#ifdef _WIN32
                closesocket(sock);
#else
                close(sock);
#endif
                uint32_t ip = ntohl(local.sin_addr.s_addr);
                if (ok && ip && (ip >> 24) != 127)
                    return ip;
            }
            return found;
        }

        // The password as HELLO carries it: FNV-1a 64 in hex, not the text (UDP is readable on the way).
        std::string PasswordHash(const std::string& password)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (char c : "crash-mp:" + password)
                h = (h ^ uint8_t(c)) * 0x100000001B3ull;
            char text[17];
            snprintf(text, sizeof(text), "%016llx", (unsigned long long)h);
            return text;
        }

        // The roster as the menu shows it (host side).
        std::vector<Player> HostRoster(const Session& s)
        {
            std::vector<Player> out;
            for (uint32_t slot = 0; slot < MAX_PLAYERS; ++slot)
                if (s.members[slot].used)
                    out.push_back({ slot, s.members[slot].name, s.members[slot].pingMs, slot == 0, s.members[slot].colour });
            return out;
        }

        // Slots stay 0..n-1 in join order while no level runs (the players' order in the game).
        void Compact(Session& s)
        {
            if (s.inGame)
                return;
            Member packed[MAX_PLAYERS];
            uint32_t next = 0;
            for (const Member& m : s.members)
                if (m.used)
                    packed[next++] = m;
            for (uint32_t i = 0; i < MAX_PLAYERS; ++i)
                s.members[i] = packed[i];
        }

        // The game's view of the session: player count and, on the host, the clients' addresses.
        void ApplyHost(Session& s)
        {
            uint32_t count = Count(s);
            if (!s.inGame)
            {
                mp::SetSession(true, count);
                net::SetRole(net::Role::Host, 0, count, 0, 0);
            }
            for (uint32_t slot = 1; slot < MAX_PLAYERS; ++slot)
                if (s.members[slot].used)
                    net::SetClientAddress(slot, s.members[slot].ip, s.members[slot].port);
        }

        // Host: every player's colour (mp_colors.h). A player who has the colour it asked for keeps it;
        // the others, in slot order, get theirs if nobody has it, else the next free one after it. The
        // host's own is its setting. True when one changed.
        bool ResolveColours(Session& s)
        {
            s.members[0].wanted = uint8_t(colors::Mine());
            uint32_t taken = 0;
            bool keep[MAX_PLAYERS] = {}, changed = false;
            for (uint32_t slot = 0; slot < MAX_PLAYERS; ++slot)
            {
                const Member& m = s.members[slot];
                if (m.used && m.colour == m.wanted && m.colour < colors::Count() && !(taken & (1u << m.colour)))
                    keep[slot] = true, taken |= 1u << m.colour;
            }
            for (uint32_t slot = 0; slot < MAX_PLAYERS; ++slot)
            {
                Member& m = s.members[slot];
                if (!m.used || keep[slot])
                    continue;
                uint8_t colour = uint8_t(colors::NextFree(m.wanted < colors::Count() ? m.wanted : 0, 1, taken));
                if (colour != m.colour)
                {
                    changed = true;
                    Log("[lobby] %s: colour %s%s", m.name.c_str(), colors::Get(colour).id, colour != m.wanted ? " (the one asked for is taken)" : "");
                }
                m.colour = colour;
                taken |= 1u << colour;
            }
            uint8_t colours[MAX_PLAYERS];
            for (uint32_t slot = 0; slot < MAX_PLAYERS; ++slot)
                colours[slot] = s.members[slot].used ? s.members[slot].colour : colors::NONE;
            colors::SetSlotColours(colours, MAX_PLAYERS);
            return changed;
        }

        void SendRoster(Session& s)
        {
            ResolveColours(s);
            Writer w(ROSTER, s.sessionId);
            w.Put32(s.startId);
            w.Put32(uint32_t(s.startLevel));
            w.Put8(s.inGame);
            w.Put8(s.options.maxPlayers);
            w.Put8(s.options.knockOutSeconds);
            w.Put8(!s.password.empty());
            w.Put8(Count(s));
            for (uint32_t slot = 0; slot < MAX_PLAYERS; ++slot)
            {
                const Member& m = s.members[slot];
                if (!m.used)
                    continue;
                w.Put8(slot);
                w.Put32(m.token);
                w.PutStr(m.name);
                w.Put16(std::min<uint32_t>(m.pingMs, 65535));
                w.Put8(m.colour);
            }
            w.Put32(s.skipId);
            w.Put32(s.skipNode);
            for (uint32_t slot = 1; slot < MAX_PLAYERS; ++slot)
                if (s.members[slot].used)
                    Send(s.members[slot].ip, s.members[slot].port, w);
            s.lastRoster = NowMs();
        }

        void Deny(const Session& s, uint32_t ip, int port, uint32_t token, Reason reason)
        {
            Writer w(DENY, s.sessionId);
            w.Put32(token);
            w.Put8(uint8_t(reason));
            Send(ip, port, w);
        }

        void Fail(Session& s, Reason reason)
        {
            if (s.state == State::Joined || s.state == State::Joining)
            {
                // A client alone again: the game as it was (the level goes on without the mirror).
                mp::SetSession(false, 1);
                net::SetRole(net::Role::None, 0, 1, 0, 0);
            }
            s.state = State::Failed;
            s.reason = reason;
            colors::ClearSlotColours();
            Log("[lobby] %s", ReasonText(reason, false));
        }

        // ---- host --------------------------------------------------------------------------------

        void HostPacket(Session& s, Type type, Reader& r, uint32_t ip, int port)
        {
            uint64_t now = NowMs();
            if (type == DISCOVER)
            {
                Writer w(ANNOUNCE, s.sessionId);
                w.Put32(VERSION);
                w.PutStr(s.members[0].name);
                w.Put8(Count(s));
                w.Put8(s.options.maxPlayers);
                w.Put8(!s.password.empty());
                w.Put8(s.inGame);
                Send(ip, port, w);
                return;
            }
            if (type == HELLO)
            {
                uint32_t version = r.Get32(), token = r.Get32();
                std::string name = r.GetStr(), password = r.GetStr();
                if (!r.ok || !token)
                    return;
                uint8_t wanted = uint8_t(r.Get8());  // (an older version has none: refused below)
                for (uint32_t slot = 1; slot < MAX_PLAYERS; ++slot)
                    if (s.members[slot].used && s.members[slot].token == token)
                    {
                        s.members[slot].ip = ip, s.members[slot].port = port, s.members[slot].seenAt = now;
                        Writer w(WELCOME, s.sessionId);
                        w.Put32(token);
                        w.Put8(slot);
                        Send(ip, port, w);
                        return;  // a repeated HELLO
                    }
                Reason refuse = version != VERSION ? Reason::Version
                                : s.inGame ? Reason::Started
                                : !s.password.empty() && password != PasswordHash(s.password) ? Reason::Password
                                : Count(s) >= s.options.maxPlayers ? Reason::Full
                                : Reason::None;
                if (refuse != Reason::None)
                {
                    Log("[lobby] %s (%s) refused: %s", name.c_str(), FormatAddress(ip, port).c_str(), ReasonText(refuse, false));
                    Deny(s, ip, port, token, refuse);
                    return;
                }
                uint32_t slot = 1;
                while (slot < MAX_PLAYERS && s.members[slot].used)
                    ++slot;
                if (slot >= MAX_PLAYERS)
                    return;
                s.members[slot] = { true, token, name.empty() ? "CRASH" : name, ip, port, now, 0, wanted };
                Log("[lobby] %s joined from %s as player %u", s.members[slot].name.c_str(), FormatAddress(ip, port).c_str(), slot + 1);
                Writer w(WELCOME, s.sessionId);
                w.Put32(token);
                w.Put8(slot);
                Send(ip, port, w);
                ApplyHost(s);
                SendRoster(s);
                return;
            }
            uint32_t token = r.Get32();
            if (!r.ok)
                return;
            uint32_t slot = 1;
            while (slot < MAX_PLAYERS && !(s.members[slot].used && s.members[slot].token == token))
                ++slot;
            if (slot >= MAX_PLAYERS)
                return;
            Member& m = s.members[slot];
            m.seenAt = now;
            m.ip = ip, m.port = port;
            if (type == PING)
            {
                uint32_t sent = r.Get32(), rtt = r.Get32();
                uint8_t wanted = uint8_t(r.Get8());
                if (r.ok)
                    m.pingMs = rtt;
                Writer w(PONG, s.sessionId);
                w.Put32(token);
                w.Put32(sent);
                Send(ip, port, w);
                if (r.ok && wanted != m.wanted)
                {
                    m.wanted = wanted;  // the player picked another colour in its lobby
                    if (ResolveColours(s))
                        SendRoster(s);
                }
            }
            else if (type == LEAVE)
            {
                Log("[lobby] %s left", m.name.c_str());
                m = {};
                Compact(s);
                ApplyHost(s);
                SendRoster(s);
            }
        }

        void HostTick(Session& s)
        {
            uint64_t now = NowMs();
            bool changed = false;
            for (uint32_t slot = 1; slot < MAX_PLAYERS; ++slot)
            {
                Member& m = s.members[slot];
                if (m.used && now - m.seenAt > TIMEOUT_MS)
                {
                    Log("[lobby] %s timed out", m.name.c_str());
                    m = {};
                    changed = true;
                }
            }
            if (changed)
            {
                Compact(s);
                ApplyHost(s);
            }
            if (ResolveColours(s))
                changed = true;  // the host picked another colour
            if (changed || now - s.lastRoster >= RESEND_MS)
                SendRoster(s);
        }

        // ---- client ------------------------------------------------------------------------------

        void ClientPacket(Session& s, Type type, uint32_t session, Reader& r, uint32_t ip, int port)
        {
            uint64_t now = NowMs();
            if (type == ANNOUNCE && s.state == State::Searching)
            {
                Server server;
                uint32_t version = r.Get32();
                server.ip = ip, server.port = port;
                server.name = r.GetStr();
                server.players = r.Get8(), server.maxPlayers = r.Get8();
                server.locked = r.Get8(), server.inGame = r.Get8();
                if (!r.ok || version != VERSION)
                    return;
                for (auto& found : s.servers)
                    if (found.sessionId == session)
                    {
                        // The LAN address is the one other machines can use too.
                        if ((ip >> 24) == 127 && (found.server.ip >> 24) != 127)
                            server.ip = found.server.ip, server.port = found.server.port;
                        found.server = server, found.seenAt = now;
                        return;
                    }
                s.servers.push_back({ server, now, session });
                Log("[lobby] found %s at %s (%u/%u%s)", server.name.c_str(), FormatAddress(ip, port).c_str(), server.players,
                    server.maxPlayers, server.locked ? ", password" : "");
                return;
            }
            if ((s.state != State::Joining && s.state != State::Joined) || ip != s.hostIp || port != s.hostPort)
                return;
            s.heardAt = now;
            if (type == WELCOME)
            {
                uint32_t token = r.Get32(), slot = r.Get8();
                if (!r.ok || token != s.token || s.state == State::Joined)
                    return;
                s.state = State::Joined;
                s.slot = slot;
                s.sessionId = session;
                Log("[lobby] joined %s as player %u", FormatAddress(ip, port).c_str(), slot + 1);
            }
            else if (type == DENY)
            {
                uint32_t token = r.Get32();
                Reason reason = Reason(r.Get8());
                if (r.ok && token == s.token)
                    Fail(s, reason);
            }
            else if (type == PONG)
            {
                uint32_t token = r.Get32(), sent = r.Get32();
                if (r.ok && token == s.token)
                    s.pingMs = uint32_t(now) - sent;
            }
            else if (type == ROSTER && s.state == State::Joined)
            {
                uint32_t startId = r.Get32();
                int32_t level = int32_t(r.Get32());
                bool inGame = r.Get8();
                s.options.maxPlayers = r.Get8();
                s.options.knockOutSeconds = r.Get8();
                bool locked = r.Get8();
                (void)locked;
                uint32_t count = r.Get8();
                std::vector<Player> roster;
                uint32_t mySlot = s.slot;
                for (uint32_t i = 0; i < count && r.ok; ++i)
                {
                    Player p;
                    p.slot = r.Get8();
                    uint32_t token = r.Get32();
                    p.name = r.GetStr();
                    p.pingMs = r.Get16();
                    p.colour = uint8_t(r.Get8());
                    p.me = token == s.token;
                    if (p.me)
                        mySlot = p.slot;
                    if (p.slot == 0)
                        s.hostName = p.name;
                    roster.push_back(p);
                }
                uint32_t skipId = r.Get32(), skipNode = r.Get32();
                if (!r.ok)
                    return;
                s.roster = roster;
                {
                    uint8_t colours[MAX_PLAYERS];
                    std::fill(std::begin(colours), std::end(colours), colors::NONE);
                    for (const Player& p : roster)
                        if (p.slot < MAX_PLAYERS)
                            colours[p.slot] = p.colour;
                    colors::SetSlotColours(colours, MAX_PLAYERS);
                }
                // A cutscene the host skipped: this machine skips it when it plays it (or already does).
                if (skipId != s.seenSkipId)
                {
                    if (s.seenSkipId || !s.firstRoster)
                    {
                        s.pendingSkipNode = skipNode, s.pendingSkipAt = now;
                        Log("[lobby] the host skipped the cutscene of node %u", skipNode);
                    }
                    s.seenSkipId = skipId;
                }
                if (s.inGameHost && !inGame)
                {
                    s.hostLeftLevel = true;
                    Log("[lobby] the host left the level");
                }
                s.inGameHost = inGame;
                // Slot and player count follow the host's while it is in the menus (fixed in a level).
                if (!inGame && (mySlot != s.slot || count != s.rosterPlayers || s.firstRoster))
                {
                    s.slot = mySlot, s.rosterPlayers = count, s.firstRoster = false;
                    mp::SetSession(true, count);
                    net::SetRole(net::Role::Client, s.slot, count, s.hostIp, s.hostPort);
                }
                if (startId != s.seenStartId)
                {
                    bool first = s.seenStartId == 0 && s.joinedAt && now - s.joinedAt < 1500 && inGame;
                    s.seenStartId = startId;
                    if (inGame && level >= 0 && !first)
                    {
                        s.slot = mySlot, s.rosterPlayers = count;
                        mp::SetSession(true, count);
                        net::SetRole(net::Role::Client, s.slot, count, s.hostIp, s.hostPort);
                        s.pendingStart = level;
                        Log("[lobby] the host started level %d: following (player %u of %u)", level, s.slot + 1, count);
                    }
                }
            }
        }

        void ClientTick(Session& s)
        {
            uint64_t now = NowMs();
            if (s.state == State::Searching)
            {
                if (now - s.lastDiscover >= DISCOVER_MS)
                {
                    s.lastDiscover = now;
                    Writer w(DISCOVER, 0);
                    w.Put32(VERSION);
                    int port = DEFAULT_PORT;
                    if (const char* p = getenv("CRASH_MP_PORT"))
                        port = atoi(p);
                    net::Broadcast(port, w.b.data(), w.b.size());
                    net::SendTo(0x7F000001, port, w.b.data(), w.b.size());
                }
                s.servers.erase(std::remove_if(s.servers.begin(), s.servers.end(),
                                               [&](const auto& e) { return now - e.seenAt > SERVER_TTL_MS; }),
                                s.servers.end());
                return;
            }
            if (s.state == State::Joining)
            {
                if (now - s.joinedAt > JOIN_TIMEOUT_MS)
                {
                    Fail(s, Reason::NoAnswer);
                    return;
                }
                if (now - s.lastHello >= RESEND_MS)
                {
                    s.lastHello = now;
                    Writer w(HELLO, 0);
                    w.Put32(VERSION);
                    w.Put32(s.token);
                    w.PutStr(LocalName());
                    w.PutStr(s.joinPassword.empty() ? std::string() : PasswordHash(s.joinPassword));
                    w.Put8(colors::Mine());
                    Send(s.hostIp, s.hostPort, w);
                }
                return;
            }
            if (s.state == State::Joined)
            {
                if (now - s.heardAt > TIMEOUT_MS)
                {
                    Fail(s, Reason::HostLeft);
                    return;
                }
                if (now - s.lastPing >= RESEND_MS)
                {
                    s.lastPing = now;
                    Writer w(PING, s.sessionId);
                    w.Put32(s.token);
                    w.Put32(uint32_t(now));
                    w.Put32(s.pingMs);
                    w.Put8(colors::Mine());
                    Send(s.hostIp, s.hostPort, w);
                }
            }
        }
    }

    // ---- menu side -------------------------------------------------------------------------------

    std::string LocalName()
    {
        static std::string name = [] {
            std::string raw;
            if (const char* env = getenv("CRASH_MP_NAME"))
                raw = env;
#ifdef _WIN32
            if (raw.empty())
            {
                char user[64] = {};
                DWORD size = sizeof(user);
                if (GetUserNameA(user, &size))
                    raw = user;
            }
#elif defined(__ANDROID__)
            if (raw.empty())
            {
                char model[PROP_VALUE_MAX] = {};  // no user name on Android: the device's model (SM-X920)
                if (__system_property_get("ro.product.model", model) > 0)
                    raw = model;
            }
#else
            if (raw.empty() && getenv("USER"))
                raw = getenv("USER");
#endif
            std::string out;
            for (char c : raw)
                if (std::isalnum(uint8_t(c)) || c == ' ')
                    out += char(std::toupper(uint8_t(c)));
            if (out.size() > 12)
                out.resize(12);
            return out.empty() ? std::string("CRASH") : out;
        }();
        return name;
    }

    bool Host()
    {
        Leave();
        int port = DEFAULT_PORT;
        if (const char* p = getenv("CRASH_MP_PORT"))
            port = atoi(p);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_s = {};
        if (!net::Open(port))
        {
            Fail(g_s, Reason::PortTaken);
            return false;
        }
        g_s.state = State::Hosting;
        g_s.sessionId = Random32();
        g_s.members[0] = { true, 0, LocalName(), 0, 0, NowMs(), 0, uint8_t(colors::Mine()) };
        g_s.localIp = LocalIp();
        if (const char* password = getenv("CRASH_MP_PASSWORD"))  // tests
            for (const char* c = password; *c; c++)
                g_s.password += char(std::toupper(uint8_t(*c)));
        ApplyHost(g_s);
        nat::Open(net::LocalPort(), g_s.localIp);  // the internet code, once the router and STUN answer
        Log("[lobby] hosting on %s, code %s", FormatAddress(g_s.localIp, net::LocalPort()).c_str(),
            EncodeCode(g_s.localIp, net::LocalPort()).c_str());
        return true;
    }

    void Search()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.state == State::Searching)
            return;
        if (g_s.state != State::Off && g_s.state != State::Failed)
            return;
        g_s = {};
        if (!net::IsOpen() && !net::Open(0))
        {
            Fail(g_s, Reason::PortTaken);
            return;
        }
        g_s.state = State::Searching;
        Log("[lobby] searching the LAN");
    }

    std::vector<Server> Servers()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<Server> out;
        for (const auto& e : g_s.servers)
            out.push_back(e.server);
        return out;
    }

    bool Join(const std::string& codeOrAddress, const std::string& password)
    {
        uint32_t ip;
        int port;
        if (!DecodeAddress(codeOrAddress, ip, port))
        {
            Log("[lobby] '%s' is not a code nor an address", codeOrAddress.c_str());
            std::lock_guard<std::mutex> lock(g_mutex);
            Fail(g_s, Reason::NoAnswer);
            return false;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.state == State::Hosting)
            return false;
        auto servers = g_s.servers;
        g_s = {};
        g_s.servers = servers;
        if (!net::IsOpen() && !net::Open(0))
        {
            Fail(g_s, Reason::PortTaken);
            return false;
        }
        g_s.state = State::Joining;
        g_s.token = Random32();
        g_s.hostIp = ip, g_s.hostPort = port;
        g_s.joinPassword = password;
        g_s.joinedAt = NowMs();
        Log("[lobby] joining %s", FormatAddress(ip, port).c_str());
        return true;
    }

    void Leave()
    {
        bool close = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_s.state == State::Joined || g_s.state == State::Joining)
            {
                Writer w(LEAVE, g_s.sessionId);
                w.Put32(g_s.token);
                Send(g_s.hostIp, g_s.hostPort, w);
            }
            if (g_s.state == State::Hosting)
                nat::Close();
            if (g_s.state == State::Hosting)
                for (uint32_t slot = 1; slot < MAX_PLAYERS; ++slot)
                    if (g_s.members[slot].used)
                        Deny(g_s, g_s.members[slot].ip, g_s.members[slot].port, g_s.members[slot].token, Reason::HostLeft);
            close = g_s.state != State::Off;
            g_s = {};
            colors::ClearSlotColours();
        }
        if (close)
        {
            mp::SetSession(false, 1);
            net::SetRole(net::Role::None, 0, 1, 0, 0);
            net::Close();
            Log("[lobby] session closed");
        }
    }

    State GetState()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_s.state;
    }

    Reason LastReason()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_s.reason;
    }

    const char* ReasonText(Reason reason, bool pt)
    {
        switch (reason)
        {
        case Reason::Full: return pt ? "A PARTIDA ESTA CHEIA" : "THE GAME IS FULL";
        case Reason::Password: return pt ? "SENHA ERRADA" : "WRONG PASSWORD";
        case Reason::Kicked: return pt ? "VOCE FOI EXPULSO" : "YOU WERE KICKED";
        case Reason::Started: return pt ? "A PARTIDA JA COMECOU" : "THE GAME HAS STARTED";
        case Reason::Version: return pt ? "VERSAO DIFERENTE" : "DIFFERENT VERSION";
        case Reason::NoAnswer: return pt ? "SEM RESPOSTA DO HOST" : "NO ANSWER FROM THE HOST";
        case Reason::HostLeft: return pt ? "O HOST SAIU" : "THE HOST LEFT";
        case Reason::PortTaken: return pt ? "PORTA EM USO" : "PORT IN USE";
        default: return "";
        }
    }

    std::vector<Player> Players()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.state == State::Hosting)
            return HostRoster(g_s);
        return g_s.roster;
    }

    std::string InviteCode()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        uint32_t ip;
        int port;
        if (g_s.state == State::Hosting && nat::PublicAddress(ip, port))
            return EncodeCode(ip, port);
        if (g_s.state == State::Hosting)
            return EncodeCode(g_s.localIp, net::LocalPort());
        if (g_s.state == State::Joined || g_s.state == State::Joining)
            return EncodeCode(g_s.hostIp, g_s.hostPort);
        return {};
    }

    std::string LocalCode()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_s.state == State::Hosting ? EncodeCode(g_s.localIp, net::LocalPort()) : std::string();
    }

    const char* InternetText(bool pt)
    {
        switch (nat::GetState())
        {
        case nat::State::Working: return pt ? "Internet: verificando..." : "Internet: checking...";
        case nat::State::Open: return pt ? "Internet: aberta" : "Internet: open";
        case nat::State::Unknown: return pt ? "Internet: abra a porta UDP 47810 no roteador" : "Internet: forward UDP port 47810 on the router";
        case nat::State::Closed: return pt ? "Internet: sua rede não permite (use uma VPN)" : "Internet: your network does not allow it (use a VPN)";
        default: return "";
        }
    }

    std::string Address()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.state == State::Hosting)
            return FormatAddress(g_s.localIp, net::LocalPort());
        return FormatAddress(g_s.hostIp, g_s.hostPort);
    }

    void Kick(uint32_t slot)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.state != State::Hosting || slot == 0 || slot >= MAX_PLAYERS || !g_s.members[slot].used)
            return;
        Member& m = g_s.members[slot];
        Log("[lobby] %s kicked", m.name.c_str());
        Deny(g_s, m.ip, m.port, m.token, Reason::Kicked);
        m = {};
        Compact(g_s);
        ApplyHost(g_s);
        SendRoster(g_s);
    }

    void SetPassword(const std::string& password)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_s.password = password;
        Log("[lobby] password %s", password.empty() ? "removed" : "set");
    }

    std::string Password()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_s.password;
    }

    Options GetOptions()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_s.options;
    }

    void SetOptions(const Options& options)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.state != State::Hosting)
            return;
        g_s.options = options;
        g_s.options.maxPlayers = std::clamp<uint32_t>(options.maxPlayers, std::max<uint32_t>(2, Count(g_s)), MAX_PLAYERS);
        Log("[lobby] options: %u players, knock-out %u s", g_s.options.maxPlayers, g_s.options.knockOutSeconds);
        SendRoster(g_s);
    }

    bool HostInGame()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_s.state == State::Hosting ? g_s.inGame : g_s.inGameHost;
    }

    void HostLevelStarted(int32_t levelIndex)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.state != State::Hosting || g_s.inGame)
            return;
        Compact(g_s);
        ApplyHost(g_s);
        g_s.inGame = true;
        g_s.startLevel = levelIndex;
        g_s.startId++;
        Log("[lobby] level %d started with %u player(s)", levelIndex, Count(g_s));
        SendRoster(g_s);
    }

    void HostBackInMenus()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.state != State::Hosting || !g_s.inGame)
            return;
        g_s.inGame = false;
        Log("[lobby] back in the menus: players can join again");
        SendRoster(g_s);
    }

    bool TakeStart(int32_t& levelIndex)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.pendingStart < 0)
            return false;
        levelIndex = g_s.pendingStart;
        g_s.pendingStart = -1;
        return true;
    }

    void HostSkippedCutscene(uint32_t node)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.state != State::Hosting)
            return;
        g_s.skipId++;
        g_s.skipNode = node;
        SendRoster(g_s);
    }

    bool TakeCutsceneSkip(uint32_t node)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_s.pendingSkipNode || NowMs() - g_s.pendingSkipAt > SKIP_KEEP_MS)
            return g_s.pendingSkipNode = 0, false;
        if (g_s.pendingSkipNode != node)
            return false;
        g_s.pendingSkipNode = 0;
        return true;
    }

    bool IsClient()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_s.state == State::Joined;
    }

    bool TakeHostLeftLevel()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return std::exchange(g_s.hostLeftLevel, false);
    }

    uint32_t KnockOutSeconds()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_s.state == State::Hosting || g_s.state == State::Joined ? g_s.options.knockOutSeconds : 0;
    }

    // ---- codes -----------------------------------------------------------------------------------

    namespace
    {
        constexpr char ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford
    }

    // 7 letters (XXXX-XXX) for the address alone on the default port, 10 (XXXXX-XXXXX) with the port.
    std::string EncodeCode(uint32_t ip, int port)
    {
        bool shortCode = port == DEFAULT_PORT;
        int digits = shortCode ? 7 : 10;
        uint64_t value = shortCode ? ip : (uint64_t(ip) << 16) | uint16_t(port);
        std::string out;
        for (int i = digits - 1; i >= 0; --i)
        {
            out += ALPHABET[(value >> (i * 5)) & 31];
            if (i == digits / 2 && i)
                out += '-';
        }
        return out;
    }

    bool DecodeAddress(const std::string& text, uint32_t& ip, int& port)
    {
        std::string t;
        for (char c : text)
            if (!std::isspace(uint8_t(c)))
                t += c;
        if (t.find('.') != std::string::npos)
        {
            unsigned a, b, c, d, p = DEFAULT_PORT;
            int fields = sscanf(t.c_str(), "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p);
            if (fields < 4 || a > 255 || b > 255 || c > 255 || d > 255 || p == 0 || p > 65535)
                return false;
            ip = (a << 24) | (b << 16) | (c << 8) | d;
            port = int(p);
            return true;
        }
        uint64_t value = 0;
        int digits = 0;
        for (char c : t)
        {
            if (c == '-')
                continue;
            char u = char(std::toupper(uint8_t(c)));
            if (u == 'O')
                u = '0';
            if (u == 'I' || u == 'L')
                u = '1';
            const char* at = strchr(ALPHABET, u);
            if (!at || !*at)
                return false;
            value = (value << 5) | uint64_t(at - ALPHABET);
            ++digits;
        }
        if (digits == 7 && value <= 0xFFFFFFFFull)
        {
            ip = uint32_t(value);
            port = DEFAULT_PORT;
            return ip != 0;
        }
        if (digits != 10)
            return false;
        ip = uint32_t(value >> 16);
        port = int(value & 0xFFFF);
        return ip != 0 && port != 0;
    }

    std::string FormatAddress(uint32_t ip, int port)
    {
        char text[32];
        snprintf(text, sizeof(text), "%u.%u.%u.%u:%d", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
        return text;
    }

    // ---- I/O thread ------------------------------------------------------------------------------

    void OnPacket(uint32_t ip, int port, const uint8_t* data, size_t size)
    {
        Reader r{ data + 4, data + size };
        Type type = Type(r.Get8());
        uint32_t session = r.Get32();
        if (!r.ok)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.state == State::Hosting)
        {
            if (type == DISCOVER || type == HELLO || session == g_s.sessionId)
                HostPacket(g_s, type, r, ip, port);
        }
        else
            ClientPacket(g_s, type, session, r, ip, port);
    }

    void IoTick()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_s.state == State::Hosting)
            HostTick(g_s);
        else if (g_s.state != State::Off && g_s.state != State::Failed)
            ClientTick(g_s);
    }
}
