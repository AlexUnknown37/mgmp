// mgmp_house.cpp -- see mgmp_house.h.

#include "mgmp_house.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_session.h"
#include "mgmp_savefile.h"
#include "mgmp_leave.h"
#include "mgmp_overlay.h"
#include "mgmp_ui.h"
#include "mgmp_resolve.h"

#include "MinHook.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

typedef void* (__fastcall* fn_add_transition)(void* scene, void* a2, void* a3, void* a4);
typedef void  (__fastcall* fn_button_click)(void* button, bool force);

// Scenes that are loaded all the time (or are the screen itself) and so say
// nothing about whether a MENU is open. Anything else live = a menu is up.
const char* const kBaseScenes[] = {
    "Shared", "Base", "House", "Tutorial", "Cutscene", "PauseMenu", "ToolTip",
    "Transition", "PostProcess", "DebugOverlay", "MainMenu", "SaveSelectionScreen",
    "TransitionScene",
};

// Buttons a guest may not press while in the House. Prefix match on the
// button's own name (Button+504). Storage, cat inspection and the pause menu
// are not here, so they keep working.
const char* const kGuestBlockedButtons[] = {
    "Shop_Buy", "Furniture", "Button_HouseBossResummon",
};

constexpr uintptr_t kButton_Name     = 504;    // std::string, see CLAUDE.md "The main menu"
// Button::Click (RVA 0x97E8E0) calls the button's callback as
// `mov rcx,[button+0xF0]; call [[rcx]+0x10]` -- a std::function-style impl whose
// vtable slot 2 is the lambda body. That body's address says WHAT the button
// does, which the name ("HouseButton", "GenericButton") does not.
constexpr uintptr_t kButton_Callback = 0xF0;

// What a guest may not do in the House, by callback body RVA. Pinned to build
// 1.1.21239 (exe SHA256 4127cd6a...): only applied when the resolver reports the
// pinned build, so an update turns the rule off instead of blocking at random.
struct BlockedAction { uint32_t rva; const char* why; };
const BlockedAction kGuestBlockedActions[] = {
    // Entering an NPC office (a store) from the Boon County map: the lambda
    // gets-or-creates the "NPCOffice" scene and adds the office component.
    { 0x00280270, "Only the host can visit the stores." },
};

struct House {
    bool installed = false;

    fn_add_transition o_end_day = nullptr;
    fn_add_transition o_depart  = nullptr;
    fn_button_click   o_click   = nullptr;

    // Toast for the guest UI.
    char     toast[128] = {};
    uint64_t toast_until = 0;

    // Menu detection, refreshed every few frames.
    bool     menu_open = false;
    bool     in_house  = false;
    uint32_t frame = 0;
    uint64_t scene_sig = 0;

    // House cursor sending.
    uint64_t last_battle_cursor = 0;
    uint64_t last_send = 0;
    bool     sent_any  = false;
    CursorMsg last{};

    // Auto-Escape for a guest that must go back through the menu.
    uint32_t esc_presses = 0;
    uint64_t esc_next_at = 0;
    bool     esc_down    = false;
    uint64_t esc_up_at   = 0;

    // Button name + action seen, logged once each (discovery for the blocklist).
    char     seen[96][64] = {};
    uint32_t seen_count = 0;

    uintptr_t base = 0;

    // Flicker diagnosis: swaps per game frame.
    uint32_t swaps = 0, frames = 0;
    uint64_t rate_since = 0;
    uint32_t rate_reports = 0;
};

House g;

bool is_guest() { return net_active() && net_role() == NetRole::Client; }
bool is_host()  { return net_active() && net_role() == NetRole::Host; }

void set_toast(const char* text) {
    strncpy_s(g.toast, text, _TRUNCATE);
    g.toast_until = GetTickCount64() + 4000;
    log_line("HOUSE", "%s", text);
}

// --- finding a function by the string literal it names -----------------------

bool section_range(uintptr_t base, const char* name, uintptr_t& lo, uintptr_t& hi) {
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (strncmp((const char*)sec->Name, name, 8) == 0) {
            lo = base + sec->VirtualAddress;
            hi = lo + sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// The one function whose code takes the address of `literal` (a NUL-terminated
// string in .rdata). 0 when the literal is missing or more than one function
// references it -- ambiguity is a refusal, as in mgmp's signature resolver.
uintptr_t function_naming(uintptr_t base, const char* literal) {
    uintptr_t tlo, thi, rlo, rhi;
    if (!section_range(base, ".text", tlo, thi) || !section_range(base, ".rdata", rlo, rhi))
        return 0;
    const size_t len = strlen(literal) + 1;   // include the terminator
    uintptr_t lit = 0;
    for (uintptr_t p = rlo + 1; p + len <= rhi; ++p) {
        if (*(const char*)p == literal[0] && *(const char*)(p - 1) == 0 &&
            memcmp((const void*)p, literal, len) == 0) {
            lit = p;
            break;
        }
    }
    if (!lit) return 0;

    uintptr_t found = 0;
    for (uintptr_t p = tlo; p + 4 <= thi; ++p) {
        const int32_t disp = *(const int32_t*)p;
        if (p + 4 + (intptr_t)disp != lit) continue;
        DWORD64 image = 0;
        auto* rf = RtlLookupFunctionEntry((DWORD64)p, &image, nullptr);
        if (!rf) continue;
        const uintptr_t start = (uintptr_t)image + rf->BeginAddress;
        if (found && found != start) return 0;   // two functions name it
        found = start;
    }
    return found;
}

// --- detours -------------------------------------------------------------------

void* __fastcall h_end_day(void* scene, void* a2, void* a3, void* a4) {
    if (is_guest()) {
        set_toast("Only the host can end the day -- your day ends when theirs does.");
        return nullptr;   // the game's own "scene is going away" result
    }
    void* r = g.o_end_day(scene, a2, a3, a4);
    if (r && is_host()) savefile_on_end_day();
    return r;
}

void* __fastcall h_depart(void* scene, void* a2, void* a3, void* a4) {
    if (is_guest()) {
        set_toast("Only the host can start an adventure -- you will follow them.");
        return nullptr;
    }
    return g.o_depart(scene, a2, a3, a4);
}

// RVA of the lambda body a button runs when clicked, 0 if unreadable.
uint32_t button_action(const void* button) {
    const void* impl = nullptr;
    const void* vt   = nullptr;
    const void* body = nullptr;
    if (!mem_read((const uint8_t*)button + kButton_Callback, &impl, sizeof(impl)) || !impl) return 0;
    if (!mem_read(impl, &vt, sizeof(vt)) || !vt) return 0;
    if (!mem_read((const uint8_t*)vt + 0x10, &body, sizeof(body)) || !body) return 0;
    const uintptr_t b = (uintptr_t)body;
    return (g.base && b > g.base && b - g.base < 0x2000000) ? (uint32_t)(b - g.base) : 0;
}

void note_button(const char* name, uint32_t action, bool house) {
    char key[64];
    _snprintf_s(key, sizeof(key), _TRUNCATE, "%s@%08X", name, action);
    for (uint32_t i = 0; i < g.seen_count; ++i)
        if (strcmp(g.seen[i], key) == 0) return;
    if (g.seen_count < 96) strncpy_s(g.seen[g.seen_count++], key, _TRUNCATE);
    // Info while in the House: these lines are how the guest blocklist is built.
    log_line_lvl(house ? LogLevel::Info : LogLevel::Trace, "HOUSE",
                 "button pressed: '%s' action %08X", name, action);
}

void __fastcall h_click(void* button, bool force) {
    char name[48] = {};
    if (button && mem_read_std_string((const uint8_t*)button + kButton_Name, name, sizeof(name))) {
        const bool house  = leave_scene_live("House");
        const uint32_t act = button_action(button);
        note_button(name, act, house);
        if (is_guest() && house) {
            for (const char* p : kGuestBlockedButtons) {
                if (strncmp(name, p, strlen(p)) == 0) {
                    set_toast("The guest can only look around -- the host does the buying and building.");
                    return;
                }
            }
            if (act && resolve_is_pinned_build()) {
                for (const BlockedAction& b : kGuestBlockedActions) {
                    if (b.rva == act) { set_toast(b.why); return; }
                }
            }
        }
    }
    g.o_click(button, force);
}

bool hook(uintptr_t target, void* detour, void** original, const char* what) {
    if (!target) {
        log_line_lvl(LogLevel::Warn, "HOUSE", "!! %s not found -- that guest rule is OFF", what);
        return false;
    }
    if (MH_CreateHook((void*)target, detour, original) != MH_OK ||
        MH_EnableHook((void*)target) != MH_OK) {
        log_line_lvl(LogLevel::Warn, "HOUSE", "!! could not hook %s at %p -- that guest rule is OFF",
                     what, (void*)target);
        return false;
    }
    log_line_lvl(LogLevel::Trace, "HOUSE", "hooked %s at %p", what, (void*)target);
    return true;
}

// --- menus and the cursor -----------------------------------------------------

void refresh_menu_state() {
    char names[64][40];
    const uint32_t n = leave_live_scenes(names, 64);
    bool menu = false;
    uint64_t sig = 1469598103934665603ULL;
    for (uint32_t i = 0; i < n; ++i) {
        for (const char* c = names[i]; *c; ++c) { sig ^= (uint8_t)*c; sig *= 1099511628211ULL; }
        sig ^= 0xFF; sig *= 1099511628211ULL;
        bool base = false;
        for (const char* b : kBaseScenes) if (strcmp(names[i], b) == 0) { base = true; break; }
        if (!base) menu = true;
    }
    g.menu_open = menu;
    if (n && sig != g.scene_sig) {
        g.scene_sig = sig;
        char line[512] = {};
        int off = 0;
        for (uint32_t i = 0; i < n && off < 470; ++i)
            off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "%s%s", i ? ", " : "", names[i]);
        log_line_lvl(LogLevel::Trace, "HOUSE", "scenes: %s%s", line, menu ? "  [menu open]" : "");
    }
}

void pump_cursor() {
    if (!session_ready()) return;
    const uint64_t now = GetTickCount64();
    if (now - g.last_battle_cursor < 500) return;   // the battle HUD path is sending

    CursorMsg out{};
    out.battle_id = 0;
    out.on_board  = 0;
    out.owns_turn = 1;
    // Shown only while this player is in the House with no menu open (storage,
    // a shop, ...). Battles use the HUD path above; everywhere else is hidden.
    g.in_house = (g.frame % 6 == 0) ? leave_scene_live("House") : g.in_house;
    if (g.in_house && !g.menu_open) {
        overlay_local_pointer(out.nx, out.ny, out.mode);
    } else {
        // "Hidden" -- see overlay_on_message. The position stays inside the
        // decoder's bounds (dec_cursor closes the connection on a wild
        // fraction), so the marker is the art index instead.
        out.nx = 0.0f;
        out.ny = 0.0f;
        out.mode = kCursorHiddenMode;
    }
    const bool changed = !g.sent_any || out.nx != g.last.nx || out.ny != g.last.ny ||
                         out.mode != g.last.mode;
    const uint64_t since = now - g.last_send;
    if ((changed && since >= 50) || since >= 500) {
        if (net_send_cursor(out)) {
            g.last = out;
            g.sent_any = true;
            g.last_send = now;
        }
    }
}

// --- the guest's trip back through the menu -------------------------------------

void post_escape(bool down) {
    HWND hwnd = (HWND)ui_hwnd();
    if (!hwnd) return;
    const LPARAM scancode = (LPARAM)0x01 << 16;
    if (down) PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 1 | scancode);
    else      PostMessageW(hwnd, WM_KEYUP, VK_ESCAPE, 1 | scancode | (1LL << 30) | (1LL << 31));
}

void pump_rejoin() {
    const uint64_t now = GetTickCount64();
    if (g.esc_down && now >= g.esc_up_at) { post_escape(false); g.esc_down = false; }

    if (!savefile_must_rejoin()) { g.esc_presses = 0; g.esc_next_at = 0; return; }
    if (leave_scene_live("MainMenu") || leave_scene_live("SaveSelectionScreen")) return;
    if (g.esc_presses >= 3 || now < g.esc_next_at || g.esc_down) return;

    // One Escape opens the pause menu; mgmp_leave (armed by savefile) then
    // presses Quit To Menu, and the auto-Play loads the host's new save. Spaced
    // out so a second press never lands on the pause menu we just opened.
    post_escape(true);
    g.esc_down    = true;
    g.esc_up_at   = now + 80;
    g.esc_next_at = now + 6000;
    ++g.esc_presses;
    log_line("HOUSE", "pressing Escape to go back through the menu and follow the host (%u/3)",
             g.esc_presses);
}

} // namespace

void house_install(uintptr_t base) {
    if (g.installed) return;
    g.installed = true;
    g.base = base;
    hook(function_naming(base, "EndDayTransition"), (void*)&h_end_day, (void**)&g.o_end_day,
         "the End Day transition");
    hook(function_naming(base, "DepartTransition"), (void*)&h_depart, (void**)&g.o_depart,
         "the Depart transition");
    hook(addr_of_call(C_ButtonClick), (void*)&h_click, (void**)&g.o_click, "Button::Click");
}

void house_note_swap() { ++g.swaps; }

void house_pump() {
    if (++g.frame % 6 == 0) refresh_menu_state();

    // Flicker diagnosis: a game that presents more than once per frame (or
    // less) explains a panel that blinks. Reported a few times, then quiet.
    ++g.frames;
    const uint64_t now = GetTickCount64();
    if (!g.rate_since) g.rate_since = now;
    if (now - g.rate_since >= 10000 && g.rate_reports < 6) {
        log_line("HOUSE", "frame pacing: %u frames, %u swaps in %.1f s (%.2f swaps per frame)",
                 g.frames, g.swaps, (now - g.rate_since) / 1000.0,
                 g.frames ? (double)g.swaps / g.frames : 0.0);
        g.frames = g.swaps = 0;
        g.rate_since = now;
        ++g.rate_reports;
    }
    if (!net_active()) return;
    pump_cursor();
    if (is_guest()) pump_rejoin();
}

void house_note_battle_cursor() { g.last_battle_cursor = GetTickCount64(); }

const char* house_toast() { return GetTickCount64() < g.toast_until ? g.toast : ""; }

bool house_menu_open() { return g.menu_open; }

} // namespace mgmp
