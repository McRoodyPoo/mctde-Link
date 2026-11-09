// D3DOverlay.h
//
// In d3d9.dll this module no longer draws anything itself. It just holds the latest
// overlay bitmap (rasterized by the NetOverlay submit thread when [Render] Backend=d3d)
// and exports it via McTDE_GetOverlayBitmap. The actual in-frame drawing is done by the
// chainloaded companion (mctde_overlay.dll), which fetches that bitmap and draws it
// through DSFix's real device. (The legacy GDI layered-window path, Backend=gdi, lives
// in MCTDE_NetOverlay.cpp and doesn't use this module.)

#pragma once

// Kept for the d3d9 proxy call site; no longer installs any hooks (companion does the work).
void D3DOverlay_HookFactory(void* pIDirect3D9, bool isEx);

// Backend selection bookkeeping (driven by [Render] config).
void D3DOverlay_SetEnabled(bool enabled);
void D3DOverlay_SetDrawAtPresent(bool atPresent);

// Submit a top-down 32-bit BGRA (0xAARRGGBB) bitmap; the companion reads it each frame.
// width<=0 means "nothing to draw". corner: 0=TL,1=TR,2=BL,3=BR; padX/padY inset px.
void D3DOverlay_Submit(const void* pixelsBGRA, int width, int height, int corner, int padX, int padY);

void D3DOverlay_Shutdown();
