#include "mgmp_lan.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace mgmp {
namespace {

// "MGMPLAN?" asks, "MGMPLAN!" answers. The magic keeps stray traffic on the
// port (anything else that happens to broadcast there) from becoming a list row.
constexpr char     kAsk[8]   = {'M','G','M','P','L','A','N','?'};
constexpr char     kReply[8] = {'M','G','M','P','L','A','N','!'};
constexpr DWORD    kSearchMs = 3000;   // how long a search accepts replies
constexpr DWORD    kResendMs = 1000;   // UDP can drop; ask again within the window

#pragma pack(push, 1)
struct AskPacket {
    char     magic[8];
    uint32_t proto;
};
struct ReplyPacket {
    char     magic[8];
    uint32_t proto;
    uint16_t port;
    uint8_t  players;
    uint8_t  flags;      // reserved
    char     name[32];
};
#pragma pack(pop)

struct Lan {
    bool     wsa = false;
    wchar_t  ini[MAX_PATH] = {};

    char     name[32]      = {};   // saved in mgmp_user.ini
    char     last_addr[64] = {};
    char     steam_name[32] = {};
    char     display[32]   = {};

    // Host side.
    SOCKET   responder      = INVALID_SOCKET;
    uint16_t responder_port = 0;
    bool     responder_warned = false;

    // Searching side.
    SOCKET   search       = INVALID_SOCKET;
    uint16_t search_port  = 0;
    DWORD    search_start = 0;
    DWORD    last_ask     = 0;
    LanHost  found[kLanMaxFound] = {};
    uint32_t found_count  = 0;
};

Lan g;

bool ensure_wsa() {
    if (g.wsa) return true;
    WSADATA d;
    g.wsa = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    return g.wsa;
}

void close_socket(SOCKET& s) {
    if (s != INVALID_SOCKET) closesocket(s);
    s = INVALID_SOCKET;
}

SOCKET open_udp(uint16_t bind_port, bool broadcast) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    if (broadcast) {
        BOOL on = TRUE;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof(on));
    }
    // Windows reports an ICMP "port unreachable" from an earlier send as
    // WSAECONNRESET on the next recvfrom; turn that off so one closed peer does
    // not end the drain loop.
    BOOL no = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &no, sizeof(no),
             nullptr, 0, &ret, nullptr, nullptr);

    sockaddr_in a = {};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port        = htons(bind_port);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

void send_to(SOCKET s, const void* data, int len, uint32_t addr_be, uint16_t port) {
    sockaddr_in to = {};
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = addr_be;
    to.sin_port        = htons(port);
    sendto(s, (const char*)data, len, 0, (sockaddr*)&to, sizeof(to));
}

// Calls fn(address, prefix_length) for every up, non-loopback IPv4 unicast
// address on this PC.
template <class Fn>
void for_each_ipv4(Fn fn) {
    ULONG size = 16 * 1024;
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto* buf = (IP_ADAPTER_ADDRESSES*)HeapAlloc(GetProcessHeap(), 0, size);
        if (!buf) return;
        ULONG r = GetAdaptersAddresses(AF_INET,
                                       GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                       GAA_FLAG_SKIP_DNS_SERVER,
                                       nullptr, buf, &size);
        if (r == ERROR_BUFFER_OVERFLOW) { HeapFree(GetProcessHeap(), 0, buf); continue; }
        if (r == NO_ERROR) {
            for (auto* a = buf; a; a = a->Next) {
                if (a->OperStatus != IfOperStatusUp) continue;
                if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
                for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                    if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                    auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
                    fn(sin->sin_addr.s_addr, u->OnLinkPrefixLength);
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, buf);
        return;
    }
}

bool is_home_lan(uint32_t addr_be) {
    const uint8_t* b = (const uint8_t*)&addr_be;
    return b[0] == 192 && b[1] == 168 || b[0] == 10 ||
           b[0] == 172 && b[1] >= 16 && b[1] <= 31;
}

void send_ask() {
    AskPacket ask = {};
    memcpy(ask.magic, kAsk, sizeof(kAsk));
    ask.proto = kProtoVersion;

    send_to(g.search, &ask, sizeof(ask), htonl(INADDR_BROADCAST), g.search_port);
    send_to(g.search, &ask, sizeof(ask), htonl(INADDR_LOOPBACK), g.search_port);
    // Each adapter's own subnet broadcast (e.g. 192.168.1.255).
    for_each_ipv4([](uint32_t addr_be, uint8_t prefix) {
        if (prefix == 0 || prefix >= 32) return;
        uint32_t host_mask = (prefix >= 32) ? 0 : (0xFFFFFFFFu >> prefix);
        uint32_t bcast = ntohl(addr_be) | host_mask;
        AskPacket a = {};
        memcpy(a.magic, kAsk, sizeof(kAsk));
        a.proto = kProtoVersion;
        send_to(g.search, &a, sizeof(a), htonl(bcast), g.search_port);
    });
    g.last_ask = GetTickCount();
}

void pump_responder() {
    const bool hosting = net_role() == NetRole::Host && net_active();
    // Follow the session: open while hosting, on the session's own port number.
    uint16_t want = hosting ? net_port() : 0;
    if (!hosting || want != g.responder_port) {
        if (g.responder != INVALID_SOCKET)
            log_line("LAN", "stopped answering LAN searches");
        close_socket(g.responder);
        g.responder_port = 0;
    }
    if (hosting && g.responder == INVALID_SOCKET && want) {
        g.responder = open_udp(want, true);
        if (g.responder != INVALID_SOCKET) {
            g.responder_port   = want;
            g.responder_warned = false;
            log_line("LAN", "answering LAN searches on UDP %u as '%s'",
                     (unsigned)want, lan_display_name());
        } else if (!g.responder_warned) {
            g.responder_warned = true;
            log_line_lvl(LogLevel::Warn, "LAN",
                         "could not open UDP %u for LAN searches (error %d) -- players "
                         "can still join by typing this PC's address",
                         (unsigned)want, WSAGetLastError());
        }
    }
    if (g.responder == INVALID_SOCKET) return;

    for (int i = 0; i < 32; ++i) {
        char buf[64];
        sockaddr_in from = {};
        int flen = sizeof(from);
        int n = recvfrom(g.responder, buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
        if (n <= 0) break;
        if (n < (int)sizeof(AskPacket) || memcmp(buf, kAsk, sizeof(kAsk)) != 0) continue;

        ReplyPacket r = {};
        memcpy(r.magic, kReply, sizeof(kReply));
        r.proto   = kProtoVersion;
        r.port    = g.responder_port;
        r.players = net_peer_count() ? net_peer_count() : 1;
        strncpy_s(r.name, lan_display_name(), _TRUNCATE);
        sendto(g.responder, (const char*)&r, sizeof(r), 0, (sockaddr*)&from, flen);
    }
}

void pump_search() {
    if (g.search == INVALID_SOCKET) return;
    const DWORD now = GetTickCount();

    for (int i = 0; i < 64; ++i) {
        char buf[128];
        sockaddr_in from = {};
        int flen = sizeof(from);
        int n = recvfrom(g.search, buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
        if (n <= 0) break;
        if (n < (int)sizeof(ReplyPacket) || memcmp(buf, kReply, sizeof(kReply)) != 0) continue;
        const ReplyPacket* r = (const ReplyPacket*)buf;

        LanHost h = {};
        inet_ntop(AF_INET, &from.sin_addr, h.addr, sizeof(h.addr));
        h.port         = r->port;
        h.players      = r->players;
        h.same_version = r->proto == kProtoVersion;
        memcpy(h.name, r->name, sizeof(h.name));
        h.name[sizeof(h.name) - 1] = 0;

        // The same host answers once per address it was asked on (broadcast,
        // subnet broadcast, loopback). Keep one row per host and port, and
        // prefer a LAN address over 127.0.0.1 so the row reads usefully.
        bool dup = false;
        for (uint32_t k = 0; k < g.found_count; ++k) {
            LanHost& e = g.found[k];
            if (e.port == h.port && strcmp(e.name, h.name) == 0) {
                if (strcmp(e.addr, "127.0.0.1") == 0 && strcmp(h.addr, "127.0.0.1") != 0)
                    strcpy_s(e.addr, h.addr);
                e.players = h.players;
                dup = true;
                break;
            }
        }
        if (!dup && g.found_count < kLanMaxFound) {
            g.found[g.found_count++] = h;
            log_line("LAN", "found '%s' at %s:%u%s", h.name, h.addr, (unsigned)h.port,
                     h.same_version ? "" : "  (different version)");
        }
    }

    if (now - g.search_start >= kSearchMs) {
        log_line("LAN", "search finished: %u game(s) found", g.found_count);
        close_socket(g.search);
        return;
    }
    if (now - g.last_ask >= kResendMs) send_ask();
}

void fetch_steam_name() {
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam) return;
    typedef void* (__cdecl * fn_accessor)();
    typedef const char* (__cdecl * fn_persona)(void*);
    fn_accessor acc = (fn_accessor)GetProcAddress(steam, "SteamAPI_SteamFriends_v018");
    if (!acc) acc = (fn_accessor)GetProcAddress(steam, "SteamAPI_SteamFriends_v017");
    fn_persona persona = (fn_persona)GetProcAddress(steam, "SteamAPI_ISteamFriends_GetPersonaName");
    if (!acc || !persona) return;
    void* friends = acc();
    if (!friends) return;
    const char* n = persona(friends);
    if (n && n[0]) strncpy_s(g.steam_name, n, _TRUNCATE);
}

void refresh_display() {
    if (g.name[0])            strncpy_s(g.display, g.name, _TRUNCATE);
    else if (g.steam_name[0]) strncpy_s(g.display, g.steam_name, _TRUNCATE);
    else {
        char user[64] = {};
        DWORD n = sizeof(user);
        if (!GetUserNameA(user, &n) || !user[0]) strcpy_s(user, "Player");
        strncpy_s(g.display, user, _TRUNCATE);
    }
}

void read_utf8(const wchar_t* key, char* out, int cap) {
    wchar_t w[128] = {};
    GetPrivateProfileStringW(L"player", key, L"", w, 128, g.ini);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr);
    out[cap - 1] = 0;
}

void write_utf8(const wchar_t* key, const char* value) {
    wchar_t w[128] = {};
    MultiByteToWideChar(CP_UTF8, 0, value, -1, w, 128);
    WritePrivateProfileStringW(L"player", key, w, g.ini);
}

} // namespace

void lan_init(const wchar_t* dll_dir) {
    swprintf_s(g.ini, L"%s\\mgmp_user.ini", dll_dir);
    read_utf8(L"name", g.name, sizeof(g.name));
    read_utf8(L"last_addr", g.last_addr, sizeof(g.last_addr));
    fetch_steam_name();
    refresh_display();
}

void lan_pump() {
    if (!ensure_wsa()) return;
    pump_responder();
    pump_search();
}

void lan_search(uint16_t port) {
    if (!ensure_wsa()) return;
    close_socket(g.search);
    g.found_count = 0;
    g.search = open_udp(0, true);
    if (g.search == INVALID_SOCKET) {
        log_line_lvl(LogLevel::Warn, "LAN", "could not open a socket to search (error %d)",
                     WSAGetLastError());
        return;
    }
    g.search_port  = port;
    g.search_start = GetTickCount();
    send_ask();
    log_line("LAN", "searching the network for games on port %u", (unsigned)port);
}

void lan_stop_search() { close_socket(g.search); }

bool lan_searching() { return g.search != INVALID_SOCKET; }

uint32_t lan_found(LanHost* out, uint32_t cap) {
    uint32_t n = g.found_count < cap ? g.found_count : cap;
    memcpy(out, g.found, n * sizeof(LanHost));
    return n;
}

void lan_local_addresses(char* out, uint32_t cap) {
    out[0] = 0;
    char lan[256] = {}, other[256] = {};
    for_each_ipv4([&](uint32_t addr_be, uint8_t) {
        char s[48];
        in_addr a; a.s_addr = addr_be;
        inet_ntop(AF_INET, &a, s, sizeof(s));
        char* dst = is_home_lan(addr_be) ? lan : other;
        if (dst[0]) strcat_s(dst, 256, ", ");
        strcat_s(dst, 256, s);
    });
    _snprintf_s(out, cap, _TRUNCATE, "%s%s%s", lan, (lan[0] && other[0]) ? ", " : "", other);
    if (!out[0]) strncpy_s(out, cap, "127.0.0.1", _TRUNCATE);
}

const char* lan_display_name() { return g.display; }
const char* lan_saved_name()   { return g.name; }
const char* lan_steam_name()   { return g.steam_name; }

void lan_save_name(const char* name) {
    strncpy_s(g.name, name, _TRUNCATE);
    write_utf8(L"name", g.name);
    refresh_display();
}

const char* lan_last_addr() { return g.last_addr; }

void lan_save_last_addr(const char* addr) {
    strncpy_s(g.last_addr, addr, _TRUNCATE);
    write_utf8(L"last_addr", g.last_addr);
}

} // namespace mgmp
