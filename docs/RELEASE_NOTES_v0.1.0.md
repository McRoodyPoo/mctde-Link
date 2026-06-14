# mctde-Link v0.1.0

This is the first public release candidate.

## Included

The end-user release package contains only:

- `d3d9.dll`
- `mctde-link.ini`
- `README.md`
- `CONFIG_REFERENCE.md`

The source package contains the Visual Studio solution, project source, sample config, docs, and changelog.

## Default Config

- Logging disabled
- Overlay enabled
- HP enabled
- WebSocket disabled
- True ping enabled on side channel `63`
- True-ping hello retry set to `500ms`
- True-ping normal send interval set to `1000ms`

## Known Notes

True ping only works with compatible peers. Players without the DLL may show cached/session ping when that data is available, but they cannot respond to the custom true-ping side channel.
