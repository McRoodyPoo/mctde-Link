# Changelog

## v0.1.1

Version-checker release.

- Updated the built-in version checker to read from `https://raw.githubusercontent.com/McRoodyPoo/mctde-Link/refs/heads/main/latest`.
- Updated the installed version to `0.1.1`.
- Updated the download URL to the `mctde-Link` GitHub releases page.

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
