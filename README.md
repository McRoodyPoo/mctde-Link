# mctde-Link

`mctde-Link` is a `d3d9.dll` proxy for Dark Souls: Prepare to Die Edition. Drop it in the game `DATA` folder and it adds the MCTDE PvP overlay, HP display, true-ping side channel, DLL chainloading, popup suppression, and a built-in phantom-cap raiser. It is built for competition-grade PTDE PvP where every slot, HP read, and latency swing matters.

## Features

- Compact roster overlay: player HP, names, ping, and role/color styling (anchorable to any corner, with the local-player row hideable).
- True ping for peers also running `mctde-Link` (Steam side channel), with cached/session ping fallback for everyone else. Optional smoothing for stable reads.
- HP display using PTDE/Ashley-style offsets.
- Built-in **PhantomUnleashed** phantom-cap raiser (see below).
- DLL chainloading for other PTDE tools via `GenericDLL` entries / a chainload folder.
- Returned-home / session popup suppression.
- Optional local OBS browser overlay (see below).
- Everything is INI-driven (`mctde-link.ini`); see `docs/CONFIG_REFERENCE.md` for every option.

## Install

Grab the latest release zip from GitHub Releases (this repo is source only). Copy `d3d9.dll` and `mctde-link.ini` into:

```text
Dark Souls Prepare to Die Edition/DATA/
```

`README.md` and `CONFIG_REFERENCE.md` are reference only (keep them wherever). To chainload other DLLs, drop them in the `[Compatibility] ChainloadFolder` (default `mctde-Link_Chainload`, next to `d3d9.dll`).

## PhantomUnleashed (built-in phantom-cap raiser)

Lifts PTDE's stock 4-player limit, built right into `d3d9.dll` (no extra DLL needed). On launch you get a **Yes / No** prompt:

- **No**: nothing is patched; you stay in the normal pool and connect with everyone.
- **Yes**: the cap is raised and you join a **segregated pool**, so vanilla players are never pulled into a larger session.

Configure it in `mctde-link.ini`:

```ini
[PhantomUnleashed]
Mode=Ask          ; Ask = prompt each launch (default) | On = always | Off = never
MaxPhantoms=18    ; total player slots (stock 4; range 4-32)
NetworkVersion=0x4D ; matchmaking pool key (vanilla = 0x2E). All players must share this AND MaxPhantoms.
MemoryPoolMB=192  ; internal memory pool (stock ~10 MB is too small for many phantoms; 0 = stock, max 255)
VerifyOnly=1      ; 1 = only log the offset self-check, never patch. Set 0 to apply.
```

**Segregation** is via `NetworkVersion`: DS1 only pairs players whose 1-byte version matches, so a raised cap *alone* doesn't separate you from vanilla. The version does. **Memory:** stock DS1's ~10 MB pool can't hold many phantoms' models/gear/animations, so the pool is raised (default 192 MB, max 255). Note DARKSOULS.exe is 32-bit (~2 GB address space, 4 GB with DSFix's Large Address Aware).

Patching happens on the main thread at the first Direct3D call, before the window opens; every patch is reversible and auto-reverts on close. A VERIFY report is always written to `PhantomUnleashed.log` next to `d3d9.dll`.

> Ships with `VerifyOnly=1` (log-only, never modifies the game). Set `VerifyOnly=0` to apply. Stage 2 runtime trampolines (what makes a >4 session stable) are calibrated for `MaxPhantoms=18`.
>
> **Do not run alongside Phantom_Break**: both patch the same offsets and double-patching crashes. PhantomUnleashed detects a loaded/chainloaded `Phantom_Break.dll` and refuses to apply. To use Phantom Break instead, set `Mode=Off` and chainload it as a `GenericDLL`.

This is a rewrite of Metal-Crow's MultiPhantom / [Dark Souls Overhaul](https://github.com/metal-crow/Dark-Souls-1-Overhaul) phantom-limit patch on `mctde-Link`'s own reversible patch engine; the patch sites, offsets, and AoB patterns are Metal-Crow's reverse-engineering work.

## OBS / WebSocket Overlay

Serve a local browser overlay for OBS. Enable in `mctde-link.ini`:

```ini
[WebSocket]
Enabled=1
Port=39876
```

Then add `http://127.0.0.1:39876/overlay.html` as an OBS Browser Source (swap the port if you changed it). It's a local server bound to `127.0.0.1`, not an outbound webhook.

## Build

Visual Studio 2022 with Desktop C++ and x86 build tools:

```powershell
& 'Z:\Visual Studio\MSBuild\Current\Bin\MSBuild.exe' '.\mctde-Link.sln' /t:Clean,Rebuild /p:Configuration=Release /p:Platform=x86 /m /v:minimal
```

Output is `Release/d3d9.dll`. A release package is just `d3d9.dll`, `mctde-link.ini`, `README.md`, and `CONFIG_REFERENCE.md`.

## Credits

Built with reference to the PTDE modding and reverse-engineering community, including [DS Gadget](https://github.com/JKAnderson/DS-Gadget), [PTDE Practice Tool](https://github.com/helloeloise/ptde_practice_tool), [DaS-PC-MPChan](https://github.com/metal-crow/DaS-PC-MPChan), [dscm-server](https://github.com/Chronial/dscm-server), [DSFix](https://github.com/PeterTh/dsfix), [Dark-Souls-1-Overhaul](https://github.com/metal-crow/Dark-Souls-1-Overhaul), Cheat Engine, and Ghidra.

Special thanks to **Ashley**, **[MetalCrow](https://github.com/metal-crow)**, **[Sean Pesce](https://github.com/SeanPesce)**, **[Eloise](https://github.com/helloeloise)**, **Dasmins**, **Alax**, **[JKAnderson](https://github.com/JKAnderson)**, **[Wulf2k](https://github.com/Wulf2k)**, **[Chronial](https://github.com/Chronial)**, **[Peter "Durante" Thoman](https://github.com/PeterTh)**, **[Grimrukh](https://github.com/Grimrukh)**, and the broader Dark Souls modding community.

Original game by **FromSoftware** and **Bandai Namco**.
