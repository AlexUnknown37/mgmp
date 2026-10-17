// mgmp_box.cpp -- see mgmp_box.h.

#include "mgmp_box.h"
#include "mgmp_proto.h"
#include "mgmp_net.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_resolve.h"
#include "mgmp_session.h"
#include "mgmp_leave.h"

#include "MinHook.h"

#include <windows.h>
#include <cstring>

namespace mgmp {
namespace {

typedef void (__fastcall* fn_box_update)(void* box);
typedef bool (__fastcall* fn_box_op)(void* box, void* house_cat);
typedef void (__fastcall* fn_cm_update)(void* cm);
typedef void* (__fastcall* fn_get_house)(void* comp);
typedef void* (__fastcall* fn_area_at)(void* house, const double* point);
typedef void  (__fastcall* fn_area_cat)(void* area, void* house_cat);

struct Remote {
    uint64_t cat  = 0;
    double   x = 0, y = 0;
    bool     held = false;
    bool     taken = false;     // removed from its area on our side
    bool     drop_pending = false;
    uint64_t at = 0;
};

constexpr uint32_t  kRva_Update = 0x000AE9D0;
constexpr uint32_t  kRva_Place  = 0x000AED00;
constexpr uint32_t  kRva_Remove = 0x000AF960;
constexpr uintptr_t kBox_Slots  = 0x100;
constexpr uintptr_t kCat_Id     = 0x80;
constexpr int       kSlots      = 4;

struct State {
    bool installed = false;
    uintptr_t base = 0;
    fn_box_update o_update = nullptr;
    fn_box_op     o_place  = nullptr;
    fn_box_op     o_remove = nullptr;

    void*    box = nullptr;          // last box seen by the update hook
    uint64_t box_seen_at = 0;
    bool     applying = false;       // our own calls: do not echo them

    uint64_t last_sent[kSlots] = {};
    uint64_t last_send_at = 0;
    bool     host_dirty = false;     // host: send state at the next pump

    // Carried cats.
    fn_cm_update o_cm_update = nullptr;
    fn_get_house get_house = nullptr;
    fn_area_at   area_at   = nullptr;
    fn_area_cat  area_add  = nullptr;
    fn_area_cat  area_remove = nullptr;
    void*    cm = nullptr;
    void*    my_held = nullptr;      // what our click manager held last frame
    uint64_t my_held_id = 0;
    uint64_t grab_sent_at = 0;
    double   last_x = 0, last_y = 0;
    Remote   remote;
    bool     have_host = false;      // guest: latest host state
    bool     apply_pending = false;  // guest: a host state arrived and is not applied yet
    uint64_t local_change_at = 0;    // guest: our own last place/remove
    uint64_t host_state[kSlots] = {};
    uint32_t corrections = 0;
};

State g;

bool is_guest() { return net_active() && net_role() == NetRole::Client && session_ready(); }
bool is_host()  { return net_active() && net_role() == NetRole::Host && session_ready(); }

bool box_live() { return g.box && GetTickCount64() - g.box_seen_at < 1000; }

void* slot(void* box, int i) {
    const void* arr = nullptr;
    if (!mem_read((const uint8_t*)box + kBox_Slots, &arr, sizeof(arr)) || !arr) return nullptr;
    void* hc = nullptr;
    mem_read((const uint8_t*)arr + i * 8, &hc, sizeof(hc));
    return hc;
}

uint64_t cat_id(const void* hc) {
    uint64_t id = 0;
    if (hc) mem_read((const uint8_t*)hc + kCat_Id, &id, sizeof(id));
    return id;
}

void read_slots(void* box, uint64_t (&ids)[kSlots]) {
    for (int i = 0; i < kSlots; ++i) ids[i] = cat_id(slot(box, i));
}

// The House's HouseCat with this cat id, or null.
void* find_house_cat(void* box, uint64_t id);

// --- carried cats --------------------------------------------------------

constexpr uintptr_t kCM_Held       = 0x38;
constexpr uintptr_t kCat_Transform = 0x40;
constexpr uintptr_t kCat_Area      = 0xE8;
constexpr uintptr_t kCat_HeldArt   = 0x50;   // renderer shown while carried
constexpr uintptr_t kCat_NormalArt = 0x58;   // renderer shown otherwise

bool cat_pos(void* hc, double& x, double& y) {
    const uint8_t* t = nullptr;
    if (!mem_read((const uint8_t*)hc + kCat_Transform, &t, sizeof(t)) || !t) return false;
    return mem_read(t + 0x80, &x, 8) && mem_read(t + 0x88, &y, 8);
}

void set_cat_pos(void* hc, double x, double y) {
    uint8_t* t = nullptr;
    if (!mem_read((const uint8_t*)hc + kCat_Transform, &t, sizeof(t)) || !t) return;
    const double z = 0.0;
    memcpy(t + 0x80, &x, 8);
    memcpy(t + 0x88, &y, 8);
    memcpy(t + 0x90, &z, 8);
}

void set_held_art(void* hc, bool held) {
    for (int k = 0; k < 2; ++k) {
        uint8_t* r = nullptr;
        if (mem_read((const uint8_t*)hc + (k ? kCat_NormalArt : kCat_HeldArt), &r, sizeof(r)) && r)
            r[0x51] = (uint8_t)(k ? !held : held);
    }
}

// The point the game tests for an area on drop: transform position plus the
// cat's scaled drop offset (0x1FF140).
void drop_point(void* hc, double* pt) {
    const uint8_t* t = nullptr;
    const uint8_t* c = nullptr;
    double x = 0, y = 0, sx = 0, sy = 0, ox = 0, oy = 0;
    if (mem_read((const uint8_t*)hc + kCat_Transform, &t, sizeof(t)) && t &&
        mem_read((const uint8_t*)hc + 0x100, &c, sizeof(c)) && c &&
        mem_read(t + 0x80, &x, 8) && mem_read(t + 0x88, &y, 8) &&
        mem_read(t + 0x98, &sx, 8) && mem_read(t + 0xA0, &sy, 8) &&
        mem_read(c + 0xC8, &ox, 8) && mem_read(c + 0xD0, &oy, 8)) {
        pt[0] = x + sx * ox;
        pt[1] = y + sy * oy;
    } else {
        cat_pos(hc, pt[0], pt[1]);
    }
}

void send_grab(uint64_t id, void* hc, bool held) {
    GrabMsg m{};
    m.cat  = id;
    m.held = held ? 1 : 0;
    if (!cat_pos(hc, m.x, m.y)) return;
    if (net_send_grab(m)) { g.grab_sent_at = GetTickCount64(); g.last_x = m.x; g.last_y = m.y; }
}

// Ours: report what our player carries.
void publish_my_grab(void* cm) {
    void* held = nullptr;
    mem_read((const uint8_t*)cm + kCM_Held, &held, sizeof(held));
    if (held != g.my_held) {
        if (g.my_held && g.my_held_id) {
            // Released (or swapped): the cat is where the game just dropped it.
            send_grab(g.my_held_id, g.my_held, false);
            log_line_lvl(LogLevel::Trace, "BOX", "dropped cat %llu", (unsigned long long)g.my_held_id);
        }
        g.my_held    = held;
        g.my_held_id = held ? cat_id(held) : 0;
        if (held) send_grab(g.my_held_id, held, true);
        return;
    }
    if (held && GetTickCount64() - g.grab_sent_at >= 50) {
        double x = 0, y = 0;
        if (cat_pos(held, x, y) && (x != g.last_x || y != g.last_y || GetTickCount64() - g.grab_sent_at >= 500))
            send_grab(g.my_held_id, held, true);
    }
}

// Theirs: show the cat the other player carries, and replay their drop.
void apply_remote_grab(void* cm) {
    Remote& r = g.remote;
    if (!r.cat) return;
    void* hc = find_house_cat(cm, r.cat);
    if (!hc) return;
    void* mine = nullptr;
    mem_read((const uint8_t*)cm + kCM_Held, &mine, sizeof(mine));
    if (mine == hc) { r.held = false; r.drop_pending = false; r.cat = 0; return; }   // we have it now

    if (r.held) {
        if (!r.taken) {
            // Out of whatever it was in (box, room) -- as the game does on pick-up.
            void* area = nullptr;
            mem_read((const uint8_t*)hc + kCat_Area, &area, sizeof(area));
            if (area) {
                g.area_remove(area, hc);
                void* none = nullptr;
                memcpy((uint8_t*)hc + kCat_Area, &none, 8);
            }
            r.taken = true;
        }
        set_cat_pos(hc, r.x, r.y);
        set_held_art(hc, true);
        if (GetTickCount64() - r.at > 3000) { r.held = false; r.drop_pending = true; }   // lost the stream
        return;
    }
    if (r.drop_pending) {
        r.drop_pending = false;
        set_cat_pos(hc, r.x, r.y);
        set_held_art(hc, false);
        double pt[2];
        drop_point(hc, pt);
        if (void* house = g.get_house(cm)) {
            if (void* area = g.area_at(house, pt)) g.area_add(area, hc);
        }
        r.cat = 0;
        r.taken = false;
    }
}

void __fastcall h_cm_update(void* cm) {
    g.o_cm_update(cm);
    g.cm = cm;
    if (!net_active() || !session_ready()) return;
    publish_my_grab(cm);
    apply_remote_grab(cm);
}

void* find_house_cat(void* box, uint64_t id) {
    if (!id) return nullptr;
    const uint8_t* comp  = nullptr;
    const uint8_t* scene = nullptr;
    const uint8_t* table = nullptr;
    const uint8_t* list  = nullptr;
    if (!mem_read((const uint8_t*)box + 0x18, &comp, sizeof(comp)) || !comp) return nullptr;
    if (!mem_read(comp + 0x08, &scene, sizeof(scene)) || !scene) return nullptr;
    if (!mem_read(scene + 0x20, &table, sizeof(table)) || !table) return nullptr;
    if (!mem_read(table + 0x4480, &list, sizeof(list)) || !list) return nullptr;
    uint32_t count = 0;
    void**   data  = nullptr;
    if (!mem_read(list + 0x0C, &count, sizeof(count)) || !mem_read(list + 0x10, &data, sizeof(data)) || !data)
        return nullptr;
    if (count > 4096) return nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        void* hc = nullptr;
        if (mem_read(data + i, &hc, sizeof(hc)) && hc && cat_id(hc) == id) return hc;
    }
    return nullptr;
}

bool contains(const uint64_t (&ids)[kSlots], uint64_t id) {
    for (uint64_t v : ids) if (v == id) return true;
    return false;
}

void send_state(void* box) {
    BoxMsg m{};
    m.op = kBoxState;
    read_slots(box, m.ids);
    if (net_send_box(m)) {
        memcpy(g.last_sent, m.ids, sizeof(m.ids));
        g.last_send_at = GetTickCount64();
    }
}

// Guest: make our box equal to the host's, slot order included.
void apply_host_state(void* box) {
    uint64_t mine[kSlots];
    read_slots(box, mine);
    if (memcmp(mine, g.host_state, sizeof(mine)) == 0) return;

    g.applying = true;
    // Out: cats the host does not have in its box.
    for (int i = 0; i < kSlots; ++i) {
        if (mine[i] && !contains(g.host_state, mine[i])) {
            if (void* hc = slot(box, i)) g.o_remove(box, hc);
        }
    }
    // In: cats the host has that we do not.
    read_slots(box, mine);
    for (int i = 0; i < kSlots; ++i) {
        const uint64_t id = g.host_state[i];
        if (id && !contains(mine, id)) {
            if (void* hc = find_house_cat(box, id)) g.o_place(box, hc);
            else log_line_lvl(LogLevel::Warn, "BOX", "!! the host's cat %llu is not in this House",
                              (unsigned long long)id);
        }
    }
    // Order: the same cats now, so put them in the host's slots.
    read_slots(box, mine);
    bool same_set = true;
    for (int i = 0; i < kSlots; ++i)
        if (g.host_state[i] && !contains(mine, g.host_state[i])) same_set = false;
    if (same_set) {
        const void* arr = nullptr;
        if (mem_read((const uint8_t*)box + kBox_Slots, &arr, sizeof(arr)) && arr) {
            void* want[kSlots] = {};
            for (int i = 0; i < kSlots; ++i) want[i] = find_house_cat(box, g.host_state[i]);
            memcpy((void*)arr, want, sizeof(want));
        }
    }
    g.applying = false;

    read_slots(box, mine);
    const bool ok = memcmp(mine, g.host_state, sizeof(mine)) == 0;
    ++g.corrections;
    log_line_lvl(ok ? LogLevel::Info : LogLevel::Warn, "BOX",
                 "%s the host's box: %llu %llu %llu %llu", ok ? "matched" : "!! could not fully match",
                 (unsigned long long)g.host_state[0], (unsigned long long)g.host_state[1],
                 (unsigned long long)g.host_state[2], (unsigned long long)g.host_state[3]);
}

// --- detours -----------------------------------------------------------------

void __fastcall h_update(void* box) {
    g.box = box;
    g.box_seen_at = GetTickCount64();
    g.o_update(box);
}

bool __fastcall h_place(void* box, void* hc) {
    const bool ok = g.o_place(box, hc);
    if (ok && !g.applying) {
        if (is_host()) g.host_dirty = true;
        else if (is_guest()) {
            BoxMsg m{};
            m.op = kBoxAdd;
            m.ids[0] = cat_id(hc);
            net_send_box(m);
            g.local_change_at = GetTickCount64();
            log_line("BOX", "put cat %llu in the box -- asking the host", (unsigned long long)m.ids[0]);
        }
    }
    return ok;
}

bool __fastcall h_remove(void* box, void* hc) {
    const uint64_t id = cat_id(hc);
    const bool ok = g.o_remove(box, hc);
    if (ok && !g.applying && id) {
        if (is_host()) g.host_dirty = true;
        else if (is_guest()) {
            BoxMsg m{};
            m.op = kBoxRemove;
            m.ids[0] = id;
            net_send_box(m);
            g.local_change_at = GetTickCount64();
            log_line("BOX", "took cat %llu out of the box -- telling the host", (unsigned long long)id);
        }
    }
    return ok;
}

uintptr_t checked(uintptr_t base, uint32_t rva, const uint8_t* want, size_t n) {
    if (!resolve_is_pinned_build()) return 0;
    uint8_t got[16] = {};
    if (!mem_read((const void*)(base + rva), got, n) || memcmp(got, want, n) != 0) return 0;
    return base + rva;
}

bool hook(uintptr_t at, void* detour, void** original, const char* what) {
    if (!at || MH_CreateHook((void*)at, detour, original) != MH_OK || MH_EnableHook((void*)at) != MH_OK) {
        log_line_lvl(LogLevel::Warn, "BOX", "!! could not hook %s -- the shared box is OFF", what);
        return false;
    }
    return true;
}

} // namespace

void box_install(uintptr_t base) {
    if (g.installed) return;
    g.installed = true;
    g.base = base;
    const uint8_t b_update[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18 };
    const uint8_t b_place[]  = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };
    const uint8_t b_remove[] = { 0x48, 0x8B, 0x81, 0x00, 0x01, 0x00, 0x00 };
    const bool ok =
        hook(checked(base, kRva_Update, b_update, sizeof(b_update)), (void*)&h_update, (void**)&g.o_update, "ButchBox::update") &&
        hook(checked(base, kRva_Place,  b_place,  sizeof(b_place)),  (void*)&h_place,  (void**)&g.o_place,  "ButchBox::place") &&
        hook(checked(base, kRva_Remove, b_remove, sizeof(b_remove)), (void*)&h_remove, (void**)&g.o_remove, "ButchBox::remove");
    if (ok) log_line_lvl(LogLevel::Trace, "BOX", "shared Adventure box hooks live");

    // Carried cats: the click manager update, plus the drop/pick-up helpers.
    const uint8_t b_cm[]    = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57 };
    const uint8_t b_house[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    const uint8_t b_at[]    = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18 };
    const uint8_t b_add[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10 };
    const uint8_t b_rem[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    g.get_house   = (fn_get_house)checked(base, 0x0ECC30, b_house, sizeof(b_house));
    g.area_at     = (fn_area_at)checked(base, 0x1E6460, b_at, sizeof(b_at));
    g.area_add    = (fn_area_cat)checked(base, 0x2E88D0, b_add, sizeof(b_add));
    g.area_remove = (fn_area_cat)checked(base, 0x2E89C0, b_rem, sizeof(b_rem));
    if (g.get_house && g.area_at && g.area_add && g.area_remove)
        hook(checked(base, 0x1FE940, b_cm, sizeof(b_cm)), (void*)&h_cm_update, (void**)&g.o_cm_update,
             "HouseCatClickManager::update");
    else
        log_line_lvl(LogLevel::Warn, "BOX", "!! a drop helper did not verify -- carried cats are not shared");
}

void box_pump() {
    if (!g.o_update || !box_live()) return;
    void* box = g.box;
    if (is_host()) {
        uint64_t now_ids[kSlots];
        read_slots(box, now_ids);
        const bool changed = memcmp(now_ids, g.last_sent, sizeof(now_ids)) != 0;
        // On a change, and every 2 s as the desync check.
        if (g.host_dirty || changed || GetTickCount64() - g.last_send_at > 2000) {
            g.host_dirty = false;
            send_state(box);
        }
    } else if (is_guest() && g.have_host && g.apply_pending &&
               GetTickCount64() - g.local_change_at > 1000) {
        // Once per host update, and not right after our own change (the host's
        // periodic state may still predate it; its answer is on the way).
        g.apply_pending = false;
        apply_host_state(box);
    }
}

void box_on_grab(uint8_t from, const GrabMsg& m) {
    (void)from;
    Remote& r = g.remote;
    if (r.cat && r.cat != m.cat && r.held) r.drop_pending = true;   // a new cat: the old one was let go
    r.cat  = m.cat;
    r.x    = m.x;
    r.y    = m.y;
    r.at   = GetTickCount64();
    if (m.held) { if (!r.held) r.taken = false; r.held = true; }
    else        { r.held = false; r.drop_pending = true; }
}

void box_on_message(uint8_t from, const BoxMsg& m) {
    (void)from;
    if (m.op == kBoxState) {
        if (!is_guest()) return;
        memcpy(g.host_state, m.ids, sizeof(g.host_state));
        g.have_host = true;
        g.apply_pending = true;
        return;
    }
    // A guest's change, applied on the host through the game's own functions.
    if (!is_host() || !box_live()) return;
    void* box = g.box;
    void* hc  = find_house_cat(box, m.ids[0]);
    if (!hc) {
        log_line_lvl(LogLevel::Warn, "BOX", "!! the guest moved cat %llu, which is not in this House",
                     (unsigned long long)m.ids[0]);
        g.host_dirty = true;   // resend our state so the guest is corrected
        return;
    }
    uint64_t ids[kSlots];
    read_slots(box, ids);
    g.applying = true;
    bool ok = true;
    if (m.op == kBoxAdd && !contains(ids, m.ids[0]))         ok = g.o_place(box, hc);
    else if (m.op == kBoxRemove && contains(ids, m.ids[0]))  ok = g.o_remove(box, hc);
    g.applying = false;
    log_line("BOX", "the guest %s cat %llu%s", m.op == kBoxAdd ? "boxed" : "unboxed",
             (unsigned long long)m.ids[0], ok ? "" : " -- the game refused it");
    g.host_dirty = true;
}

} // namespace mgmp
