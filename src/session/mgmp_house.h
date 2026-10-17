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
//   * Each player sees the other's mouse in the House only while that player
//     has NO menu open (storage, a shop, ...), and in battles. Hidden elsewhere.
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

// From the swap takeover, once per present (flicker diagnosis).
void house_note_swap();

// From the Button::update hook, after the original: where a guest that must
// follow the host actually starts the quit-to-menu transition.
void house_on_button_update(void* button);

// Guest: the host ended the day (MSG_ENDDAY). Press our own End Day sign and
// seed the night from `seed`.
void house_on_end_day_message(uint64_t seed);

// Guest: an End Day from the host is waiting to be started (from mgmp_box's
// House click-manager update, i.e. inside the scene update).
bool house_end_day_pending();
// Starts the guest's End Day: the same AddComponent<EndDayTransition> the sign
// uses (0x1F77F0), on the House component's scene, with the House as the
// captured component the completion reads (+0x20 scene, +0x28 director).
void house_do_end_day(void* house);

// One line for the UI ("" when nothing to say): why a guest click was refused.
const char* house_toast();

// Whether this peer has a menu open right now (any scene beyond the always-
// loaded set). Exposed for the debug panel.
bool house_menu_open();

// A House drawer is open here, so this screen is slid sideways: peer pointers
// (screen fractions from an unshifted view) would land wrong, so none are drawn.
bool house_local_view_shifted();

} // namespace mgmp
