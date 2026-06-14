# mctde-Link

`mctde-Link` is a `d3d9.dll` proxy for Dark Souls: Prepare to Die Edition. It loads from the game `DATA` folder and provides the MCTDE network overlay, HP display, true-ping side channel, compatibility chainloading, and popup suppression used by the PTDE multiplayer tooling setup.

## Release Install

Copy these files from `dist` into your PTDE `DATA` folder:

```text
d3d9.dll
mctde-link.ini
```

Example target:

```text
Dark Souls Prepare to Die Edition/DATA/
```

If you chainload other DLLs, put them in the folder configured by:

```ini
[Compatibility]
ChainloadFolder=mctde-Link-Chainload
```

## Build

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
dist/                   Release-ready drag/drop files
samples/                Example INI files
docs/                   Compatibility notes and patch history
```
