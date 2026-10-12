# Mewgenics Online — LAN co-op for Mewgenics

> **Very early alpha. A side project.** Expect bugs, back up your saves, and
> please send logs when something goes wrong.

An unofficial mod that lets two people play Mewgenics together: the **host**
plays their own save, and the **guest** joins and is brought into the host's
game — their House, their cats, and their adventures.

This is a fork of **[mgmp by SanTertrust](https://github.com/SanTertrust/mgmp)**
(MIT), which did the hard part: synchronised battles, shared adventures,
reconnects and desync detection. This fork adds Mewtator/Mewjector loading, a
LAN game finder, Steam invites, shared House visits and a shared End Day.

It is not an official feature and has nothing to do with the developers of
Mewgenics. Built with the help of [Claude](https://claude.ai/) (Anthropic's
AI) — most of the code in both the original and this fork was written by AI.

---

## Your saves are safe

- The guest's own saves are **never written**. The host's game is copied into a
  separate `mgmp_coop.sav`, and the guest plays on that copy. The previous copy
  is backed up to `mgmp_backups` before each new one is written.
- The mod **never deletes** a save.
- Still: back up your save folder before trying an alpha mod.
  `%APPDATA%\Glaiel Games\Mewgenics\<your id>\saves`

---

## What works

**Adventures** (from mgmp): shared map, shops, events, level-ups and inventory;
lockstep battles where each player controls half the cats; each other's mouse
and aim preview; rejoin mid-battle; desync detection.

**This fork:**

- **Loads through Mewjector**, so you can turn it on and off in **Mewtator**.
- **Mewgenics Online window** (F1): your name, *Host on LAN*, join by address,
  and **Find LAN games** — a list of hosts on your network you click to join.
- **Join the host at home.** The host can load their save first and host from
  the House; the guest is brought into a copy of the host's House.
- **Guest is look-only in the House.** The guest can open storage, pick up
  cats and read cat info. End Day, adventures, the Boon County map, stores and
  the build menu are the host's.
- **Ghost cursors in the House**, shown only while that player has no menu or
  panel open (panels slide the view, so the position would be wrong).
- **Shared End Day.** When the host ends the day, the guest's game ends its day
  too and both roll the night's events from the same seed. *(New — still being
  tested.)*
- **Force Sync** button for the guest: reloads the host's game from scratch.
- Leaving the House for an adventure (or coming home) brings the guest along.

- **Steam invites and joining** (new, not yet tested between two PCs): *Host on
  Steam*, then *Invite friend*, or friends pick *Join Game* in the Steam friends
  list. No addresses or port forwarding — the connection goes through Steam.

**Not yet:** a live shared House
(the guest's House is a copy and does not update when the host moves things
mid-day), 3–4 players.

Pinned to **Mewgenics 1.1.21239** (Steam build 25143593).

---

## Install

You need Windows 64-bit, Mewgenics on Steam, **[Mewtator](https://www.nexusmods.com/mewgenics/mods/1)**
and **[Mewjector](https://www.nexusmods.com/mewgenics/mods/218)**. Both
players install the same version of this mod.

1. **Mewjector:** put `version.dll` and `chainloader.ini` next to
   `Mewgenics.exe`. Then open `chainloader.ini` and set
   **`EnableEPFallback=0`** — with the default, the game crashes at startup on
   this build.
2. **This mod:** copy the `MewgenicsOnline` folder (containing `mgmp.dll`,
   `mgmp.json`, `info.json`) into Mewtator's `mods` folder.
3. **Mewtator:** in Settings set your game folder and tick **Enable DLL Mod
   Support**, then enable **Mewgenics Online** in the mod list.
4. Launch the game from Mewtator. Press **F1** to show/hide the Mewgenics
   Online window.

To test with two copies on one PC, turn on *Allow launching concurrent game
instances* in Mewtator's settings.

---

## Playing over Steam

1. **Host:** load your save, press **Host on Steam**, then **Invite friend
   (Steam overlay)**.
2. **Guest:** accept the invite (or right-click the host in your Steam friends
   list and pick **Join Game**). If your game is not running, Steam starts it
   and the mod joins once it is up. Stay on the main menu; the mod loads you in.

## Playing over LAN

1. **Host:** load your save as normal (you can be at home or start a run), then
   press **Host on LAN** in the Mewgenics Online window. Allow Mewgenics through
   Windows Firewall on private networks if asked.
2. **Guest:** stay on the **main menu**, press **Find LAN games** and click the
   host (or type the host's address and press Join). The mod presses Play and
   loads you into the host's game by itself.
3. If your game ever looks different from the host's, press **Force Sync**.

Over the internet, use a virtual LAN (Tailscale, ZeroTier, Radmin VPN,
Hamachi) or forward TCP port **27600** on the host's router; then type the
host's address.

---

## Building from source

Visual Studio 2022/2026 (or Build Tools) with C++ and CMake:

```powershell
cmake -B build -A x64
cmake --build build --config Release
ctest --test-dir build --build-config Release
powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 -ModsDir "<Mewtator>\mods"
```

`MODLOG.md` is this fork's development journal; `CLAUDE.md` is mgmp's
reverse-engineering notes.

---

## Logs

Logs are written next to `mgmp.dll` (`mgmp_trace_<date>-<time>_<pid>.log`).
When reporting a problem, send **both** players' logs.

---

## How it works, briefly

Two players staying in sync is not one problem, it is two, and they need
opposite solutions.

**Battles are deterministic.** Given the same starting position, the game plays
out identically every time, right down to the individual dice rolls — this was
measured, not assumed. So the mod does not send the *result* of anything.
It sends only what each player clicked, and both machines work out the
consequences independently and arrive at the same place. This is called
lockstep, and it is why battles cost almost no bandwidth.

**Everything outside battle is not deterministic**, but it is saveable. The
game already knows how to write a cat, an inventory and a run's history to
disk. So for the map, shops, level-ups and events, the host simply decides and
sends the result over.

Both halves are checked constantly. Every turn of every battle, and every time
you enter a map node, the two machines exchange a fingerprint of their state
and compare it. Agreement is proven, not hoped for.

The full technical story — every address, struct offset, measurement and dead
end — is in `CLAUDE.md`. It is long, and it is the real documentation.

---

## Project layout

```
src/core/          loading, hooking, logging, config, crash reporting
src/determinism/   random number tracking, recording and replay
src/net/           the network transport and the message format
src/session/       everything that actually keeps two games in step
src/ui/            the debug panel and the on-screen overlay
loader/            mgmp_loader.exe
tests/             unit tests for the logic that can be tested alone
tools/             Python and PowerShell helpers used while developing
third_party/       Dear ImGui, MinHook, stb_image, nlohmann/json
```

Run the tests with:

```powershell
ctest --test-dir build --build-config Release
```

---

## A note on what this is

This mod contains no Mewgenics code, art or data. It is an independent program
that attaches to a legally purchased copy of the game on your own machine. You
need to own the game; nothing here will help you play it if you do not.

It is not affiliated with, endorsed by, or supported by the developers or
publisher of Mewgenics. If it breaks your save, that is on you — back up your
saves before using it. If you report a bug to the game's developers while this
mod is loaded, please mention that it is loaded.

Mewgenics is by Tyler Glaiel and Edmund McMillen. It is a very good game and
you should buy it.

---

## Credits

- **[mgmp](https://github.com/SanTertrust/mgmp)** by SanTertrust — the original
  co-op mod this fork is built on (MIT).
- **[Mewjector](https://github.com/githubuser508/mewjector)** and
  **[Mewtator](https://www.nexusmods.com/mewgenics/mods/1)** — loading and mod
  management.
- Dear ImGui, MinHook, stb_image, nlohmann/json (see `third_party/`).

If you enjoy this, you can support me on [Ko-fi](https://ko-fi.com/alexjustalex).
