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

The WebSocket server is local-only and binds to `127.0.0.1`.

## Build

Requirements:

- Visual Studio 2022
- Desktop development with C++
- x86 build tools

Build command:

```powershell
& 'Z:\Visual Studio\MSBuild\Current\Bin\MSBuild.exe' '.\mctde-Link.sln' /t:Clean,Rebuild /p:Configuration=Release /p:Platform=x86 /m /v:minimal
```

## Repository Layout

```text
mctde-Link.sln          Visual Studio solution
mctde-Link/             Source and project files
dist/                   Release-ready drag/drop files
samples/                Example INI files
docs/                   Compatibility notes and patch history
```
