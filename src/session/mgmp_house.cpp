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

constexpr uintptr_t kButton_Name = 504;   // std::string, see CLAUDE.md "The main menu"

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

    // Button names seen, logged once each (discovery for the blocklist).
    char     seen[96][48] = {};
    uint32_t seen_count = 0;
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

void note_button(const char* name) {
    for (uint32_t i = 0; i < g.seen_count; ++i)
        if (strcmp(g.seen[i], name) == 0) return;
    if (g.seen_count < 96) strncpy_s(g.seen[g.seen_count++], name, _TRUNCATE);
    log_line_lvl(LogLevel::Trace, "HOUSE", "button pressed: '%s'", name);
}

void __fastcall h_click(void* button, bool force) {
    char name[48] = {};
    if (button && mem_read_std_string((const uint8_t*)button + kButton_Name, name, sizeof(name))) {
        note_button(name);
        if (is_guest() && leave_scene_live("House")) {
            for (const char* p : kGuestBlockedButtons) {
                if (strncmp(name, p, strlen(p)) == 0) {
                    set_toast("The guest can only look around -- the host does the buying and building.");
                    return;
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
    if (g.menu_open) {
        overlay_local_pointer(out.nx, out.ny, out.mode);
    } else {
        out.nx = -1.0f;   // "hidden" -- see overlay_on_message
        out.ny = -1.0f;
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
    hook(function_naming(base, "EndDayTransition"), (void*)&h_end_day, (void**)&g.o_end_day,
         "the End Day transition");
    hook(function_naming(base, "DepartTransition"), (void*)&h_depart, (void**)&g.o_depart,
         "the Depart transition");
    hook(addr_of_call(C_ButtonClick), (void*)&h_click, (void**)&g.o_click, "Button::Click");
}

void house_pump() {
    if (++g.frame % 6 == 0) refresh_menu_state();
    if (!net_active()) return;
    pump_cursor();
    if (is_guest()) pump_rejoin();
}

void house_note_battle_cursor() { g.last_battle_cursor = GetTickCount64(); }

const char* house_toast() { return GetTickCount64() < g.toast_until ? g.toast : ""; }

bool house_menu_open() { return g.menu_open; }

} // namespace mgmp
