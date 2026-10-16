// mgmp_box.h -- the Adventure box, shared (our fork).
//
// In the House the team for the next run is the four cats placed in Butch's
// box (glaiel::ButchBox, a CatPlacementArea; vtable RVA 0xEFCCC0). Both players
// may add and remove cats; the HOST's box is the truth.
//
//   * A local place/remove happens immediately on that player's screen.
//   * A guest's change is sent to the host (MSG_BOX op add/remove), which
//     applies it to its own box through the game's own functions -- so the
//     game's rules about which cats may go decide, on the host.
//   * After every change, and every 2 s as a desync check, the host sends its
//     four slots (MSG_BOX op state). A guest whose box differs is corrected to
//     the host's, slot order included: the order is the team order, and the
//     team order is how battle control is split between the players.
//
// Game functions (pinned build, bytes checked before hooking):
//   0xAE9D0  ButchBox slot 11, per-frame update (gives us the box pointer)
//   0xAED00  ButchBox slot 31, place(box, HouseCat*) -> bool; picks a random free slot
//   0xAF960  ButchBox slot 32, remove(box, HouseCat*) -> bool
//   box+0x100 -> HouseCat*[4] (the slots); HouseCat+0x80 = the cat's u64 id
//   House cats: [[box+0x18]+8] = scene; [[scene+0x20]+0x4480] = component list
//   for type id 0x448 (HouseCat) = {.., u32 count @0xC, HouseCat** @0x10}
#pragma once

#include <cstdint>

namespace mgmp {

struct BoxMsg;

void box_install(uintptr_t base);
void box_pump();                       // once per frame (session_update)
void box_on_message(uint8_t from, const BoxMsg& m);

} // namespace mgmp
