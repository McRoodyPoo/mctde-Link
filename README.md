# mctde-Link

`mctde-Link` is a `d3d9.dll` proxy for Dark Souls: Prepare to Die Edition. It loads from the game `DATA` folder and provides the MCTDE network overlay, HP display, true-ping side channel, compatibility chainloading, and popup suppression used by the PTDE multiplayer tooling setup.

## Built for PTDE PvP

`mctde-Link` is meant for competition-grade PTDE PvP sessions where every player slot, HP read, and latency swing matters. The overlay is intentionally compact: it gives you matchup-critical information without turning the game screen into a dashboard.

## Features

- Compact PvP roster overlay showing player HP, names, ping, and role/color styling.
- Live ping monitor via Steam side channel for compatible peers running `mctde-Link`.
- Cached/session ping fallback for players who are not running the DLL when Steam or the game exposes usable ping data.
- HP display using PTDE/Ashley-style offsets.
- Optional local OBS/browser overlay served from `127.0.0.1` for stream layouts, recording, or tournament capture.
- Compatibility hub behavior through `GenericDLL` entries and optional chainload folder support for other PTDE tools.
- Returned-home/session popup suppression to reduce match-flow noise.
- INI-driven controls for overlay position, font, line height, local-player hiding, HP polling, true-ping behavior, WebSocket output, debug logging, and compatibility DLL loading.

## Quality of Life

- Display smoothing/floor sampling is configurable for players who want stable ping reads instead of frame-to-frame jitter.
- HP polling, 1 HP linger timing, and overlay refresh behavior are configurable for different setups.
- The overlay can be anchored to any corner and tuned with padding, font, and row spacing controls.
- The local player row can be hidden when you only care about opponents and partners.
- Debug options are split out so troubleshooting can be enabled without permanently making the release config noisy.

## Install

This repository is source code only. For normal installation, download the latest release zip from GitHub Releases.

The release zip contains exactly these files:

```text
d3d9.dll
mctde-link.ini
README.md
CONFIG_REFERENCE.md
```

Copy `d3d9.dll` and `mctde-link.ini` to:

```text
Dark Souls Prepare to Die Edition/DATA/
```

Keep `README.md` and `CONFIG_REFERENCE.md` anywhere convenient for reference. They do not need to be copied into the game folder.

## Optional Chainloading

If you chainload other DLLs, put them in the folder configured by:

```ini
[Compatibility]
ChainloadFolder=mctde-Link_Chainload
```

By default, that folder should live next to `d3d9.dll` in the PTDE `DATA` folder.

## Phantom Break Compatibility

Phantom Break can be loaded alongside `mctde-Link` through an explicit `GenericDLL` entry in `mctde-link.ini`, but the overlay is not fully compatible with Phantom Break yet.

Current limitation: the overlay roster is still built around the normal four-player PTDE world layout. A fifth Phantom Break player will not populate into a new fifth row, and if one of the four rostered players leaves, the fifth player will not automatically move into the open overlay slot.

To enable Phantom Break with `mctde-Link`:

1. Keep `d3d9.dll` and `mctde-link.ini` in the PTDE `DATA` folder.
2. Put Phantom Break's DLL in the PTDE `DATA` folder, or keep it elsewhere and use its full path.
3. Add it to an open slot under `[DLLs]`:

```ini
[DLLs]
GenericDLL0=Phantom_Break.dll
```

If the DLL is not in the PTDE `DATA` folder, use the full path instead:

```ini
[DLLs]
GenericDLL0=C:\Path\To\Phantom_Break.dll
```

Use the first empty `GenericDLL` slot if `GenericDLL0` is already used. If your copy has a different filename, use that exact filename in the `GenericDLL` entry.

To disable Phantom Break, blank its `GenericDLL` entry in `mctde-link.ini`:

```ini
[DLLs]
GenericDLL0=
```

## MorePhantoms (built-in phantom-cap raise)

`mctde-Link` now includes its own optional phantom-cap raiser, built directly into
`d3d9.dll` — no separate DLL or chainload entry is needed. It lifts PTDE's stock limit of
four simultaneous players in a world.

### Launch-time prompt

When the game starts you'll see a **Yes / No** box:

> **Enable MorePhantoms?** This raises the co-op / invasion phantom cap above the stock 4.
> While enabled you will **only** be able to connect with other players who also have
> MorePhantoms enabled. Choose **No** to play with everyone on the normal phantom limit.

- **No** — nothing is patched; you stay in the normal pool and can connect with everyone.
- **Yes** — the phantom-cap patches are applied and you join the **segregated MorePhantoms
  pool**, so a vanilla player is never pulled into a larger-than-four session.

### Configuration

The `[MorePhantoms]` section of `mctde-link.ini` controls it:

```ini
[MorePhantoms]
Mode=Ask          ; Ask = prompt each launch (default) | On = always | Off = never
MaxPhantoms=18    ; total player slots in your world (stock 4; range 4-32)
NetworkVersion=0x4D ; matchmaking pool key (vanilla retail = 0x2E). This is what segregates
                  ;   you from the vanilla pool; all players must share it AND MaxPhantoms.
MemoryPoolMB=192  ; game internal memory pool (stock ~10 MB is too small for many phantoms;
                  ;   0 = leave stock, max 255)
VerifyOnly=1      ; 1 = only log the offset self-check, never patch. Set 0 to apply.
```

Stock DS1 self-allocates only a ~10 MB internal memory pool — fine for 4 players, but too
small once many phantoms load their models, gear, and animations, which would cause
allocation failures. MorePhantoms raises this pool (default 192 MB, capped at a safe 255 MB)
so a full session has room. (DARKSOULS.exe is 32-bit, so total memory is still bounded by the
~2 GB address space — 4 GB if a tool like DSFix enables Large Address Aware.)

**How segregation works:** Dark Souls only pairs players whose 1-byte network version
matches. Raising the phantom cap alone does **not** separate you from vanilla players — you
must also change this version. MorePhantoms writes `NetworkVersion` (default `0x4D`) at the
game's three version-check sites, so a MorePhantoms client only ever connects to other
MorePhantoms clients sharing the same value. (Reverse engineering of the version sites:
Metal-Crow's Overhaul.)

The prompt and patching happen on the game's main thread at its first Direct3D call —
before the game window opens and before any multiplayer/session init — because the phantom
cap and pool segregation must be in place before the game comes up. The VERIFY report is
written to `MorePhantoms.log` next to `d3d9.dll` regardless of the `[Settings] EnableLogging`
switch.

> **Status / safety:** this feature ships with `VerifyOnly=1`. In that mode it only writes a
> verification report to `MorePhantoms.log` and never modifies the running game. Set
> `VerifyOnly=0` to actually apply. Both the Stage 1 cap/segregation patches and the Stage 2
> runtime offset-shift trampolines (which make a larger-than-four session stable) are
> implemented; Stage 2 is calibrated for `MaxPhantoms=18`. Every patch is reversible and is
> reverted automatically when the game closes.
>
> **Do not run alongside Phantom_Break.** Both mods patch the same phantom-cap offsets;
> running them together double-patches the game and crashes. MorePhantoms detects a loaded or
> chainloaded `Phantom_Break.dll` and refuses to apply.

This feature is a rewrite of Metal-Crow's MultiPhantom /
[Dark Souls Overhaul](https://github.com/metal-crow/Dark-Souls-1-Overhaul) phantom-limit
patch, re-implemented on mctde-Link's own reversible patch engine (with parameterization,
verification, logging, and restore-on-teardown). The patch sites, offset facts, and AoB
patterns are credited to Metal-Crow's original reverse-engineering work.

## OBS / WebSocket Overlay

The mod can also serve a local browser overlay for OBS or other browser-source tools.

Enable it in `mctde-link.ini`:

```ini
[WebSocket]
Enabled=1
Port=39876
SendMs=33
```

Then add this URL as an OBS Browser Source:

```text
http://127.0.0.1:39876/overlay.html
```

If you change `Port`, replace `39876` in the URL with your configured port. This is a local WebSocket/browser-source server, not an outbound webhook. It binds to `127.0.0.1`.

## Build From Source

Requirements:

- Visual Studio 2022
- Desktop development with C++
- x86 build tools

Build command:

```powershell
& 'Z:\Visual Studio\MSBuild\Current\Bin\MSBuild.exe' '.\mctde-Link.sln' /t:Clean,Rebuild /p:Configuration=Release /p:Platform=x86 /m /v:minimal
```

The DLL is emitted as:

```text
Release/d3d9.dll
```

To make a user-facing release package, include only:

```text
d3d9.dll
mctde-link.ini
README.md
CONFIG_REFERENCE.md
```

## Runtime Notes

- Logging is disabled by default in `mctde-link.ini`.
- The overlay keeps confirmed names stable for the current roster.
- HP, name, and ping fields are designed to keep their last good value until a valid replacement exists.
- True ping requires the remote player to run a compatible build. Non-compatible players can still show cached/session ping when the game or Steam exposes it.
- See `docs/CONFIG_REFERENCE.md` for an explanation of every INI option.

## Repository Layout

```text
mctde-Link.sln          Visual Studio solution
mctde-Link/             Source and project files
docs/                   INI reference, compatibility notes, and patch history
samples/                Example INI files
```

## Bibliography

This project/library was built with reference to community Dark Souls: Prepare to Die Edition modding and reverse-engineering work, including:

- DS Gadget by JKAnderson; runtime memory tooling and PTDE testing reference: <https://github.com/JKAnderson/DS-Gadget>
- PTDE Practice Tool; practice/debug tooling and PTDE hook/reference work: <https://github.com/veeenu/ptde_practice_tool>
- DSCM / DaS-PC-MPChan; multiplayer/network tooling reference: <https://github.com/metal-crow/DaS-PC-MPChan>
- DSCM-Net server; network backend reference for DSCM: <https://github.com/Chronial/dscm-server>
- dsfix; PTDE compatibility, rendering, and DLL-loading reference: <https://github.com/PeterTh/dsfix>
- Cheat Engine; memory inspection and Cheat Engine table research: <https://cheatengine.org/>
- Ghidra; executable analysis and reverse-engineering work: <https://github.com/NationalSecurityAgency/ghidra>
- Dark-Souls-1-Overhaul; Phantom_Break.dll compatibility/network reference: <https://github.com/metal-crow/Dark-Souls-1-Overhaul>

## Special Thanks

Special thanks to **Ashley**, **MetalCrow**, **Sean Pesce** **Eloise**, **Dasmins**, **Alax**, JKAnderson, Wulf2k, Chronial, Peter “Durante” Thoman, Grimrukh, and the broader Dark Souls modding and reverse-engineering community.

Original game by **FromSoftware** and **Bandai Namco**.
```
