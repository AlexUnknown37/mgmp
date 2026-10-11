# MODLOG — Mewgenics Online (fork of mgmp)

Journal for our fork. Upstream's technical bible is `CLAUDE.md` (what the binary does);
this file records OUR goals, decisions, paths and progress.

## Goal
Steam + LAN co-op for Mewgenics. The host starts their game; the client joins. The client
plays on a dummy save synced from the host. **Client saves are never written, and no save is
ever deleted** (by the mod, on either machine).

## Repos
- Fork: https://github.com/AlexUnknown37/mgmp (`origin`, branch `main`)
- Upstream: https://github.com/SanTertrust/mgmp (`upstream`, MIT). Base commit `bcd9985`
  (tag `v1.0.0-1.1.21239`). Pull its fixes with `git fetch upstream && git merge upstream/main`.
- Keep upstream's LICENSE/copyright; credit SanTertrust in README.

## Paths
- Game: `D:\_Games\Steam\steamapps\common\Mewgenics` (Steam app 686060, build 25143593 =
  game 1.1.21239, Mewgenics.exe SHA256 4127cd6a...77ea — the build mgmp targets)
- Saves: `%APPDATA%\Glaiel Games\Mewgenics\76561198013192683\saves`
  (`steamcampaign01.sav`, `test00.sav`, `backups\*.savbackup`, `steam_autocloud.vdf` = Steam Cloud on)
- Game log: `...\76561198013192683\logs\debug.log`
- Mod manager: Mewtator `D:\_Games\Mods\Mewtator`. DLL loader: Mewjector (version.dll proxy,
  https://github.com/githubuser508/mewjector, Nexus mewgenics/mods/218), not installed yet.
- Build: VS 2026 Community at `B:\Program Files\Microsoft Visual Studio\18\Community`
  (also VS 2022 Pro). CMake is the bundled one:
  `B:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
  `cmake -B build -A x64` then `cmake --build build --config Release`; tests via `ctest --test-dir build --build-config Release`.

## Save backups (restore path)
- 2026-10-11 pre-mod: `C:\Users\alexi\.universal-modder\backups\mewgenics-saves\20261011-141342.zip`
  (`um backup restore`), plus a plain copy at
  `D:\The_Work_Space\MewgenicsOnline\save-backups\Mewgenics_2026-10-11_pre-mod`.

## What upstream already does (v1.0.0)
- Lockstep battles, host-authoritative meta (map/shop/level-up/inventory), reconnect + mid-battle join,
  cursor/aim sharing, ImGui F1 panel, desync halt + dump.
- Client loads the host's run into `mgmp_coop.sav`; its own slots are never written (redirect via
  `MewDirector::init(std::string)`, see CLAUDE.md "The save file").
- Transport: direct TCP port 27600 only. `SteamNetworkingMessages002` linked but unused.
- Loading: `mgmp_loader.exe` launches the game suspended and injects `mgmp.dll` (not Mewjector).
- House (breeding/furniture) is NOT shared — adventure only. Pinned to 1.1.21239.

## Our plan
1. [x] Fork, clone, build baseline (all 9 unit tests pass, 2026-10-11).
2. [~] Load as a Mewjector mod (DLL in `mods/`, managed by Mewtator) instead of the injector.
       Loads and hooks via Mewjector's `mods/` scan (verified 2026-10-11). Mewtator manifest path not yet verified.
3. [ ] Steam transport: host opens a Steam lobby / rich presence; client joins via friends list or invite.
       Transport behind the existing interface; keep TCP for LAN.
4. [~] LAN: keep direct IP, add automatic host discovery on the local network.
       Built 2026-10-11 (not yet tested in game): "Mewgenics Online" window + Find LAN games.
5. [ ] Save hardening: back up all saves before each session; never delete; refuse to write any
       non-`mgmp_coop.sav` file while a client; check whether Steam Cloud uploads `mgmp_coop.sav`.
6. [ ] Packaging for Mewtator + README (credit upstream, Ko-fi link).

## Log
- 2026-10-11: Recon. KB note `games/mewgenics/combat-roster-panel.md` (native C++ engine, SDL3, Mewjector).
  Found mgmp; decided to fork rather than rewrite (user agreed). Baseline build OK with VS 2026.
- 2026-10-11: Testing will be mostly LAN for now (user).
- 2026-10-11: **Mewjector loading.**
  - Mewjector built from source (`third_party_src/mewjector`, HEAD ef8b86c = v3.5) with
    `cl /LD /O2 /GS- version.c /Fe:version.dll /link /DEF:version.def` under VS 2026 vcvars64.
    Installed `version.dll` + `chainloader.ini` + empty `mods/` into the game folder.
  - **Mewjector v3.5 EP fallback crashes this build**: with `EnableEPFallback=` (auto = on) the game dies
    ~10 ms after "[EP-fallback] Async mod-loader spawned", 0xC0000409 (GS cookie / fast-fail) at
    Mewgenics.exe+0xD64D7D, before any mod is loaded (log: `test-logs/chainloader_run1_crash.log`).
    **Fix: `EnableEPFallback=0`** — the game does call a version.dll export ~2 s into startup, so mods
    load on that first proxy call. Must go in our install instructions. Worth reporting to Mewjector.
  - mgmp change: `dllmain.cpp` `wait_for_game_window()` — without the loader, init waits until the
    game's top-level window is visible (+1 s), matching the loader's "nothing before frame 1" timing.
    `mgmp_loader` sets `MGMP_LOADER=1` in the game's environment so its own launches skip the wait.
  - Result: `addresses: 54 resolved ... (pinned build)`, swap taken over, ImGui panel up, all default
    hooks live. Logs land beside the DLL (`mgmp_trace_*.log`, `mgmp_boot.log`).
  - Mewtator: a DLL inside an enabled mod folder is written into Mewtator's manifest for Mewjector
    (needs Settings > "Enable DLL Mod Support"). Mod folder = `D:\_Games\Mods\Mewtator\mods\MewgenicsOnline`
    (`info.json`, `mgmp.dll`, `mgmp.json`); `tools/deploy.ps1 -ModsDir ...` builds + copies (keeps an existing mgmp.json).
  - Mewtator has `concurrent_launches_enabled` — useful for two instances on one PC for LAN tests.
- 2026-10-11: Mewtator path verified: `MewtatorManifest=...\mods\mewtator_dll_manifest.txt` written into
  chainloader.ini by Mewtator; `[Mewtator] Loading: ...\MewgenicsOnline\mgmp.dll` OK; 54 resolved, 21 hooks live.
- 2026-10-11: **LAN finder, copied from Gungeon Online's F8 window** (user asked).
  - `src/net/mgmp_lan.{h,cpp}`: host answers UDP "MGMPLAN?" on the session port number (UDP 27600) with
    "MGMPLAN!" + proto + TCP port + players + name. Search sends to 255.255.255.255, every adapter's subnet
    broadcast (VPN adapters like Radmin would otherwise eat the global broadcast) and 127.0.0.1 (two copies on
    one PC); accepts replies 3 s, re-asks each 1 s; dedupes per name+port preferring a non-loopback address.
    Pumped from `session_update` (FrameBegin), non-blocking sockets, never touches the session or saves.
  - Player name: `mgmp_user.ini` beside the DLL (`[player] name, last_addr`); default = Steam persona name via
    flat API (`SteamAPI_SteamFriends_v018/v017` + `SteamAPI_ISteamFriends_GetPersonaName`), else Windows user.
  - UI: new "Mewgenics Online" ImGui window (name + Save, Host on LAN, address + Join, Find LAN games list,
    while connected: status, your LAN addresses, players, Leave). mgmp's session/log windows are behind a
    "Developer panels" checkbox. `net_port()` added to mgmp_net.
- 2026-10-11: **First LAN test (two copies, one PC)** -- with the OLD dll (panel host/join, new UI not deployed yet).
  Connected fine (proto 27, handshake both sides). Guest did nothing: the host had loaded its save and was in the
  House BEFORE pressing host, and mgmp only publishes on a save-slot click ("this host has not chosen a save slot
  in this process"). Guest sat on the save screen waiting.
  - Fix: `h_MewDirInit` calls `savefile_note_loaded(name)` on every non-redirected load (hook is live even with
    role off); `savefile_pump` adopts that name as the slot when there was no click, and publishes it.
  - **House handoff** (user wants the guest in the host's House): the House is NOT replicated (mgmp scope). The
    guest gets a COPY of the host's save, so it lands in the same House as last written to disk. When the host
    leaves the House for an adventure (House scene gone -> adventure loaded and on the map) or comes home (House
    scene back after an adventure was seen, +5 s for the game's own save), the host re-publishes as `fresh`; a guest
    not on a menu gets `leave_request_local()` (Esc -> mod presses Quit To Menu) + a UI banner, then auto-Play
    loads the new save. `leave_scene_live(name)` added to mgmp_leave. Not yet tested.
- TODO noticed:
  - Live House sync (seeing each other's House actions) is out of scope for now: the house sim draws RNG with no
    re-seed (CLAUDE.md "The house"), so only host-authoritative state sync could work.
  - `net_join` does a blocking `connect()` on the game thread: a wrong/unreachable IP freezes the game up to
    ~20 s. Make it non-blocking.
  - `tune::kHookSaveScum` disables the save-scum penalty even in plain single-player with the mod
    loaded. Should only apply while in a co-op session.
  - Hook compatibility: mgmp uses its own MinHook + verifies prologues by signature. Another Mewjector mod
    that hooks the same functions first (e.g. Combat Roster hooks FrameBegin, NextTurn, StatusMenu::update)
    would make mgmp's signatures miss -> refuse to load. Consider MJ_InstallHook when Mewjector is present.
- 2026-10-11: **House rules (user spec)**: guest may only look (storage, pick up/inspect cats); End Day on the
  host ends the guest's day too; ghost cursor of the other player only while that player has a menu open;
  Force Sync button for the guest. Second LAN test showed the join works end to end (guest auto-Play -> save
  screen -> redirected load of mgmp_coop.sav -> House). Guest pressing End Day did nothing for the host (expected).
  - RE (capstone over the shipped exe, `scratchpad/re_tool.py`): `AddComponent<EndDayTransition>` = RVA 0x1F77F0
    (only function naming the literal "EndDayTransition"; callers: the sign lambda chunk 0x1EF495 + three
    console-command-like functions taking vector<string>). `AddComponent<DepartTransition>` = 0x3C9F90 (only
    caller 0x3B13F0 "start adventure with team", itself called from 0xAE360). Both begin
    `cmp byte [rcx+0x4B0],0 / jne -> return 0` (scene dying), so returning null is a native path; callers ignore rax.
    Button::Click = RVA 0x97E8E0 on this build (`kCalls`), name at Button+504.
  - `src/session/mgmp_house.{h,cpp}`: finds both creators at runtime by the string literal (scan .text for a
    rip-relative ref, RtlLookupFunctionEntry for the start, refuse if >1 function); MinHooks them + Button::Click.
    Guest: End Day / Depart return null + toast; House buttons with prefixes Shop_Buy, Furniture,
    Button_HouseBossResummon swallowed. Every distinct button name is logged at Trace ("HOUSE button pressed")
    for refining the blocklist. Host: End Day -> `savefile_on_end_day()`.
  - Savefile handoffs now: ToAdventure / ToHouse / DayEnd (wait for the save's mtime to change, House live,
    +2 s; 60 s fallback) / Sync (host in House or on the map). Client on a fresh save outside the menus:
    must_rejoin + leave armed + `house` posts VK_ESCAPE (down, +80 ms up; max 3, 6 s apart) so the pause menu
    opens and mgmp_leave presses Quit To Menu, then auto-Play reloads.
  - Proto 28: MSG_SYNCREQ (client -> host). CURSOR with nx < 0 = hidden. Outside battle each peer sends its
    pointer only while in the House with NO non-base scene live (user corrected: hide while a menu is open;
    only House or battle), base set in kBaseScenes, and the hide sentinel
    otherwise. Scene-set changes logged at Trace ("HOUSE scenes: ...") to learn menu scene names.
  - Known gap: blocking Depart happens after the guest has picked a team (0x3B13F0 continues after the null).
- 2026-10-11: **Test 3 findings + fixes.**
  - Force Sync "logged the guest out": NOT the sync. The hidden-cursor sentinel nx = -1 fails `dec_cursor`'s
    strict `nx > -1.0f` bound -> host logs "undecodable CURSOR frame -- closing" and drops the peer every time a
    cursor went hidden (menus). Now hidden = `mode == kCursorHiddenMode (0xFF)` with nx = ny = 0.
    Lesson: mgmp's decoders close the connection on any out-of-range field.
  - The main menu loads `test00.sav` through MewDirector::init by itself -> `savefile_note_loaded` now only
    records names containing "campaign".
  - Guest could open the Boon County map and stores. House objects are Buttons named 'HouseButton' /
    'GenericButton'; the name is useless. Button::Click (0x97E8E0) runs `[[button+0xF0]]+0x10` = the lambda
    body; its RVA identifies the action. Store entry from the map = lambda 0x280270 (get-or-create scene
    "NPCOffice", add office component) -> blocked for the guest (pinned-build only). Every House button press is
    now logged at Info as "button pressed: '<name>' action <RVA>" so the map opener can be identified next test.
    NPCMapDrawer: 0x1A94C0/0x1A9580 are type-name getters; registry vtable 0xEE65A0 (28 slots) -- not the opener.
  - Flicker (mod panel + Steam overlay): unknown yet. Added "frame pacing: N frames, M swaps" log (every 10 s,
    6 times) to see whether the game presents more/less than once per frame.
- 2026-10-11: **Test 4** (guest clicked: map, storage, drag cat, cat info, build menu, store 1, store 2).
  - Logged actions: HouseButton 0x205270 (shared by map/storage/build -- the dedupe only printed it once),
    GenericButton 0x205430 (then the NPCOffice scene opened = store entry; body `mov rcx,[rcx+8]; jmp 0x27CCB0`),
    GenericButton 0x285EB0 (inside the store), GenericButton 0x2053F0 (`jmp 0x27CD80`, store-related, left
    unblocked in case it is the exit). 0x280270 never fired from a click.
  - 0x205270 = generic House "toggle panel": the panel object is the capture at impl+0x10; opens via 0x203C80,
    closes via 0x2049D0. Now the log prints the panel's RTTI class, and the guest is blocked for panels whose
    class contains NPCMap / Furniture / Build (guesses -- confirm names next test). Store entry 0x205430 blocked.
  - Frame pacing: 1.00 swaps per frame on both peers (~175 fps uncapped), 1.1 only at boot. So the flicker of the
    mod panel + Steam overlay + game cursor (all drawn last in a frame) is NOT double presents. Open; next step is
    isolating it (mod disabled / one copy / vsync on).
