// D3DOverlay.h
//
// In-frame Direct3D9 overlay, fully contained in d3d9.dll. The NetOverlay submit thread
// rasterizes the overlay to a BGRA bitmap and calls D3DOverlay_Submit() (when [Render]
// Backend=d3d). This module hooks the real device (via D3DOverlay_HookFactory) and draws
// that bitmap in-frame at Present, on top of DSFix's finished frame. No separate companion
// DLL, chainload-folder entry, or throwaway device is involved. (The legacy GDI
// layered-window path, Backend=gdi, lives in MCTDE_NetOverlay.cpp and doesn't use this module.)

#pragma once

// Called by the d3d9 proxy with the real IDirect3D9 it returns to the game / DSFix.
// (Device hooks are installed earlier via D3DOverlay_InstallEarly; this only inits the lock.)
void D3DOverlay_HookFactory(void* pIDirect3D9, bool isEx);

// Install the device Present/Reset hooks early -- from HubThread during startup, BEFORE DSFix
// hooks d3d9 -- so our overlay draws inner to (on top of) DSFix's frame composite. Pass the
// real system Direct3DCreate9 function pointer (used to spin up a throwaway probe device whose
// shared vtable is patched). Safe to call once; subsequent calls are no-ops.
void D3DOverlay_InstallEarly(void* direct3DCreate9Fn);

// Backend selection bookkeeping (driven by [Render] config).
void D3DOverlay_SetEnabled(bool enabled);
void D3DOverlay_SetDrawAtPresent(bool atPresent);

// Submit a top-down 32-bit BGRA (0xAARRGGBB) bitmap; the companion reads it each frame.
// width<=0 means "nothing to draw". corner: 0=TL,1=TR,2=BL,3=BR; padX/padY inset px.
void D3DOverlay_Submit(const void* pixelsBGRA, int width, int height, int corner, int padX, int padY);

void D3DOverlay_Shutdown();
