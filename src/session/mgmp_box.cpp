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
