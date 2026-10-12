#include "mgmp_steam.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_log.h"
#include "mgmp_session.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdarg>

#pragma comment(lib, "ws2_32.lib")

namespace mgmp {
namespace {

// --- the flat API, resolved from the game's steam_api64.dll -----------------

typedef void*    (__cdecl* fn_iface)();
typedef uint64_t (__cdecl* fn_create_lobby)(void*, int type, int max_members);
typedef uint64_t (__cdecl* fn_join_lobby)(void*, uint64_t lobby);
typedef void     (__cdecl* fn_leave_lobby)(void*, uint64_t lobby);
typedef uint64_t (__cdecl* fn_lobby_owner)(void*, uint64_t lobby);
typedef bool     (__cdecl* fn_set_lobby_data)(void*, uint64_t lobby, const char* k, const char* v);
typedef bool     (__cdecl* fn_set_rich)(void*, const char* k, const char* v);
typedef void     (__cdecl* fn_clear_rich)(void*);
typedef void     (__cdecl* fn_invite_dialog)(void*, uint64_t lobby);
typedef uint64_t (__cdecl* fn_get_steamid)(void*);
typedef bool     (__cdecl* fn_call_done)(void*, uint64_t call, bool* failed);
typedef bool     (__cdecl* fn_call_result)(void*, uint64_t call, void* out, int cb, int expected, bool* failed);
typedef int      (__cdecl* fn_send_msg)(void*, const void* identity, const void* data, uint32_t cb, int flags, int channel);
typedef int      (__cdecl* fn_recv_msgs)(void*, int channel, void** out, int max);
typedef bool     (__cdecl* fn_accept)(void*, const void* identity);
typedef bool     (__cdecl* fn_close_session)(void*, const void* identity);
typedef void     (__cdecl* fn_msg_release)(void* msg);
typedef void     (__cdecl* fn_ident_set)(void* identity, uint64_t id);
typedef uint64_t (__cdecl* fn_ident_get)(const void* identity);
typedef void     (__cdecl* fn_register_cb)(void* cb, int id);

struct Api {
    void* matchmaking = nullptr;
    void* friends     = nullptr;
    void* user        = nullptr;
    void* utils       = nullptr;
    void* messages    = nullptr;

    fn_create_lobby   create_lobby   = nullptr;
    fn_join_lobby     join_lobby     = nullptr;
    fn_leave_lobby    leave_lobby    = nullptr;
    fn_lobby_owner    lobby_owner    = nullptr;
    fn_set_lobby_data set_lobby_data = nullptr;
    fn_set_rich       set_rich       = nullptr;
    fn_clear_rich     clear_rich     = nullptr;
    fn_invite_dialog  invite_dialog  = nullptr;
    fn_get_steamid    get_steamid    = nullptr;
    fn_call_done      call_done      = nullptr;
    fn_call_result    call_result    = nullptr;
    fn_send_msg       send_msg       = nullptr;
    fn_recv_msgs      recv_msgs      = nullptr;
    fn_accept         accept         = nullptr;
    fn_close_session  close_session  = nullptr;
    fn_msg_release    msg_release    = nullptr;
    fn_ident_set      ident_set      = nullptr;
    fn_ident_get      ident_get      = nullptr;
    fn_register_cb    register_cb    = nullptr;
};

// Callback ids and layouts (Steamworks, Windows packing = natural alignment).
constexpr int kCb_LobbyEnter             = 504;   // LobbyEnter_t
constexpr int kCb_LobbyCreated           = 513;   // LobbyCreated_t
constexpr int kCb_GameLobbyJoinRequested = 333;   // GameLobbyJoinRequested_t
constexpr int kCb_RichPresenceJoin       = 337;   // GameRichPresenceJoinRequested_t
constexpr int kCb_SessionRequest         = 1251;  // SteamNetworkingMessagesSessionRequest_t

struct LobbyCreated { int32_t result; uint32_t pad; uint64_t lobby; };
struct LobbyEnter   { uint64_t lobby; uint32_t perms; uint8_t locked; uint8_t pad[3]; uint32_t response; uint32_t pad2; };
struct LobbyJoinReq { uint64_t lobby; uint64_t friend_id; };
struct RichJoinReq  { uint64_t friend_id; char connect[256]; };

constexpr int kSendReliable   = 8;      // k_nSteamNetworkingSend_Reliable
constexpr int kLobbyFriends   = 1;      // k_ELobbyTypeFriendsOnly
constexpr int kIdentityBytes  = 136;    // sizeof(SteamNetworkingIdentity)
constexpr int kMsgIdentityOff = 16;     // SteamNetworkingMessage_t::m_identityPeer
constexpr uint32_t kChunk     = 64 * 1024;

// Bridge frames: 1 byte type, then payload.
enum : uint8_t { kData = 0, kClose = 1 };

// Steam's CCallbackBase, laid out the way MSVC lays out the real one: the
// overloaded Run() virtuals come out in REVERSE declaration order, so the
// call-result overload is slot 0 and the plain callback slot 1.
class Callback {
public:
    virtual void run_call_result(void* param, bool io_failure, uint64_t call) { (void)io_failure; (void)call; run(param); }
    virtual void run(void* param) = 0;
    virtual int  size() = 0;
    uint8_t flags = 0;
    int     id    = 0;
};

void on_lobby_join_requested(const LobbyJoinReq& r);
void on_rich_join_requested(const RichJoinReq& r);
void on_session_request(const void* identity);

class CbLobbyJoin : public Callback {
public:
    void run(void* p) override { on_lobby_join_requested(*(const LobbyJoinReq*)p); }
    int  size() override { return (int)sizeof(LobbyJoinReq); }
};
class CbRichJoin : public Callback {
public:
    void run(void* p) override { on_rich_join_requested(*(const RichJoinReq*)p); }
    int  size() override { return (int)sizeof(RichJoinReq); }
};
class CbSession : public Callback {
public:
    void run(void* p) override { on_session_request(p); }
    int  size() override { return kIdentityBytes; }
};

// One mgmp TCP stream carried over Steam to one remote player.
struct Link {
    uint64_t    peer = 0;              // remote SteamID
    SOCKET      sock = INVALID_SOCKET; // local TCP side
    bool        connecting = false;    // host: our connect to mgmp in flight
    std::string to_sock;               // bytes from Steam not yet written to the socket
    bool        dead = false;
};

enum class Mode { Off, HostCreating, Hosting, GuestJoining, Guest };

struct State {
    bool     tried  = false;
    bool     ok     = false;
    Api      api;
    uint64_t self   = 0;

    Mode     mode   = Mode::Off;
    uint64_t lobby  = 0;
    uint64_t call   = 0;            // pending CreateLobby / JoinLobby
    uint16_t host_port = 0;         // host: mgmp's TCP port
    uint64_t host_id   = 0;         // guest: the lobby owner

    SOCKET   listener   = INVALID_SOCKET;   // guest: where mgmp connects
    uint16_t guest_port = 0;
    std::vector<Link> links;

    uint64_t pending_join = 0;      // a lobby to join as soon as we can
    char     status[160] = {};

    CbLobbyJoin cb_lobby;
    CbRichJoin  cb_rich;
    CbSession   cb_session;
};

State g;

void set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(g.status, sizeof(g.status), _TRUNCATE, fmt, ap);
    va_end(ap);
    log_line("STEAM", "%s", g.status);
}

template <class F> bool resolve(HMODULE m, const char* name, F& out) {
    out = (F)GetProcAddress(m, name);
    if (!out) log_line_lvl(LogLevel::Warn, "STEAM", "!! %s is missing from steam_api64.dll", name);
    return out != nullptr;
}

void identity_for(uint64_t id, uint8_t (&buf)[kIdentityBytes]) {
    memset(buf, 0, sizeof(buf));
    g.api.ident_set(buf, id);
}

bool steam_send(uint64_t peer, uint8_t type, const char* data, uint32_t len) {
    std::vector<uint8_t> frame(1 + len);
    frame[0] = type;
    if (len) memcpy(frame.data() + 1, data, len);
    uint8_t ident[kIdentityBytes];
    identity_for(peer, ident);
    const int r = g.api.send_msg(g.api.messages, ident, frame.data(), (uint32_t)frame.size(),
                                 kSendReliable, 0);
    return r == 1;   // k_EResultOK
}

void set_nonblocking(SOCKET s) { u_long nb = 1; ioctlsocket(s, FIONBIO, &nb); }

Link* find_link(uint64_t peer) {
    for (auto& l : g.links) if (l.peer == peer && !l.dead) return &l;
    return nullptr;
}

void close_link(Link& l, bool tell_peer) {
    if (l.sock != INVALID_SOCKET) { closesocket(l.sock); l.sock = INVALID_SOCKET; }
    if (tell_peer && l.peer) steam_send(l.peer, kClose, nullptr, 0);
    l.dead = true;
}

// Host: a TCP connection into our own mgmp host for a new Steam peer.
bool open_host_side(Link& l) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    set_nonblocking(s);
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(g.host_port);
    connect(s, (sockaddr*)&a, sizeof(a));   // non-blocking: completes later
    l.sock       = s;
    l.connecting = true;
    return true;
}

void pump_links() {
    // Steam -> socket
    void* msgs[32];
    for (int round = 0; round < 8; ++round) {
        const int n = g.api.recv_msgs(g.api.messages, 0, msgs, 32);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            const uint8_t* m = (const uint8_t*)msgs[i];
            const void* data = *(void* const*)m;
            const int   cb   = *(const int*)(m + 8);
            const uint64_t from = g.api.ident_get(m + kMsgIdentityOff);
            if (cb >= 1 && data) {
                const uint8_t type = *(const uint8_t*)data;
                Link* l = find_link(from);
                if (type == kData) {
                    if (!l && g.mode == Mode::Hosting) {
                        g.links.push_back(Link{});
                        l = &g.links.back();
                        l->peer = from;
                        if (!open_host_side(*l)) { l->dead = true; l = nullptr; }
                        else log_line("STEAM", "player %llu connected over Steam", (unsigned long long)from);
                    }
                    if (l) l->to_sock.append((const char*)data + 1, (size_t)cb - 1);
                } else if (type == kClose && l) {
                    log_line("STEAM", "player %llu closed the Steam link", (unsigned long long)from);
                    close_link(*l, false);
                }
            }
            g.api.msg_release(msgs[i]);
        }
        if (n < 32) break;
    }

    for (auto& l : g.links) {
        if (l.dead || l.sock == INVALID_SOCKET) continue;
        if (l.connecting) {
            fd_set w; FD_ZERO(&w); FD_SET(l.sock, &w);
            fd_set e; FD_ZERO(&e); FD_SET(l.sock, &e);
            timeval tv{0, 0};
            if (select(0, nullptr, &w, &e, &tv) > 0) {
                if (FD_ISSET(l.sock, &e)) { log_line("STEAM", "!! could not reach our own host socket"); close_link(l, true); continue; }
                if (FD_ISSET(l.sock, &w)) l.connecting = false;
            }
            if (l.connecting) continue;
        }
        // socket <- buffered Steam bytes
        while (!l.to_sock.empty()) {
            const int w = send(l.sock, l.to_sock.data(), (int)std::min<size_t>(l.to_sock.size(), kChunk), 0);
            if (w > 0) { l.to_sock.erase(0, (size_t)w); continue; }
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            close_link(l, true);
            break;
        }
        if (l.dead) continue;
        // socket -> Steam
        static char buf[kChunk];
        for (int round = 0; round < 16; ++round) {
            const int r = recv(l.sock, buf, (int)sizeof(buf), 0);
            if (r > 0) { steam_send(l.peer, kData, buf, (uint32_t)r); continue; }
            if (r == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                log_line("STEAM", "local side of the Steam link closed");
                close_link(l, true);
            }
            break;
        }
    }
    // Drop dead links.
    for (size_t i = 0; i < g.links.size();) {
        if (g.links[i].dead) g.links.erase(g.links.begin() + i); else ++i;
    }
}

// Guest: accept mgmp's own connection to the bridge and pair it with the host.
void pump_guest_listener() {
    if (g.listener == INVALID_SOCKET) return;
    SOCKET c = accept(g.listener, nullptr, nullptr);
    if (c == INVALID_SOCKET) return;
    set_nonblocking(c);
    BOOL nodelay = TRUE;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    if (Link* old = find_link(g.host_id)) close_link(*old, true);
    Link l{};
    l.peer = g.host_id;
    l.sock = c;
    g.links.push_back(std::move(l));
    log_line("STEAM", "bridging our connection to the host over Steam");
}

bool open_guest_listener() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;   // any free port
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 2) != 0) { closesocket(s); return false; }
    int len = sizeof(a);
    getsockname(s, (sockaddr*)&a, &len);
    set_nonblocking(s);
    g.listener   = s;
    g.guest_port = ntohs(a.sin_port);
    return true;
}

void join_lobby(uint64_t lobby) {
    if (!g.ok || !lobby) return;
    if (g.mode != Mode::Off) steam_leave();
    if (net_active()) session_request_disconnect();
    g.lobby = lobby;
    g.call  = g.api.join_lobby(g.api.matchmaking, lobby);
    g.mode  = Mode::GuestJoining;
    set_status("joining a friend's game over Steam...");
}

void on_lobby_join_requested(const LobbyJoinReq& r) {
    log_line("STEAM", "accepted an invite from %llu", (unsigned long long)r.friend_id);
    g.pending_join = r.lobby;
}

void on_rich_join_requested(const RichJoinReq& r) {
    const char* p = strstr(r.connect, "+connect_lobby");
    if (!p) return;
    g.pending_join = _strtoui64(p + strlen("+connect_lobby"), nullptr, 10);
    log_line("STEAM", "joining %llu's game from the friends list", (unsigned long long)r.friend_id);
}

void on_session_request(const void* identity) {
    // Accept only while hosting: the bridge pairs each remote with mgmp's host.
    if (g.mode != Mode::Hosting) return;
    g.api.accept(g.api.messages, identity);
}

void pump_calls() {
    if (!g.call) return;
    bool failed = false;
    if (!g.api.call_done(g.api.utils, g.call, &failed)) return;
    const uint64_t call = g.call;
    g.call = 0;

    if (g.mode == Mode::HostCreating) {
        LobbyCreated r{};
        if (failed || !g.api.call_result(g.api.utils, call, &r, sizeof(r), kCb_LobbyCreated, &failed) ||
            failed || r.result != 1) {
            set_status("could not create a Steam lobby (result %d) -- LAN still works", r.result);
            g.mode = Mode::Off;
            return;
        }
        g.lobby = r.lobby;
        g.mode  = Mode::Hosting;
        g.api.set_lobby_data(g.api.matchmaking, g.lobby, "mgmp_proto", std::to_string(kProtoVersion).c_str());
        char connect[64];
        _snprintf_s(connect, _TRUNCATE, "+connect_lobby %llu", (unsigned long long)g.lobby);
        g.api.set_rich(g.api.friends, "connect", connect);
        set_status("hosting on Steam -- invite a friend, or they can Join Game from the friends list");
    } else if (g.mode == Mode::GuestJoining) {
        LobbyEnter r{};
        if (failed || !g.api.call_result(g.api.utils, call, &r, sizeof(r), kCb_LobbyEnter, &failed) ||
            failed || r.response != 1) {
            set_status("could not join that Steam game (response %u)", r.response);
            g.mode = Mode::Off;
            return;
        }
        g.host_id = g.api.lobby_owner(g.api.matchmaking, g.lobby);
        if (!g.host_id || g.host_id == g.self || !open_guest_listener()) {
            set_status("joined the lobby but could not set up the Steam link");
            steam_leave();
            return;
        }
        g.mode = Mode::Guest;
        set_status("connected to the host's lobby -- joining their game");
        session_request_join("127.0.0.1", g.guest_port);
    }
}

uint64_t launch_lobby_arg() {
    const wchar_t* cl = GetCommandLineW();
    const wchar_t* p  = cl ? wcsstr(cl, L"+connect_lobby") : nullptr;
    if (!p) return 0;
    return _wcstoui64(p + wcslen(L"+connect_lobby"), nullptr, 10);
}

} // namespace

void steam_init() {
    if (g.tried) return;
    g.tried = true;
    HMODULE m = GetModuleHandleW(L"steam_api64.dll");
    if (!m) { log_line("STEAM", "steam_api64.dll is not loaded -- Steam join is off"); return; }

    Api& a = g.api;
    fn_iface mm = nullptr, fr = nullptr, us = nullptr, ut = nullptr, ms = nullptr;
    bool ok = resolve(m, "SteamAPI_SteamMatchmaking_v009", mm) &&
              resolve(m, "SteamAPI_SteamFriends_v018", fr) &&
              resolve(m, "SteamAPI_SteamUser_v023", us) &&
              resolve(m, "SteamAPI_SteamUtils_v010", ut) &&
              resolve(m, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002", ms) &&
              resolve(m, "SteamAPI_ISteamMatchmaking_CreateLobby", a.create_lobby) &&
              resolve(m, "SteamAPI_ISteamMatchmaking_JoinLobby", a.join_lobby) &&
              resolve(m, "SteamAPI_ISteamMatchmaking_LeaveLobby", a.leave_lobby) &&
              resolve(m, "SteamAPI_ISteamMatchmaking_GetLobbyOwner", a.lobby_owner) &&
              resolve(m, "SteamAPI_ISteamMatchmaking_SetLobbyData", a.set_lobby_data) &&
              resolve(m, "SteamAPI_ISteamFriends_SetRichPresence", a.set_rich) &&
              resolve(m, "SteamAPI_ISteamFriends_ClearRichPresence", a.clear_rich) &&
              resolve(m, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog", a.invite_dialog) &&
              resolve(m, "SteamAPI_ISteamUser_GetSteamID", a.get_steamid) &&
              resolve(m, "SteamAPI_ISteamUtils_IsAPICallCompleted", a.call_done) &&
              resolve(m, "SteamAPI_ISteamUtils_GetAPICallResult", a.call_result) &&
              resolve(m, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser", a.send_msg) &&
              resolve(m, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel", a.recv_msgs) &&
              resolve(m, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser", a.accept) &&
              resolve(m, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser", a.close_session) &&
              resolve(m, "SteamAPI_SteamNetworkingMessage_t_Release", a.msg_release) &&
              resolve(m, "SteamAPI_SteamNetworkingIdentity_SetSteamID64", a.ident_set) &&
              resolve(m, "SteamAPI_SteamNetworkingIdentity_GetSteamID64", a.ident_get) &&
              resolve(m, "SteamAPI_RegisterCallback", a.register_cb);
    if (!ok) { log_line("STEAM", "Steam join is off (missing functions above)"); return; }

    a.matchmaking = mm(); a.friends = fr(); a.user = us(); a.utils = ut(); a.messages = ms();
    if (!a.matchmaking || !a.friends || !a.user || !a.utils || !a.messages) {
        log_line("STEAM", "Steam is not running for this game -- Steam join is off");
        return;
    }
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    g.self = a.get_steamid(a.user);
    a.register_cb(&g.cb_lobby,   kCb_GameLobbyJoinRequested);
    a.register_cb(&g.cb_rich,    kCb_RichPresenceJoin);
    a.register_cb(&g.cb_session, kCb_SessionRequest);
    g.ok = true;
    g.pending_join = launch_lobby_arg();
    log_line("STEAM", "ready (Steam id %llu)%s", (unsigned long long)g.self,
             g.pending_join ? " -- launched from an invite, will join it" : "");
}

void steam_pump() {
    // Initialised here, on the game's main thread, so SteamAPI_RegisterCallback
    // never races the game's own SteamAPI_RunCallbacks.
    if (!g.tried) steam_init();
    if (!g.ok) return;
    // An invite accepted from anywhere: join once the game is up enough to
    // act on it (the session code handles the save screen from there).
    if (g.pending_join && g.mode != Mode::GuestJoining && !g.call) {
        const uint64_t lobby = g.pending_join;
        g.pending_join = 0;
        join_lobby(lobby);
    }
    pump_calls();
    if (g.mode == Mode::Hosting || g.mode == Mode::Guest) {
        pump_guest_listener();
        pump_links();
    }
    // The session ended (Leave, a failure): drop the lobby with it.
    if ((g.mode == Mode::Hosting || g.mode == Mode::Guest) && !net_active() &&
        !session_request_pending()) {
        static uint32_t idle = 0;
        if (++idle > 120) { idle = 0; steam_leave(); }
    }
}

bool steam_available() { return g.ok; }
const char* steam_status() { return g.mode == Mode::Off ? "" : g.status; }
bool steam_hosting() { return g.mode == Mode::Hosting; }
bool steam_in_session() { return g.mode == Mode::Hosting || g.mode == Mode::Guest; }

void steam_host(uint16_t port) {
    if (!g.ok) return;
    if (g.mode != Mode::Off) steam_leave();
    g.host_port = port;
    session_request_host(port);
    g.call = g.api.create_lobby(g.api.matchmaking, kLobbyFriends, kMaxPeers);
    g.mode = Mode::HostCreating;
    set_status("creating a Steam lobby...");
}

void steam_invite() {
    if (g.ok && g.mode == Mode::Hosting && g.lobby)
        g.api.invite_dialog(g.api.friends, g.lobby);
}

void steam_leave() {
    if (!g.ok) return;
    for (auto& l : g.links) {
        uint8_t ident[kIdentityBytes];
        identity_for(l.peer, ident);
        close_link(l, true);
        g.api.close_session(g.api.messages, ident);
    }
    g.links.clear();
    if (g.listener != INVALID_SOCKET) { closesocket(g.listener); g.listener = INVALID_SOCKET; }
    if (g.lobby) g.api.leave_lobby(g.api.matchmaking, g.lobby);
    if (g.mode == Mode::Hosting || g.mode == Mode::HostCreating) g.api.clear_rich(g.api.friends);
    g.lobby = 0;
    g.call  = 0;
    g.mode  = Mode::Off;
    g.status[0] = 0;
    log_line("STEAM", "left the Steam lobby");
}

} // namespace mgmp
