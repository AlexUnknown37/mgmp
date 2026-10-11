// mgmp_lan.h -- finding a host on the local network, and the player's name.
//
// Same idea as Gungeon Online's "Find LAN games": the joining player shouts
// "any hosts?" as a UDP broadcast on the session port, and every host running
// this mod answers with its name, its TCP port and its protocol version. The
// search also asks 127.0.0.1 directly, so two copies of the game on one PC find
// each other, and every adapter's own subnet broadcast, because on a PC with a
// VPN adapter (Radmin, Hamachi, ZeroTier) 255.255.255.255 may only leave by one
// of them.
//
// Discovery is NOT the session. It only fills a list; joining still goes through
// the ordinary TCP connect in mgmp_net. Nothing here touches the simulation or
// any save, and nothing here blocks the frame thread: both sockets are
// non-blocking and drained from lan_pump().
#pragma once

#include <cstdint>

namespace mgmp {

constexpr uint32_t kLanMaxFound = 16;

struct LanHost {
    char     name[32];
    char     addr[48];       // dotted IPv4 the reply came from
    uint16_t port;           // the host's TCP session port
    uint8_t  players;        // peers in the host's session, host included
    bool     same_version;   // protocol version matches ours
};

// Reads mgmp_user.ini beside the DLL (player name, last address typed).
void lan_init(const wchar_t* dll_dir);

// Once per frame, from session_update. Opens the host's responder while this
// peer is hosting and closes it otherwise; drains replies to a running search.
void lan_pump();

// Starts (or restarts) a search for hosts listening on `port`. Results arrive
// over the next few frames.
void lan_search(uint16_t port);
void lan_stop_search();
bool lan_searching();              // replies still being accepted
uint32_t lan_found(LanHost* out, uint32_t cap);

// This PC's IPv4 addresses, best LAN guess first, joined with ", ".
void lan_local_addresses(char* out, uint32_t cap);

// The name shown to other players: the saved name, else the Steam persona
// name, else the Windows user name.
const char* lan_display_name();
const char* lan_saved_name();       // "" when none is saved
const char* lan_steam_name();       // "" when Steam did not answer
void        lan_save_name(const char* name);

const char* lan_last_addr();
void        lan_save_last_addr(const char* addr);

} // namespace mgmp
