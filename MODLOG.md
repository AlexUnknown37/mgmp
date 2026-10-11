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
2. [ ] Load as a Mewjector mod (DLL in `mods/`, managed by Mewtator) instead of the injector.
3. [ ] Steam transport: host opens a Steam lobby / rich presence; client joins via friends list or invite.
       Transport behind the existing interface; keep TCP for LAN.
4. [ ] LAN: keep direct IP, add automatic host discovery on the local network.
5. [ ] Save hardening: back up all saves before each session; never delete; refuse to write any
       non-`mgmp_coop.sav` file while a client; check whether Steam Cloud uploads `mgmp_coop.sav`.
6. [ ] Packaging for Mewtator + README (credit upstream, Ko-fi link).

## Log
- 2026-10-11: Recon. KB note `games/mewgenics/combat-roster-panel.md` (native C++ engine, SDL3, Mewjector).
  Found mgmp; decided to fork rather than rewrite (user agreed). Baseline build OK with VS 2026.
