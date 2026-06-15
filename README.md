# mctde-Link

`mctde-Link` is a `d3d9.dll` proxy for Dark Souls: Prepare to Die Edition. It loads from the game `DATA` folder and provides the MCTDE network overlay, HP display, true-ping side channel, compatibility chainloading, and popup suppression used by the PTDE multiplayer tooling setup.

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
ChainloadFolder=mctde-Link-Chainload
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
GenericDLL0=PhantomBreak.dll
```

If the DLL is not in the PTDE `DATA` folder, use the full path instead:

```ini
[DLLs]
GenericDLL0=C:\Path\To\PhantomBreak.dll
```

Use the first empty `GenericDLL` slot if `GenericDLL0` is already used.

To disable Phantom Break, blank its `GenericDLL` entry in `mctde-link.ini`:

```ini
[DLLs]
GenericDLL0=
```

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
