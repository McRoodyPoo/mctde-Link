# mctde-Link v1.0.0

This is the first full release candidate.

## Included

- `dist/d3d9.dll`
- `dist/mctde-link.ini`
- Visual Studio source project
- Install notes and compatibility docs

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
