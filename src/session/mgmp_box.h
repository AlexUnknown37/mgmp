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
struct GrabMsg;

void box_install(uintptr_t base);
void box_pump();                       // once per frame (session_update)
void box_on_message(uint8_t from, const BoxMsg& m);

// Carried cats (MSG_GRAB). The House click manager (glaiel::HouseCatClickManager,
// update = 0x1FE940) keeps the carried HouseCat at +0x38 and moves its transform
// ([cat+0x40] +0x80/+0x88) to the mouse every frame. The other game shows the
// same cat held at the same world position, and on release replays the drop the
// way 0x1FF140 does: House = 0xECC30(comp), area = 0x1E6460(House, &point),
// 0x2E88D0(area, cat) -- so a cat dropped into the box lands in the box on both.
// Picking up takes the cat out of its area first: 0x2E89C0([cat+0xE8], cat).
void box_on_grab(uint8_t from, const GrabMsg& m);

} // namespace mgmp
