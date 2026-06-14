# Install

1. Open the release folder.
2. Copy `dist/d3d9.dll` into the Dark Souls PTDE `DATA` folder.
3. Copy `dist/mctde-link.ini` into the same `DATA` folder.
4. Launch the game.

To uninstall, remove `d3d9.dll` from the `DATA` folder. Keep a backup if another mod also uses a `d3d9.dll` proxy.

## Optional Chainloading

The default config uses:

```ini
[Compatibility]
ChainloadFolder=mctde-Link-Chainload
```

Create that folder next to `d3d9.dll` and place compatible DLLs there if needed.

## Debug Logging

Logging is off by default. To enable it:

```ini
[Settings]
EnableLogging=1

[Debug]
DumpOverlayData=1
DebugP2PBridge=1
```

Turn logging back off for normal play.
