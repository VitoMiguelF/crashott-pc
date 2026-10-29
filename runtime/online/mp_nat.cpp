// UPnP port mapping and STUN for a hosted session (mp_nat.h).
#include "mp_nat.h"
#include "mp.h"
#include "../diagnostics.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
using socket_t = SOCKET;
static void CloseSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t INVALID_SOCKET = -1;
static void CloseSocket(socket_t s) { close(s); }
#endif

namespace
{
    using mp::nat::State;

    std::mutex g_mutex;   // the public address and the STUN transaction
    std::mutex g_router;  // the router's fields below, held over the UPnP exchanges (seconds)
    std::atomic<State> g_state{ State::Off };
    std::atomic<uint32_t> g_generation{ 0 };  // a later Open / Close makes a running one stale
    uint32_t g_publicIp = 0;
    int g_publicPort = 0;
    // The routers (g_router): this network's and, behind two (an operator's modem and a router of
    // one's own), the one above it; their control URL and service and the external port mapped.
    struct Router
    {
        uint32_t ip = 0;
        int port = 0, mapped = 0;
        std::string control, service;
    };
    Router g_routers[2];
    uint32_t g_mappedGeneration = 0;  // the Open that made the mappings
    // STUN: the request's transaction id and the address in the answer.
    uint8_t g_transaction[12];
    std::atomic<uint64_t> g_stunAnswer{ 0 };  // ip << 16 | port, 0 = none yet

    constexpr uint32_t STUN_COOKIE = 0x2112A442;

    bool IsPrivate(uint32_t ip)
    {
        return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 24) == 0 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 ||
               (ip >> 16) == 0xA9FE || (ip >> 22) == (0x64400000u >> 22);  // 100.64/10: carrier-grade NAT
    }

    void SetTimeout(socket_t s, int ms)
    {
#ifdef _WIN32
        DWORD t = DWORD(ms);
#else
        timeval t{ ms / 1000, (ms % 1000) * 1000 };
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    }

    sockaddr_in Address(uint32_t ip, int port)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(ip);
        a.sin_port = htons(uint16_t(port));
        return a;
    }

    size_t FindNoCase(const std::string& text, const char* key, size_t from = 0)
    {
        size_t n = strlen(key);
        for (size_t i = from; i + n <= text.size(); ++i)
        {
            size_t k = 0;
            while (k < n && tolower(uint8_t(text[i + k])) == tolower(uint8_t(key[k])))
                ++k;
            if (k == n)
                return i;
        }
        return std::string::npos;
    }

    // The text between <name> and </name> after `from` ("" if none).
    std::string Tag(const std::string& xml, const char* name, size_t from = 0)
    {
        std::string open = std::string("<") + name + ">", close = std::string("</") + name + ">";
        size_t a = xml.find(open, from);
        size_t b = a == std::string::npos ? a : xml.find(close, a);
        return b == std::string::npos ? "" : xml.substr(a + open.size(), b - a - open.size());
    }

    // "http://192.168.0.1:5000/path" (routers give their address as digits).
    bool ParseUrl(const std::string& url, uint32_t& ip, int& port, std::string& path)
    {
        unsigned a, b, c, d, p = 80;
        char rest[512] = "/";
        if (sscanf(url.c_str(), "http://%u.%u.%u.%u:%u%511s", &a, &b, &c, &d, &p, rest) < 5 &&
            sscanf(url.c_str(), "http://%u.%u.%u.%u%511s", &a, &b, &c, &d, rest) < 4)
            return false;
        ip = (a << 24) | (b << 16) | (c << 8) | d;
        port = int(p);
        path = rest[0] == '/' ? rest : "/";
        return true;
    }

    // SSDP: the router's description URL (its LOCATION); `unicast`: asked of that address. Sent from the
    // LAN address: Windows otherwise multicasts on whatever adapter it likes and the router never hears it.
    std::string Discover(uint32_t localIp, uint32_t unicast)
    {
        socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
            return {};
        SetTimeout(s, 700);
        sockaddr_in me = Address(localIp, 0);
        in_addr interfaceIp{};
        interfaceIp.s_addr = htonl(localIp);
        bind(s, reinterpret_cast<sockaddr*>(&me), sizeof(me));
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&interfaceIp), sizeof(interfaceIp));
        const char* search = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\n"
                             "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
        sockaddr_in to = Address(unicast ? unicast : 0xEFFFFFFA, 1900);
        std::string location;
        for (int attempt = 0; attempt < 3 && location.empty(); ++attempt)
        {
            sendto(s, search, int(strlen(search)), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
            char buffer[1500];
            for (int got; location.empty() && (got = int(recv(s, buffer, sizeof(buffer) - 1, 0))) > 0;)
            {
                std::string answer(buffer, size_t(got));
                size_t at = FindNoCase(answer, "\r\nLOCATION:");
                if (at == std::string::npos)
                    continue;
                at += 11;
                size_t end = answer.find("\r\n", at);
                location = answer.substr(at, end - at);
                location.erase(0, location.find_first_not_of(' '));
            }
        }
        CloseSocket(s);
        return location;
    }

    // A plain HTTP/1.1 exchange with the router; its status and body. `local`: the address this machine
    // has towards the router (the mapping's internal client).
    int Http(uint32_t ip, int port, const std::string& request, std::string& body, uint32_t* local = nullptr)
    {
        socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return 0;
        SetTimeout(s, 2000);
        sockaddr_in to = Address(ip, port);
        std::string answer;
        if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0)
        {
            if (local)
            {
                sockaddr_in me{};
                socklen_t size = sizeof(me);
                getsockname(s, reinterpret_cast<sockaddr*>(&me), &size);
                *local = ntohl(me.sin_addr.s_addr);
            }
            send(s, request.data(), int(request.size()), 0);
            char buffer[2048];
            for (int got; answer.size() < 256 * 1024 && (got = int(recv(s, buffer, sizeof(buffer), 0))) > 0;)
                answer.append(buffer, size_t(got));
        }
        CloseSocket(s);
        int status = 0;
        sscanf(answer.c_str(), "HTTP/%*s %d", &status);
        size_t at = answer.find("\r\n\r\n");
        body = at == std::string::npos ? "" : answer.substr(at + 4);
        return status;
    }

    std::string Dotted(uint32_t ip)
    {
        char text[16];
        snprintf(text, sizeof(text), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
        return text;
    }

    // The Host header: the router's address (MiniUPnPd answers 404 to a name, against DNS rebinding).
    std::string RouterHost(const Router& r) { return Dotted(r.ip) + ":" + std::to_string(r.port); }

    int Soap(const Router& r, const char* action, const std::string& arguments, std::string& reply, uint32_t* local = nullptr)
    {
        std::string body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + std::string(action) +
                           " xmlns:u=\"" + r.service + "\">" + arguments + "</u:" + action + "></s:Body></s:Envelope>";
        std::string request = "POST " + r.control + " HTTP/1.1\r\nHost: " + RouterHost(r) + "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
                              "SOAPAction: \"" + r.service + "#" + action + "\"\r\nContent-Length: " + std::to_string(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body;
        return Http(r.ip, r.port, request, reply, local);
    }

    // UPnP: the router's WAN connection service (control / service) from its description.
    bool FindRouter(Router& r, uint32_t localIp, uint32_t unicast)
    {
        std::string location = Discover(localIp, unicast), path, description;
        if (location.empty() || !ParseUrl(location, r.ip, r.port, path))
            return false;
        if (Http(r.ip, r.port, "GET " + path + " HTTP/1.1\r\nHost: " + RouterHost(r) + "\r\nConnection: close\r\n\r\n", description) != 200)
            return false;
        for (const char* type : { "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
                                  "urn:schemas-upnp-org:service:WANPPPConnection:1" })
        {
            size_t at = description.find(std::string("<serviceType>") + type + "</serviceType>");
            std::string control = at == std::string::npos ? "" : Tag(description, "controlURL", at);
            if (control.empty())
                continue;
            uint32_t ip;
            int port;
            if (control.rfind("http://", 0) == 0)
                ParseUrl(control, ip, port, control);
            r.control = control[0] == '/' ? control : "/" + control;
            r.service = type;
            return true;
        }
        return false;
    }

    // The router's WAN address (0 if it doesn't say: MiniUPnPd keeps a private one to itself);
    // `local`: this machine's address towards it.
    uint32_t ExternalIp(const Router& r, uint32_t* local = nullptr)
    {
        std::string reply;
        unsigned a, b, c, d;
        if (Soap(r, "GetExternalIPAddress", "", reply, local) != 200 ||
            sscanf(Tag(reply, "NewExternalIPAddress").c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
            return 0;
        return (a << 24) | (b << 16) | (c << 8) | d;
    }

    // UPnP: the router forwards an external UDP port to `client`'s `port` (0: this machine); the
    // external port (r.mapped), 0 if not.
    int MapPort(Router& r, int port, uint32_t client)
    {
        uint32_t local = 0;
        ExternalIp(r, &local);
        std::string ip = Dotted(client ? client : local), reply;
        for (int external = port; external < port + 4; ++external)
            for (int lease : { 7200, 0 })  // some routers only take permanent ones (error 725)
            {
                std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(external) +
                                   "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(port) +
                                   "</NewInternalPort><NewInternalClient>" + ip + "</NewInternalClient><NewEnabled>1</NewEnabled>"
                                   "<NewPortMappingDescription>Crash MULTIPLAYER</NewPortMappingDescription><NewLeaseDuration>" +
                                   std::to_string(lease) + "</NewLeaseDuration>";
                if (Soap(r, "AddPortMapping", args, reply) == 200)
                    return r.mapped = external;
                if (reply.find("725") == std::string::npos)
                    break;  // not a lease problem: the port is taken (718), try the next one
            }
        return 0;
    }

    // STUN (RFC 5389) on the game's socket: this machine's address as the internet sees it; `server`:
    // the STUN server's address.
    uint64_t Stun(uint32_t& server)
    {
        for (auto [name, port] : { std::pair{ "stun.l.google.com", "19302" }, std::pair{ "stun.cloudflare.com", "3478" } })
        {
            addrinfo hints{}, *found = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(name, port, &hints, &found) != 0 || !found)
                continue;
            server = ntohl(reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr);
            freeaddrinfo(found);
            uint8_t request[20] = { 0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42 };
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                std::random_device random;
                for (uint8_t& b : g_transaction)
                    b = uint8_t(random());
                memcpy(request + 8, g_transaction, 12);
            }
            g_stunAnswer = 0;
            for (int attempt = 0; attempt < 6 && !g_stunAnswer; ++attempt)
            {
                if (attempt % 2 == 0)
                    mp::net::SendTo(server, atoi(port), request, sizeof(request));
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (g_stunAnswer)
                return g_stunAnswer;
        }
        return 0;
    }

    // Two routers deep (the first's WAN address private): the second's address, two hops towards
    // `target`, and the first's WAN address, the first one recorded on the way to the second (an
    // ICMP echo with the IP record-route option). Windows only (an unprivileged ICMP API).
    bool RouterAbove(uint32_t target, uint32_t& above, uint32_t& wan)
    {
        above = wan = 0;
#ifdef _WIN32
        HANDLE icmp = IcmpCreateFile();
        if (icmp == INVALID_HANDLE_VALUE)
            return false;
        char data[8] = {}, reply[256] = {};
        auto* answer = reinterpret_cast<ICMP_ECHO_REPLY*>(reply);
        IP_OPTION_INFORMATION options{};
        options.Ttl = 2;
        IcmpSendEcho(icmp, htonl(target), data, sizeof(data), &options, reply, sizeof(reply), 1000);
        if (answer->Status == IP_TTL_EXPIRED_TRANSIT)
            above = ntohl(answer->Address);
        uint8_t route[40] = { 7, 39, 4 };  // record route, room for 9 addresses
        options.Ttl = 64;
        options.OptionsData = route;
        options.OptionsSize = sizeof(route);
        memset(reply, 0, sizeof(reply));
        if (above && IcmpSendEcho(icmp, htonl(above), data, sizeof(data), &options, reply, sizeof(reply), 1000) &&
            answer->Status == IP_SUCCESS && answer->Options.OptionsSize >= 7 && answer->Options.OptionsData[0] == 7 &&
            answer->Options.OptionsData[2] >= 8)
        {
            const uint8_t* at = answer->Options.OptionsData + 3;
            wan = uint32_t(at[0]) << 24 | at[1] << 16 | at[2] << 8 | at[3];
        }
        IcmpCloseHandle(icmp);
#else
        (void)target;
#endif
        return above && wan && IsPrivate(above) && IsPrivate(wan);
    }

    void Work(int port, uint32_t localIp, uint32_t generation)
    {
        uint32_t stunServer = 0;
        uint64_t stun = Stun(stunServer);
        uint32_t seen = uint32_t(stun >> 16), external = 0, above = 0, wan = 0;
        int mapped = 0;
        {
            std::lock_guard<std::mutex> lock(g_router);
            g_routers[0] = g_routers[1] = Router{};
            g_mappedGeneration = generation;
            if (FindRouter(g_routers[0], localIp, 0) && (mapped = MapPort(g_routers[0], port, 0)) != 0)
            {
                external = ExternalIp(g_routers[0]);
                // Behind a second router: it forwards the port to the first's WAN address.
                if ((!external || IsPrivate(external)) && stunServer && RouterAbove(stunServer, above, wan))
                {
                    mapped = FindRouter(g_routers[1], localIp, above) ? MapPort(g_routers[1], mapped, wan) : 0;
                    external = mapped ? ExternalIp(g_routers[1]) : 0;
                }
            }
        }
        State state = State::Closed;
        uint32_t ip = 0;
        int publicPort = port;
        if (mapped && external && !IsPrivate(external) && (!seen || seen == external))
            state = State::Open, ip = external, publicPort = mapped;
        else if (seen && !IsPrivate(seen))
            state = State::Unknown, ip = seen;  // a port forwarded by hand would be the game's own
        std::lock_guard<std::mutex> lock(g_mutex);
        if (generation != g_generation)
            return;  // closed meanwhile (Close's thread removes the mappings)
        g_publicIp = ip;
        g_publicPort = publicPort;
        g_state = state;
        std::string routers = g_routers[0].control.empty() ? "none" : Dotted(g_routers[0].ip);
        if (above)
            routers += " + " + Dotted(above) + (g_routers[1].control.empty() ? " (no UPnP)" : "") + " via " + Dotted(wan);
        Log("[nat] UPnP %s (routers %s, external %s), STUN %s:%u: %s", mapped ? "mapped" : "no", routers.c_str(),
            Dotted(external).c_str(), Dotted(seen).c_str(), unsigned(stun & 0xFFFF),
            state == State::Open ? "open to the internet" : state == State::Unknown ? "public address, port not confirmed" : "LAN only");
    }
}

namespace mp::nat
{
    void Open(int port, uint32_t localIp)
    {
        if (const char* nat = getenv("CRASH_MP_NAT"); nat && !strcmp(nat, "0"))
            return;  // tests (tools/run_game.py)
        Close();
        uint32_t generation;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            generation = ++g_generation;
            g_state = State::Working;
        }
        std::thread(Work, port, localIp, generation).detach();
    }

    void Close()
    {
        uint32_t closing;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            closing = ++g_generation;
            g_state = State::Off;
            g_publicIp = 0;
        }
        // After mappings still being made (g_router); not those of a later Open.
        std::thread([closing] {
            std::lock_guard<std::mutex> lock(g_router);
            if (g_mappedGeneration > closing)
                return;
            for (Router& r : g_routers)
            {
                if (!r.mapped)
                    continue;
                std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(r.mapped) +
                                   "</NewExternalPort><NewProtocol>UDP</NewProtocol>", reply;
                Log("[nat] port mapping on %s %s", Dotted(r.ip).c_str(),
                    Soap(r, "DeletePortMapping", args, reply) == 200 ? "removed" : "not removed (it expires)");
                r.mapped = 0;
            }
        }).detach();
    }

    State GetState() { return g_state; }

    bool PublicAddress(uint32_t& ip, int& port)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ip = g_publicIp, port = g_publicPort;
        return g_publicIp != 0;
    }

    bool OnPacket(const uint8_t* data, size_t size)
    {
        auto get16 = [&](size_t at) { return uint32_t(data[at] << 8 | data[at + 1]); };
        if (size < 20 || get16(0) != 0x0101 || get16(4) != (STUN_COOKIE >> 16) || get16(6) != (STUN_COOKIE & 0xFFFF))
            return false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (memcmp(data + 8, g_transaction, 12) != 0)
                return true;  // not our request's (dropped)
        }
        for (size_t at = 20; at + 4 <= size;)
        {
            uint32_t type = get16(at), length = get16(at + 2);
            if (at + 4 + length > size)
                break;
            if ((type == 0x0020 || type == 0x0001) && length >= 8 && data[at + 5] == 1)
            {
                uint32_t port = get16(at + 6), ip = get16(at + 8) << 16 | get16(at + 10);
                if (type == 0x0020)
                    port ^= STUN_COOKIE >> 16, ip ^= STUN_COOKIE;
                g_stunAnswer = uint64_t(ip) << 16 | port;
                break;
            }
            at += 4 + ((length + 3) & ~3u);
        }
        return true;
    }
}
