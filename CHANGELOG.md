# Changelog

## v0.4.5 - Clean Exit

- When the game is started without the launcher (Steam-direct launch or a folder double-click), the mod still closes the game and reopens the launcher — but now spawns the launcher outside Steam's kill-on-close job. Previously the reopened launcher stayed in Steam's process tree, so Steam kept counting the game as running even after it closed; now Steam registers the close.

## v0.4.4 - Homecoming

- Fixed the launcher not reopening after a mod auto-update.
- The update helper now relaunches `mctde_launcher.exe` when present (unless `[Launcher] RequireLauncher` is 0), instead of letting the updated game run directly.

> Note: applies to updates started from 0.4.4 onward; the 0.4.x → 0.4.4 update itself still uses the old helper.

## v0.4.3 - Reversal

- Reverted the 0.4.2 launcher-guard change. Restored the original behavior: starting the game without the launcher (folder/direct launch) reopens mctde_launcher.exe, exactly as in 0.4.1. 0.4.2's Steam-only scoping stopped the launcher from reopening on a folder launch; this undoes that.

## v0.4.1 - GooeyInterface

- Renamed the in-game phantom-cap feature from MorePhantoms to PhantomUnleashed (config section is now `[PhantomUnleashed]`).
- Existing `[MorePhantoms]` settings are no longer read; re-enable the feature through the launcher or rename the section to `[PhantomUnleashed]`.
- mctde-Link now runs through the mctde launcher: starting the game directly reopens the launcher instead.
- If the launcher is missing, mctde-Link offers to download and install it from GitHub over HTTPS, verified by hash, only with your consent.
- Added `[Launcher] RequireLauncher` (default on) to turn the launcher requirement off.

## v0.3.0 - PhantomUnleashed

- Raises the co-op/invasion phantom cap from 4 to 18.
- Adds a launch prompt to enable it; disabled by default.
- Puts you in a separate matchmaking pool with only other PhantomUnleashed players.
- Overlay now shows up to 18 players.
- Enlarges the game's memory pool so larger sessions have room.
- Won't run alongside Phantom_Break, to avoid a crash.
- Linux/Proton: the Ask prompt is unsupported — set Mode=On or Off in mctde-link.ini.
- Special thanks to Metal-Crow for permission to use his reverse engineering and techniques.

## v0.2.1 - Steam Bendoverlay

- Fixed the Steam overlay colliding with the mod overlay, which could flash the screen white or briefly freeze the game whenever a friend-activity notification appeared. The overlay was opening an extra render pass each frame that re-triggered Steam's notification compositor in a broken render state; it now draws without that extra pass, so Steam composites normally.
- Removed the OK pop-up windows that say "?LeaveName? has returned home" upon a phantom disconnecting. Their connection status will now be represented in the overlay instead.

## v0.2.0 - Auto-Update

- Added an in-game auto-updater. When a newer version is available, choosing **Yes** on the update prompt now downloads and installs the latest build automatically, then relaunches Dark Souls — no manual download needed.
- The updater always pulls the newest release, so players who skipped several versions still land on the current build in one step. Your `mctde-link.ini` settings are preserved across updates.
- Choosing **No** now opens the latest releases page and closes the game.
- mctde (on NexusMods) keeps its manual download flow, since it can't be installed automatically.

> Note: updating *to* 0.2.0 from an older build is a one-time manual download. From 0.2.0 onward, future updates install themselves.

## v0.1.3 - Phantom Unbreak

- Overlay now renders as a true in-frame device wrapper inside d3d9.dll, drawing on top of DSFix's finished frame instead of fighting it.
- The obsolete mctde_overlay.dll companion is ignored automatically if left behind in the chainload folder, so updating can't double-hook DSFix.
- Fixed a device-teardown resource leak that could crash on resolution changes or device recreation.

## v0.1.2 - Performance Upgrade

- Fixed the overlay causing frametime stutter.
- Overlay now draws in-game instead of as a separate window.
- Now works properly with DSFix.

## v0.1.1 - VersionChecker Expansion

- Expanded the built-in version checker to read both `mctde` and `mctde-link` versions from `latest.txt`.
- Set bundled version values to `mctde=0.88` and `mctde-link=0.1.1`.
- Added separate update prompts and download targets for mctde and mctde-link.
- Prioritized the mctde update prompt when both mctde and mctde-link are out of date.
- Changed the No button behavior on update prompts to close Dark Souls.

## v0.1.0

First full release candidate.

- Added PTDE `d3d9.dll` proxy/hub loading.
- Added network overlay with HP, name, ping, and role styling.
- Added true-ping side-channel support for compatible peers.
- Added cached/session ping fallback for non-compatible peers when available.
- Added HP read path using PTDE/Ashley-style offsets.
- Added popup suppression for returned-home/session messages.
- Added compatibility chainload support for other DLLs.
- Stabilized overlay display rows so HP, names, and ping do not blank during missed reads.
- Disabled runtime logging by default.
