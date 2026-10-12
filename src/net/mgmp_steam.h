// mgmp_steam.h -- Steam invites and joining (our fork).
//
// HOW IT FITS. mgmp's transport is a TCP connection (mgmp_net) and stays
// exactly as it is. Steam only replaces the wire between the two machines:
//
//   guest mgmp --TCP--> 127.0.0.1:<bridge port> ==Steam relay==> host bridge --TCP--> 127.0.0.1:<host port> host mgmp
//
// The bridge carries mgmp's byte stream inside reliable, ordered
// SteamNetworkingMessages (channel 0), so nothing above the socket can tell a
// Steam session from a LAN one -- every feature that works on LAN works here.
//
// FINDING EACH OTHER is a friends-only Steam lobby. The host creates it, sets
// rich presence "connect" so friends see "Join Game", and can open Steam's
// invite dialog. A guest that accepts an invite (GameLobbyJoinRequested_t),
// clicks Join Game (GameRichPresenceJoinRequested_t) or is launched by Steam
// with "+connect_lobby <id>" joins the lobby; the lobby's owner is the host.
//
// Everything goes through the flat API exported by the game's own
// steam_api64.dll (the game itself never links SteamMatchmaking). Callbacks
// are registered with SteamAPI_RegisterCallback and delivered by the game's own
// SteamAPI_RunCallbacks on its main thread. All pumping is non-blocking and runs
// from session_update.
#pragma once

#include <cstdint>

namespace mgmp {

// After config_load. Safe without Steam: everything reports unavailable.
void steam_init();
void steam_pump();            // once per frame, from session_update

bool        steam_available();
const char* steam_status();   // one line for the UI ("" when idle)

// Host: start hosting (the ordinary TCP host on `port`) and open a lobby.
void steam_host(uint16_t port);
// Host: open the Steam overlay's invite dialog for our lobby.
void steam_invite();
bool steam_hosting();         // a lobby of ours is up

// Either side: leave the lobby and stop the bridge (the session itself is
// left by the caller).
void steam_leave();
bool steam_in_session();      // hosting or joined through Steam

} // namespace mgmp
