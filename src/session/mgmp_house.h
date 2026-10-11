// mgmp_house.h -- co-op rules for the House (our fork; not in upstream mgmp).
//
// The House is NOT replicated: the guest plays on a copy of the host's save
// (mgmp_coop.sav). So the rules are about keeping that copy honest:
//
//   * The guest may LOOK -- open storage, pick up and inspect cats. Anything
//     that would move the guest's copy on its own (End Day, departing on an
//     adventure, buying in a house shop, furniture editing) is blocked.
//   * When the host ends the day, the host re-sends its save once the game has
//     written the new day, and the guest reloads into it (mgmp_savefile's
//     handoff). The guest's game is driven back through the menu for it: an
//     Escape key press opens the pause menu, mgmp_leave presses Quit To Menu,
//     and the auto-Play loads the new save.
//   * Each player sees the other's mouse outside battle only while that player
//     has a menu open (storage, a shop, ...). Otherwise it is hidden.
//
// The two transitions are found at runtime by the string literal each one's
// AddComponent<T> instantiation names ("EndDayTransition", "DepartTransition"):
// the single function referencing it is the creator, and returning null from it
// is the game's own "scene is going away" path (it checks Scene+0x4B0 first and
// returns 0 itself).
#pragma once

#include <cstdint>

namespace mgmp {

// After hooks_install (MinHook initialised). Non-fatal: anything that does not
// resolve is logged and that rule is simply off.
void house_install(uintptr_t base);

// Once per frame from session_update.
void house_pump();

// Called by mgmp_cursor whenever the battle HUD ticks, so the House cursor path
// stays quiet while the battle path is sending.
void house_note_battle_cursor();

// One line for the UI ("" when nothing to say): why a guest click was refused.
const char* house_toast();

// Whether this peer has a menu open right now (any scene beyond the always-
// loaded set). Exposed for the debug panel.
bool house_menu_open();

} // namespace mgmp
