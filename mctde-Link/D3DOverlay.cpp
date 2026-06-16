// D3DOverlay.cpp -- see D3DOverlay.h.
//
// Bitmap producer/exporter only. The NetOverlay submit thread rasterizes the overlay to a
// BGRA bitmap and calls D3DOverlay_Submit(); the chainloaded companion (mctde_overlay.dll)
// fetches it via the exported McTDE_GetOverlayBitmap and draws it in-frame through DSFix's
// real device. No D3D hooking happens here anymore.
#include "pch.h"
#include <windows.h>
#include <vector>
#include "D3DOverlay.h"

static CRITICAL_SECTION  g_subLock;
static volatile LONG     g_subLockInit = 0;
static volatile LONG     g_subLockReady = 0;
static std::vector<BYTE> g_subPixels;
static int  g_subW = 0, g_subH = 0, g_subCorner = 0, g_subPadX = 0, g_subPadY = 0;
static bool g_subValid = false;
static bool g_enabled = false;

static void EnsureSubLockInit()
{
    if (InterlockedCompareExchange(&g_subLockInit, 1, 0) == 0)
    {
        InitializeCriticalSection(&g_subLock);
        InterlockedExchange(&g_subLockReady, 1);
    }
    else
    {
        while (!g_subLockReady)
            Sleep(0);
    }
}

void D3DOverlay_HookFactory(void* pIDirect3D9, bool isEx)
{
    (void)pIDirect3D9; (void)isEx;
    EnsureSubLockInit(); // ensure the submit lock exists before the overlay thread submits
}

void D3DOverlay_SetEnabled(bool enabled)        { g_enabled = enabled; }
void D3DOverlay_SetDrawAtPresent(bool atPresent) { (void)atPresent; }

void D3DOverlay_Submit(const void* pixelsBGRA, int width, int height, int corner, int padX, int padY)
{
    if (!g_subLockReady)
        return;
    EnterCriticalSection(&g_subLock);
    if (width <= 0 || height <= 0 || !pixelsBGRA)
    {
        g_subW = 0; g_subH = 0;
        g_subValid = true;
    }
    else
    {
        size_t bytes = (size_t)width * (size_t)height * 4;
        g_subPixels.resize(bytes);
        memcpy(g_subPixels.data(), pixelsBGRA, bytes);
        g_subW = width; g_subH = height;
        g_subCorner = corner; g_subPadX = padX; g_subPadY = padY;
        g_subValid = true;
    }
    LeaveCriticalSection(&g_subLock);
}

// Exported so the chainloaded overlay companion can fetch the latest overlay bitmap.
// Returns the byte size needed; if dest is large enough, copies the BGRA pixels into it.
extern "C" __declspec(dllexport) int McTDE_GetOverlayBitmap(
    void* dest, int destCap, int* outW, int* outH, int* outCorner, int* outPadX, int* outPadY)
{
    if (!g_subLockReady)
        return 0;
    EnterCriticalSection(&g_subLock);
    int w = g_subW, h = g_subH;
    int need = (w > 0 && h > 0) ? w * h * 4 : 0;
    if (outW) *outW = w;
    if (outH) *outH = h;
    if (outCorner) *outCorner = g_subCorner;
    if (outPadX) *outPadX = g_subPadX;
    if (outPadY) *outPadY = g_subPadY;
    if (dest && destCap >= need && need > 0)
        memcpy(dest, g_subPixels.data(), need);
    LeaveCriticalSection(&g_subLock);
    return need;
}

void D3DOverlay_Shutdown()
{
    g_enabled = false;
}
