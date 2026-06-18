// D3DOverlay.cpp -- see D3DOverlay.h.
//
// In-frame Direct3D9 overlay, rendered from inside d3d9.dll itself (no separate companion DLL).
//
// Two halves:
//   1. Bitmap producer  -- the NetOverlay submit thread rasterizes the overlay to a 32-bit
//      BGRA bitmap and calls D3DOverlay_Submit() (only when [Render] Backend=d3d).
//   2. In-frame renderer -- our d3d9 proxy hooks IDirect3D9::CreateDevice (D3DOverlay_HookFactory)
//      and returns a WRAPPER device (MctdeDevice) instead of the raw device. DSFix then wraps our
//      wrapper, so structurally DSFix's frame composite happens BEFORE it calls down into our
//      wrapper's Present -- i.e. we are *inner* to DSFix and draw on top of its finished frame,
//      with no shared-vtable timing fight (the earlier approaches lost that race or deadlocked).
//      Phantom_Break never touches d3d9, so it is unaffected.
#include "pch.h"
#include <windows.h>
#include <d3d9.h>
#include <vector>
#include "D3DOverlay.h"

// WriteHubLog lives in dllmain.cpp (external linkage).
void WriteHubLog(const char* message);

// ------------------------------------------------------------
// Submitted-bitmap storage (producer side)
// ------------------------------------------------------------

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

// Exported for backward compatibility (a previously-shipped external companion fetched the
// bitmap through this). The in-process renderer below no longer needs it. Harmless to keep.
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

// ------------------------------------------------------------
// Drawing helpers (operate on the real device)
// ------------------------------------------------------------

static IDirect3DDevice9*     g_device = NULL; // the real device we are currently drawing through
static IDirect3DStateBlock9* g_sb     = NULL;
static IDirect3DTexture9*    g_tex    = NULL;
static int  g_texW = 0, g_texH = 0;
static int  g_drawW = 0, g_drawH = 0, g_drawCorner = 0, g_drawPadX = 0, g_drawPadY = 0;
static bool g_hasDraw = false;
static std::vector<BYTE> g_renderBuf;

struct TLV { float x, y, z, rhw; DWORD color; float u, v; };
#define OVL_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static int NextPow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }

static void ReleaseObjs()
{
    if (g_tex) { g_tex->Release(); g_tex = NULL; }
    if (g_sb)  { g_sb->Release();  g_sb = NULL; }
    g_texW = g_texH = 0;
    g_hasDraw = false;
}

static bool EnsureTex(IDirect3DDevice9* dev, int w, int h)
{
    int tw = NextPow2(w), th = NextPow2(h);
    if (g_tex && tw <= g_texW && th <= g_texH) return true;
    if (tw < g_texW) tw = g_texW;
    if (th < g_texH) th = g_texH;
    if (g_tex) { g_tex->Release(); g_tex = NULL; }
    if (FAILED(dev->CreateTexture((UINT)tw, (UINT)th, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                  D3DPOOL_DEFAULT, &g_tex, NULL)) || !g_tex)
    {
        g_tex = NULL; g_texW = g_texH = 0; return false;
    }
    g_texW = tw; g_texH = th;
    return true;
}

static void PullAndUpload(IDirect3DDevice9* dev)
{
    int w = 0, h = 0, corner = 0, px = 0, py = 0;
    {
        if (!g_subLockReady) { g_hasDraw = false; return; }
        EnterCriticalSection(&g_subLock);
        if (!g_subValid || g_subW <= 0 || g_subH <= 0)
        {
            LeaveCriticalSection(&g_subLock);
            g_hasDraw = false; return;
        }
        w = g_subW; h = g_subH; corner = g_subCorner; px = g_subPadX; py = g_subPadY;
        size_t bytes = (size_t)w * h * 4;
        if (g_renderBuf.size() < bytes) g_renderBuf.resize(bytes);
        memcpy(g_renderBuf.data(), g_subPixels.data(), bytes);
        LeaveCriticalSection(&g_subLock);
    }
    if (!EnsureTex(dev, w, h)) { g_hasDraw = false; return; }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(g_tex->LockRect(0, &lr, NULL, D3DLOCK_DISCARD)))
    {
        const BYTE* src = g_renderBuf.data();
        BYTE* dst = (BYTE*)lr.pBits;
        int rb = w * 4;
        for (int r = 0; r < h; ++r) memcpy(dst + (size_t)r * lr.Pitch, src + (size_t)r * rb, rb);
        g_tex->UnlockRect(0);
        g_drawW = w; g_drawH = h; g_drawCorner = corner; g_drawPadX = px; g_drawPadY = py;
        g_hasDraw = true;
    }
}

static void DrawQuad(IDirect3DDevice9* dev)
{
    if (!g_hasDraw || !g_tex || !g_sb || g_drawW <= 0) return;
    D3DVIEWPORT9 vp;
    if (FAILED(dev->GetViewport(&vp))) return;
    int sw = (int)vp.Width, sh = (int)vp.Height, w = g_drawW, h = g_drawH, x, y;
    switch (g_drawCorner)
    {
    case 1: x = sw - w - g_drawPadX; y = g_drawPadY;          break;
    case 2: x = g_drawPadX;          y = sh - h - g_drawPadY; break;
    case 3: x = sw - w - g_drawPadX; y = sh - h - g_drawPadY; break;
    default: x = g_drawPadX;         y = g_drawPadY;          break;
    }
    float u = (float)w / g_texW, v = (float)h / g_texH, fx = (float)x - 0.5f, fy = (float)y - 0.5f;
    TLV verts[4] = {
        { fx,     fy,     0, 1, 0xFFFFFFFF, 0, 0 },
        { fx + w, fy,     0, 1, 0xFFFFFFFF, u, 0 },
        { fx,     fy + h, 0, 1, 0xFFFFFFFF, 0, v },
        { fx + w, fy + h, 0, 1, 0xFFFFFFFF, u, v },
    };
    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);
    dev->SetFVF(OVL_FVF);
    dev->SetTexture(0, g_tex);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(TLV));
}

// Draw the overlay onto the real back buffer that is about to be flipped. Called from the
// wrapper's Present, after DSFix has composited its frame -- so the overlay lands on top.
static void DrawOverlayFrame(IDirect3DDevice9* dev)
{
    if (!g_enabled || !dev) return;
    if (dev->TestCooperativeLevel() != D3D_OK) return;

    if (dev != g_device) { ReleaseObjs(); g_device = dev; }
    if (!g_sb) dev->CreateStateBlock(D3DSBT_ALL, &g_sb);
    if (!g_sb) return;

    PullAndUpload(dev);
    if (!g_hasDraw) return; // nothing to draw this frame

    IDirect3DSurface9* bb = NULL;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    IDirect3DSurface9* oldRT = NULL;
    dev->GetRenderTarget(0, &oldRT);
    if (SUCCEEDED(dev->SetRenderTarget(0, bb)))
    {
        if (SUCCEEDED(dev->BeginScene()))
        {
            g_sb->Capture();
            DrawQuad(dev);
            g_sb->Apply();
            dev->EndScene();
        }
        if (oldRT) dev->SetRenderTarget(0, oldRT);
    }
    if (oldRT) oldRT->Release();
    bb->Release();
}

// ------------------------------------------------------------
// Device wrapper -- inner to DSFix by construction
// ------------------------------------------------------------

class MctdeDevice : public IDirect3DDevice9
{
public:
    MctdeDevice(IDirect3DDevice9* real) : m_real(real), m_ref(1) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        // Avoid linking dxguid: let the real device answer, then if it handed back its own
        // primary interface (IUnknown / IDirect3DDevice9), substitute our wrapper so the caller
        // keeps talking to us.
        HRESULT hr = m_real->QueryInterface(riid, ppv);
        if (SUCCEEDED(hr) && *ppv == (void*)m_real)
        {
            *ppv = static_cast<IDirect3DDevice9*>(this);
            AddRef();
            m_real->Release();
        }
        return hr;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0)
        {
            // Drop our render objects (state block / texture) while the real device is still
            // alive -- they were created from it and hold refs on it. Skipping this both leaks
            // the real device at teardown and leaves g_device/g_tex/g_sb dangling, which would
            // be a use-after-free if a new device is later created and presents.
            if (g_device == m_real) { ReleaseObjs(); g_device = NULL; }
            m_real->Release();
            delete this;
        }
        return (ULONG)r;
    }

    // The two methods we actually care about.
    STDMETHODIMP Reset(D3DPRESENT_PARAMETERS* pp) override
    {
        ReleaseObjs();
        if (g_device == m_real) g_device = NULL;
        return m_real->Reset(pp);
    }
    STDMETHODIMP Present(const RECT* a, const RECT* b, HWND c, const RGNDATA* d) override
    {
        DrawOverlayFrame(m_real);
        return m_real->Present(a, b, c, d);
    }

    // Everything else: straight pass-through to the real device.
    STDMETHODIMP TestCooperativeLevel() override { return m_real->TestCooperativeLevel(); }
    STDMETHODIMP_(UINT) GetAvailableTextureMem() override { return m_real->GetAvailableTextureMem(); }
    STDMETHODIMP EvictManagedResources() override { return m_real->EvictManagedResources(); }
    STDMETHODIMP GetDirect3D(IDirect3D9** ppD3D9) override { return m_real->GetDirect3D(ppD3D9); }
    STDMETHODIMP GetDeviceCaps(D3DCAPS9* p) override { return m_real->GetDeviceCaps(p); }
    STDMETHODIMP GetDisplayMode(UINT i, D3DDISPLAYMODE* p) override { return m_real->GetDisplayMode(i, p); }
    STDMETHODIMP GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) override { return m_real->GetCreationParameters(p); }
    STDMETHODIMP SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* s) override { return m_real->SetCursorProperties(x, y, s); }
    STDMETHODIMP_(void) SetCursorPosition(int x, int y, DWORD f) override { m_real->SetCursorPosition(x, y, f); }
    STDMETHODIMP_(BOOL) ShowCursor(BOOL b) override { return m_real->ShowCursor(b); }
    STDMETHODIMP CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** sc) override { return m_real->CreateAdditionalSwapChain(pp, sc); }
    STDMETHODIMP GetSwapChain(UINT i, IDirect3DSwapChain9** sc) override { return m_real->GetSwapChain(i, sc); }
    STDMETHODIMP_(UINT) GetNumberOfSwapChains() override { return m_real->GetNumberOfSwapChains(); }
    STDMETHODIMP GetBackBuffer(UINT i, UINT bi, D3DBACKBUFFER_TYPE t, IDirect3DSurface9** bb) override { return m_real->GetBackBuffer(i, bi, t, bb); }
    STDMETHODIMP GetRasterStatus(UINT i, D3DRASTER_STATUS* p) override { return m_real->GetRasterStatus(i, p); }
    STDMETHODIMP SetDialogBoxMode(BOOL b) override { return m_real->SetDialogBoxMode(b); }
    STDMETHODIMP_(void) SetGammaRamp(UINT i, DWORD f, const D3DGAMMARAMP* r) override { m_real->SetGammaRamp(i, f, r); }
    STDMETHODIMP_(void) GetGammaRamp(UINT i, D3DGAMMARAMP* r) override { m_real->GetGammaRamp(i, r); }
    STDMETHODIMP CreateTexture(UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* sh) override { return m_real->CreateTexture(w, h, l, u, f, p, t, sh); }
    STDMETHODIMP CreateVolumeTexture(UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** t, HANDLE* sh) override { return m_real->CreateVolumeTexture(w, h, d, l, u, f, p, t, sh); }
    STDMETHODIMP CreateCubeTexture(UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** t, HANDLE* sh) override { return m_real->CreateCubeTexture(e, l, u, f, p, t, sh); }
    STDMETHODIMP CreateVertexBuffer(UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** vb, HANDLE* sh) override { return m_real->CreateVertexBuffer(len, u, fvf, p, vb, sh); }
    STDMETHODIMP CreateIndexBuffer(UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** ib, HANDLE* sh) override { return m_real->CreateIndexBuffer(len, u, f, p, ib, sh); }
    STDMETHODIMP CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL lk, IDirect3DSurface9** s, HANDLE* sh) override { return m_real->CreateRenderTarget(w, h, f, m, q, lk, s, sh); }
    STDMETHODIMP CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL dc, IDirect3DSurface9** s, HANDLE* sh) override { return m_real->CreateDepthStencilSurface(w, h, f, m, q, dc, s, sh); }
    STDMETHODIMP UpdateSurface(IDirect3DSurface9* ss, const RECT* sr, IDirect3DSurface9* ds, const POINT* dp) override { return m_real->UpdateSurface(ss, sr, ds, dp); }
    STDMETHODIMP UpdateTexture(IDirect3DBaseTexture9* s, IDirect3DBaseTexture9* d) override { return m_real->UpdateTexture(s, d); }
    STDMETHODIMP GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* ds) override { return m_real->GetRenderTargetData(rt, ds); }
    STDMETHODIMP GetFrontBufferData(UINT i, IDirect3DSurface9* ds) override { return m_real->GetFrontBufferData(i, ds); }
    STDMETHODIMP StretchRect(IDirect3DSurface9* ss, const RECT* sr, IDirect3DSurface9* ds, const RECT* dr, D3DTEXTUREFILTERTYPE fi) override { return m_real->StretchRect(ss, sr, ds, dr, fi); }
    STDMETHODIMP ColorFill(IDirect3DSurface9* s, const RECT* r, D3DCOLOR c) override { return m_real->ColorFill(s, r, c); }
    STDMETHODIMP CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** s, HANDLE* sh) override { return m_real->CreateOffscreenPlainSurface(w, h, f, p, s, sh); }
    STDMETHODIMP SetRenderTarget(DWORD i, IDirect3DSurface9* rt) override { return m_real->SetRenderTarget(i, rt); }
    STDMETHODIMP GetRenderTarget(DWORD i, IDirect3DSurface9** rt) override { return m_real->GetRenderTarget(i, rt); }
    STDMETHODIMP SetDepthStencilSurface(IDirect3DSurface9* z) override { return m_real->SetDepthStencilSurface(z); }
    STDMETHODIMP GetDepthStencilSurface(IDirect3DSurface9** z) override { return m_real->GetDepthStencilSurface(z); }
    STDMETHODIMP BeginScene() override { return m_real->BeginScene(); }
    STDMETHODIMP EndScene() override { return m_real->EndScene(); }
    STDMETHODIMP Clear(DWORD c, const D3DRECT* r, DWORD f, D3DCOLOR col, float z, DWORD s) override { return m_real->Clear(c, r, f, col, z, s); }
    STDMETHODIMP SetTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) override { return m_real->SetTransform(s, m); }
    STDMETHODIMP GetTransform(D3DTRANSFORMSTATETYPE s, D3DMATRIX* m) override { return m_real->GetTransform(s, m); }
    STDMETHODIMP MultiplyTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) override { return m_real->MultiplyTransform(s, m); }
    STDMETHODIMP SetViewport(const D3DVIEWPORT9* v) override { return m_real->SetViewport(v); }
    STDMETHODIMP GetViewport(D3DVIEWPORT9* v) override { return m_real->GetViewport(v); }
    STDMETHODIMP SetMaterial(const D3DMATERIAL9* m) override { return m_real->SetMaterial(m); }
    STDMETHODIMP GetMaterial(D3DMATERIAL9* m) override { return m_real->GetMaterial(m); }
    STDMETHODIMP SetLight(DWORD i, const D3DLIGHT9* l) override { return m_real->SetLight(i, l); }
    STDMETHODIMP GetLight(DWORD i, D3DLIGHT9* l) override { return m_real->GetLight(i, l); }
    STDMETHODIMP LightEnable(DWORD i, BOOL e) override { return m_real->LightEnable(i, e); }
    STDMETHODIMP GetLightEnable(DWORD i, BOOL* e) override { return m_real->GetLightEnable(i, e); }
    STDMETHODIMP SetClipPlane(DWORD i, const float* p) override { return m_real->SetClipPlane(i, p); }
    STDMETHODIMP GetClipPlane(DWORD i, float* p) override { return m_real->GetClipPlane(i, p); }
    STDMETHODIMP SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override { return m_real->SetRenderState(s, v); }
    STDMETHODIMP GetRenderState(D3DRENDERSTATETYPE s, DWORD* v) override { return m_real->GetRenderState(s, v); }
    STDMETHODIMP CreateStateBlock(D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** sb) override { return m_real->CreateStateBlock(t, sb); }
    STDMETHODIMP BeginStateBlock() override { return m_real->BeginStateBlock(); }
    STDMETHODIMP EndStateBlock(IDirect3DStateBlock9** sb) override { return m_real->EndStateBlock(sb); }
    STDMETHODIMP SetClipStatus(const D3DCLIPSTATUS9* c) override { return m_real->SetClipStatus(c); }
    STDMETHODIMP GetClipStatus(D3DCLIPSTATUS9* c) override { return m_real->GetClipStatus(c); }
    STDMETHODIMP GetTexture(DWORD s, IDirect3DBaseTexture9** t) override { return m_real->GetTexture(s, t); }
    STDMETHODIMP SetTexture(DWORD s, IDirect3DBaseTexture9* t) override { return m_real->SetTexture(s, t); }
    STDMETHODIMP GetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD* v) override { return m_real->GetTextureStageState(s, t, v); }
    STDMETHODIMP SetTextureStageState(DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v) override { return m_real->SetTextureStageState(s, t, v); }
    STDMETHODIMP GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD* v) override { return m_real->GetSamplerState(s, t, v); }
    STDMETHODIMP SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) override { return m_real->SetSamplerState(s, t, v); }
    STDMETHODIMP ValidateDevice(DWORD* n) override { return m_real->ValidateDevice(n); }
    STDMETHODIMP SetPaletteEntries(UINT n, const PALETTEENTRY* e) override { return m_real->SetPaletteEntries(n, e); }
    STDMETHODIMP GetPaletteEntries(UINT n, PALETTEENTRY* e) override { return m_real->GetPaletteEntries(n, e); }
    STDMETHODIMP SetCurrentTexturePalette(UINT n) override { return m_real->SetCurrentTexturePalette(n); }
    STDMETHODIMP GetCurrentTexturePalette(UINT* n) override { return m_real->GetCurrentTexturePalette(n); }
    STDMETHODIMP SetScissorRect(const RECT* r) override { return m_real->SetScissorRect(r); }
    STDMETHODIMP GetScissorRect(RECT* r) override { return m_real->GetScissorRect(r); }
    STDMETHODIMP SetSoftwareVertexProcessing(BOOL b) override { return m_real->SetSoftwareVertexProcessing(b); }
    STDMETHODIMP_(BOOL) GetSoftwareVertexProcessing() override { return m_real->GetSoftwareVertexProcessing(); }
    STDMETHODIMP SetNPatchMode(float n) override { return m_real->SetNPatchMode(n); }
    STDMETHODIMP_(float) GetNPatchMode() override { return m_real->GetNPatchMode(); }
    STDMETHODIMP DrawPrimitive(D3DPRIMITIVETYPE p, UINT s, UINT c) override { return m_real->DrawPrimitive(p, s, c); }
    STDMETHODIMP DrawIndexedPrimitive(D3DPRIMITIVETYPE p, INT bv, UINT mv, UINT nv, UINT si, UINT pc) override { return m_real->DrawIndexedPrimitive(p, bv, mv, nv, si, pc); }
    STDMETHODIMP DrawPrimitiveUP(D3DPRIMITIVETYPE p, UINT c, const void* d, UINT st) override { return m_real->DrawPrimitiveUP(p, c, d, st); }
    STDMETHODIMP DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE p, UINT mv, UINT nv, UINT pc, const void* id, D3DFORMAT idf, const void* vd, UINT vst) override { return m_real->DrawIndexedPrimitiveUP(p, mv, nv, pc, id, idf, vd, vst); }
    STDMETHODIMP ProcessVertices(UINT s, UINT d, UINT c, IDirect3DVertexBuffer9* db, IDirect3DVertexDeclaration9* vd, DWORD f) override { return m_real->ProcessVertices(s, d, c, db, vd, f); }
    STDMETHODIMP CreateVertexDeclaration(const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** d) override { return m_real->CreateVertexDeclaration(e, d); }
    STDMETHODIMP SetVertexDeclaration(IDirect3DVertexDeclaration9* d) override { return m_real->SetVertexDeclaration(d); }
    STDMETHODIMP GetVertexDeclaration(IDirect3DVertexDeclaration9** d) override { return m_real->GetVertexDeclaration(d); }
    STDMETHODIMP SetFVF(DWORD f) override { return m_real->SetFVF(f); }
    STDMETHODIMP GetFVF(DWORD* f) override { return m_real->GetFVF(f); }
    STDMETHODIMP CreateVertexShader(const DWORD* fn, IDirect3DVertexShader9** s) override { return m_real->CreateVertexShader(fn, s); }
    STDMETHODIMP SetVertexShader(IDirect3DVertexShader9* s) override { return m_real->SetVertexShader(s); }
    STDMETHODIMP GetVertexShader(IDirect3DVertexShader9** s) override { return m_real->GetVertexShader(s); }
    STDMETHODIMP SetVertexShaderConstantF(UINT r, const float* d, UINT c) override { return m_real->SetVertexShaderConstantF(r, d, c); }
    STDMETHODIMP GetVertexShaderConstantF(UINT r, float* d, UINT c) override { return m_real->GetVertexShaderConstantF(r, d, c); }
    STDMETHODIMP SetVertexShaderConstantI(UINT r, const int* d, UINT c) override { return m_real->SetVertexShaderConstantI(r, d, c); }
    STDMETHODIMP GetVertexShaderConstantI(UINT r, int* d, UINT c) override { return m_real->GetVertexShaderConstantI(r, d, c); }
    STDMETHODIMP SetVertexShaderConstantB(UINT r, const BOOL* d, UINT c) override { return m_real->SetVertexShaderConstantB(r, d, c); }
    STDMETHODIMP GetVertexShaderConstantB(UINT r, BOOL* d, UINT c) override { return m_real->GetVertexShaderConstantB(r, d, c); }
    STDMETHODIMP SetStreamSource(UINT n, IDirect3DVertexBuffer9* sd, UINT o, UINT st) override { return m_real->SetStreamSource(n, sd, o, st); }
    STDMETHODIMP GetStreamSource(UINT n, IDirect3DVertexBuffer9** sd, UINT* o, UINT* st) override { return m_real->GetStreamSource(n, sd, o, st); }
    STDMETHODIMP SetStreamSourceFreq(UINT n, UINT s) override { return m_real->SetStreamSourceFreq(n, s); }
    STDMETHODIMP GetStreamSourceFreq(UINT n, UINT* s) override { return m_real->GetStreamSourceFreq(n, s); }
    STDMETHODIMP SetIndices(IDirect3DIndexBuffer9* i) override { return m_real->SetIndices(i); }
    STDMETHODIMP GetIndices(IDirect3DIndexBuffer9** i) override { return m_real->GetIndices(i); }
    STDMETHODIMP CreatePixelShader(const DWORD* fn, IDirect3DPixelShader9** s) override { return m_real->CreatePixelShader(fn, s); }
    STDMETHODIMP SetPixelShader(IDirect3DPixelShader9* s) override { return m_real->SetPixelShader(s); }
    STDMETHODIMP GetPixelShader(IDirect3DPixelShader9** s) override { return m_real->GetPixelShader(s); }
    STDMETHODIMP SetPixelShaderConstantF(UINT r, const float* d, UINT c) override { return m_real->SetPixelShaderConstantF(r, d, c); }
    STDMETHODIMP GetPixelShaderConstantF(UINT r, float* d, UINT c) override { return m_real->GetPixelShaderConstantF(r, d, c); }
    STDMETHODIMP SetPixelShaderConstantI(UINT r, const int* d, UINT c) override { return m_real->SetPixelShaderConstantI(r, d, c); }
    STDMETHODIMP GetPixelShaderConstantI(UINT r, int* d, UINT c) override { return m_real->GetPixelShaderConstantI(r, d, c); }
    STDMETHODIMP SetPixelShaderConstantB(UINT r, const BOOL* d, UINT c) override { return m_real->SetPixelShaderConstantB(r, d, c); }
    STDMETHODIMP GetPixelShaderConstantB(UINT r, BOOL* d, UINT c) override { return m_real->GetPixelShaderConstantB(r, d, c); }
    STDMETHODIMP DrawRectPatch(UINT h, const float* s, const D3DRECTPATCH_INFO* i) override { return m_real->DrawRectPatch(h, s, i); }
    STDMETHODIMP DrawTriPatch(UINT h, const float* s, const D3DTRIPATCH_INFO* i) override { return m_real->DrawTriPatch(h, s, i); }
    STDMETHODIMP DeletePatch(UINT h) override { return m_real->DeletePatch(h); }
    STDMETHODIMP CreateQuery(D3DQUERYTYPE t, IDirect3DQuery9** q) override { return m_real->CreateQuery(t, q); }

private:
    IDirect3DDevice9* m_real;
    volatile LONG     m_ref;
};

// ------------------------------------------------------------
// IDirect3D9::CreateDevice hook -- returns our wrapper to the caller (DSFix / game)
// ------------------------------------------------------------

static const int VT_D3D_CREATEDEVICE = 16; // IDirect3D9::CreateDevice

typedef HRESULT (STDMETHODCALLTYPE *CreateDevice_t)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateDevice_t g_origCreateDevice = NULL;

static bool PatchVTSlot(void* iface, int idx, void* hook, void** orig)
{
    if (!iface) return false;
    void** vt = *(void***)iface;
    if (vt[idx] == hook) return false;
    DWORD op = 0;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &op)) return false;
    if (orig) *orig = vt[idx];
    vt[idx] = hook;
    DWORD ig = 0; VirtualProtect(&vt[idx], sizeof(void*), op, &ig);
    return true;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(
    IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
    D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** ppDevice)
{
    HRESULT hr = g_origCreateDevice(self, adapter, type, focus, flags, pp, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice)
    {
        *ppDevice = new MctdeDevice(*ppDevice); // hand the caller (DSFix) our wrapper
        WriteHubLog("[overlay] CreateDevice wrapped (overlay is inner to DSFix).");
    }
    return hr;
}

// Called by the d3d9 proxy's Direct3DCreate9 with the real IDirect3D9 it returns to the game /
// DSFix. We hook CreateDevice so the device handed out is our wrapper. (CreateDeviceEx is left
// alone: Dark Souls PTDE uses the non-Ex path.)
void D3DOverlay_HookFactory(void* pIDirect3D9, bool isEx)
{
    (void)isEx;
    EnsureSubLockInit();
    if (!pIDirect3D9) return;
    PatchVTSlot((IDirect3D9*)pIDirect3D9, VT_D3D_CREATEDEVICE, (void*)&Hook_CreateDevice, (void**)&g_origCreateDevice);
}

// Kept for the HubThread call site; the wrapper is installed lazily at CreateDevice instead.
void D3DOverlay_InstallEarly(void* direct3DCreate9Fn) { (void)direct3DCreate9Fn; EnsureSubLockInit(); }

void D3DOverlay_SetEnabled(bool enabled)         { g_enabled = enabled; }
void D3DOverlay_SetDrawAtPresent(bool atPresent) { (void)atPresent; }

void D3DOverlay_Shutdown()
{
    g_enabled = false;
}
