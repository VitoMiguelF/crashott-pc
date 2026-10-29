#pragma once
// A hosted session over the internet without a server of ours (docs/internals/multiplayer.md, "Internet"):
// the host asks its router to forward the game's UDP port (UPnP IGD: SSDP discovery, AddPortMapping) and
// learns its public address from a public STUN server (stun.l.google.com, on the game's own socket); the
// invite code then carries the public address. Behind two routers (an operator's modem and one's own) both
// get the mapping (Windows: the modem found by ICMP). A host behind carrier-grade NAT (4G, some fibre) or
// with UPnP off can only be joined on its network, over a VPN or with the port forwarded by hand.

#include <cstddef>
#include <cstdint>

namespace mp::nat
{
    enum class State { Off, Working, Open, Unknown, Closed };
    // Open: the router forwards the port and the address is public. Unknown: no UPnP, but STUN gave a
    // public address (works if the port was forwarded by hand). Closed: no public address (CGNAT).

    void Open(int port, uint32_t localIp);  // host: starts the work on its own thread (LAN address, host order)
    void Close();         // the session closed: the port mapping goes (on its own thread)
    State GetState();
    bool PublicAddress(uint32_t& ip, int& port);  // host byte order
    // I/O thread (mp_net.cpp): a STUN answer on the game's socket; true if it was one.
    bool OnPacket(const uint8_t* data, size_t size);
}
