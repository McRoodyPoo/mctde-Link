# Changelog

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
