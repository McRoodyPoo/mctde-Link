# mctde-link.ini Reference

This file controls the `d3d9.dll` hub, compatibility chainloading, overlay layout, HP polling, debug logging, WebSocket output, and true-ping behavior.

Boolean values use `0` for off and `1` for on.

## Settings

`EnableLogging=0`

Controls normal log output from the hub, overlay, and version checker. Keep this `0` for normal play. Set to `1` only while diagnosing a bug.

## Compatibility

`ChainloadFolder=mctde-Link-Chainload`

Names a folder next to `d3d9.dll` that will be scanned for additional DLLs to load. This is for compatibility with other PTDE tools or hook DLLs. The loader skips `d3d9.dll` itself and loads other DLLs in alphabetical order.

## DLLs

`GenericDLL0=` through `GenericDLL9=`

Optional explicit DLL entries to load during startup. Leave blank to disable. The code supports `GenericDLL0` through `GenericDLL31`, but the sample lists the first ten to keep the config readable.

## Overlay

`ShowHeader=1`

Legacy layout toggle. It is still read by the config loader, but the current compact overlay does not draw a header, so this has no visible effect in v0.1.0.

`MarkerGutterExtra=8`

Adds horizontal space around the ping/local marker between the HP field and the player name. Lower values make the row more compact. Clamped to `8` through `80`.

`FontFace=Tahoma`

Font used for overlay text. If empty or unavailable, the overlay falls back to `Tahoma`.

`FontHeight=24`

Height of normal overlay text, including names and ping markers. Clamped to `10` through `40`.

`HpFontHeight=24`

Height of the HP text. It cannot be smaller than `FontHeight` and is clamped to a maximum of `80`.

`LineHeight=20`

Vertical spacing for each displayed player row. The code raises this automatically if it is too small for the HP font.

`Corner=top_left`

Overlay anchor position. Supported values are `top_left`, `top_right`, `bottom_left`, and `bottom_right`. Short forms like `tr`, `bl`, and `br` are also accepted.

`PaddingX=5`

Horizontal distance from the selected screen corner.

`PaddingY=5`

Vertical distance from the selected screen corner.

`RefreshMs=1000`

Main overlay roster refresh interval in milliseconds. This controls slower row/state rebuilding, not the faster HP poll. Values below `250` are clamped to `250`.

`HideLocal=0`

When set to `1`, hides the local/self row and only shows remote players.

`ForceTopmost=0`

When set to `1`, makes the external overlay window topmost. Leave this `0` unless the overlay is being hidden behind the game or another window.

`ShowUnknownWorldRows=1`

Optional supported key. If present and set to `0`, rows without a learned identity can be hidden. The sample omits it because showing live world rows is safer for general use.

## HP

`Enabled=1`

Turns HP display and HP polling on or off.

`CurrentOffset=724`

Decimal offset from the character object to current HP. `724` is `0x2D4`, the default PTDE/Ashley-style current HP offset.

`MaxOffset=728`

Decimal offset from the character object to max HP. `728` is `0x2D8`, the default PTDE/Ashley-style max HP offset.

`OneHpLingerMs=500`

How long the 1 HP highlight should remain visible after a 1 HP write is detected. Values above `5000` are clamped.

`PollMs=33`

Fast HP polling interval in milliseconds. Lower values update HP more often but cost more CPU time. Clamped to `1` through `100`.

`AutoScan=0`

Optional diagnostic key. When enabled, HP reading can try a small list of known offset pairs if the configured pair fails. Leave off for release play.

`DetectInstantRefill=1`

Optional supported key. Detects the pattern where HP briefly hits 1 and is immediately refilled before a normal poll sees the exact 1 HP frame.

`InstantRefillMinGain=100`

Optional supported key. Minimum HP gain used by `DetectInstantRefill` to decide that a refill probably masked a 1 HP event. Clamped to `1` through `5000`.

## Debug

`DumpOverlayData=0`

Writes periodic snapshots of world rows, Steam nodes, identities, and overlay rows. Leave off unless debugging.

`DebugP2PBridge=0`

Logs P2P identity-learning and packet bridge details. Leave off unless debugging identity/name/ping issues.

`IdentityTtlMs=120000`

Optional supported key. Controls how long a learned player-number to SteamID identity may live after the row is no longer present. Minimum is clamped to `5000`.

## WebSocket

The WebSocket feature serves a local browser overlay. It is meant for OBS Browser Source or other local browser-source tools. It is not an outbound webhook.

Browser source URL:

```text
http://127.0.0.1:39876/overlay.html
```

If you change `Port`, replace `39876` in the URL with your configured port.

`Enabled=0`

Enables the local OBS/browser-source WebSocket overlay server. The external GDI overlay works without this.

`Port=39876`

Localhost port for the WebSocket/browser overlay server. Values outside `1024` through `65535` fall back to `39876`.

`SendMs=33`

How often WebSocket clients receive overlay JSON updates, in milliseconds. Clamped to `16` through `1000`.

## TruePing

`UseHighResTimer=0`

Enables `timeBeginPeriod(1)` while the true-ping thread is running. This can improve timing precision but may affect system timer behavior, so the release config leaves it off.

`PollSleepMs=33`

Sleep interval for the true-ping receive/send worker loop. Lower values react faster but can cost more CPU and frametime stability. Clamped to `0` through `50`.

`DisplayMode=2`

Controls displayed true-ping smoothing. `0` shows latest raw RTT/2 sample, `1` uses EWMA smoothing, and `2` shows the recent best/floor sample. `2` is recommended for stable display.

`BestWindow=8`

Number of recent samples considered by `DisplayMode=2`. Clamped to `1` through `16`.

`Enabled=1`

Master switch for true ping. If off, the overlay can still show Steam/session-table cached ping when available.

`Debug=0`

Logs true-ping state changes and diagnostics. Leave off for normal play.

`Verbose=0`

Optional supported key. Logs more true-ping packet/send/receive details than `Debug`. This is noisy and should stay off unless diagnosing protocol issues.

`PreferOverlay=1`

When true-ping data is available, prefer it over cached/session ping for display.

`ShowSourceMarker=1`

Shows a `(C)` marker for cached/session ping values. True-ping values display as plain `NNNMS`.

`SendEnabled=1`

Allows this client to send true-ping hello/ping packets.

`ReceiveEnabled=1`

Allows this client to read and respond to true-ping packets.

`Channel=63`

Steam P2P side channel used by true ping. Channels below `16` are treated as game-facing/reserved unless `AllowGameChannel=1`.

`AllowGameChannel=0`

Allows low Steam P2P channels if you explicitly need to test them. Keep this `0`; using game-facing channels can steal packets from Dark Souls.

`SendType=2`

Steam P2P send type for true-ping packets. Clamped to `0` through `3`.

`SendMs=1000`

Normal interval between true-ping samples after handshake, in milliseconds. Clamped to `100` through `10000`.

`HelloMs=500`

Retry interval for true-ping hello packets before handshake. Clamped to `500` through `30000`.

`StaleMs=4000`

How long a true-ping sample can remain fresh. If a peer is stale, the sender will try to refresh it. Clamped to `1000` through `60000`.

`SmoothDisplay=1`

Optional supported key. Enables smoothing unless `DisplayMode` is explicitly set. Mostly kept for backwards compatibility.

`SmoothWeight=4`

Optional supported key. EWMA weight used by `DisplayMode=1`. Clamped to `1` through `16`.

## Legacy Keys

These keys may exist in older test configs but are not read by the v0.1.0 source:

- `Overlay.AllowUnambiguousSteamFallback`
- `Overlay.AllowDisplayNamePingFallback`
- `Overlay.AllowRecentP2PFallback`
- `TruePing.StrictWorldTargets`
- `TruePing.AllowNameFallback`

Remove them from release configs to avoid confusion.
