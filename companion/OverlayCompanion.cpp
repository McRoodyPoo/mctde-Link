// OverlayCompanion.cpp
//
// Standalone overlay-render DLL, loaded from mctde-Link's chainload folder (the same slot
// the PTDE practice tool / hudhook runs from, which is proven to render in-frame alongside
// DSFix). It does NOT contain any game logic. Each frame it:
//   1. hooks the d3d9 device's Present (vtable swap on a throwaway device),
//   2. pulls the latest overlay bitmap from d3d9.dll's exported McTDE_GetOverlayBitmap,
//   3. draws it as one alpha-blended quad onto the render target bound at Present time.
//
// Diagnostics go to OutputDebugStringA (view with DebugView) prefixed "[mctde-companion]".

#include <windows.h>
#include <d3d9.h>
#include <vector>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")

static const int VT_RESET   = 16;
static const int VT_PRESENT = 17;
static const int VT_ENDSCENE = 42;

typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef int (*GetBitmap_t)(void*, int, int*, int*, int*, int*, int*);

static Present_t g_origPresent = NULL;
static Reset_t   g_origReset   = NULL;
static GetBitmap_t g_getBitmap = NULL;

static IDirect3DDevice9*     g_device = NULL;
static IDirect3DStateBlock9* g_sb = NULL;
static IDirect3DTexture9*    g_tex = NULL;
static int g_texW = 0, g_texH = 0;
static int g_drawW = 0, g_drawH = 0, g_drawCorner = 0, g_drawPadX = 0, g_drawPadY = 0;
static bool g_hasDraw = false;
static bool g_hooked = false;
static std::vector<BYTE> g_buf;

struct TLV { float x, y, z, rhw; DWORD color; float u, v; };
#define OVL_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static void Log(const char* s)
{
    OutputDebugStringA(s);
    // Also append to a log file next to this DLL so it can be read after a run.
    static char path[MAX_PATH] = "";
    if (!path[0])
    {
        HMODULE self = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&Log, &self);
        GetModuleFileNameA(self, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) lstrcpyA(slash + 1, "mctde_companion.log");
        else lstrcpyA(path, "mctde_companion.log");
    }
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD wr = 0;
        char line[320];
        int n = wsprintfA(line, "%s\r\n", s);
        WriteFile(h, line, (DWORD)n, &wr, NULL);
        CloseHandle(h);
    }
}

static int NextPow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }

static bool PatchVT(void* iface, int idx, void* hook, void** orig)
{
    if (!iface) return false;
    void** vt = *(void***)iface;
    DWORD op = 0;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &op)) return false;
    if (orig) *orig = vt[idx];
    vt[idx] = hook;
    DWORD ig = 0; VirtualProtect(&vt[idx], sizeof(void*), op, &ig);
    return true;
}

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
    if (!g_getBitmap) return;
    int w = 0, h = 0, corner = 0, px = 0, py = 0;
    int need = g_getBitmap(NULL, 0, &w, &h, &corner, &px, &py);
    if (need <= 0 || w <= 0 || h <= 0) { g_hasDraw = false; return; }
    if ((int)g_buf.size() < need) g_buf.resize(need);
    if (g_getBitmap(g_buf.data(), (int)g_buf.size(), &w, &h, &corner, &px, &py) != need) { g_hasDraw = false; return; }
    if (!EnsureTex(dev, w, h)) { g_hasDraw = false; return; }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(g_tex->LockRect(0, &lr, NULL, D3DLOCK_DISCARD)))
    {
        const BYTE* src = g_buf.data();
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
    g_sb->Capture();
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
    g_sb->Apply();

    static bool logged = false;
    if (!logged) { logged = true; char b[128]; wsprintfA(b, "[mctde-companion] first draw vp=%dx%d pos=%d,%d sz=%dx%d", sw, sh, x, y, w, h); Log(b); }
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* wrapper, const RECT* a, const RECT* b, HWND c, const RGNDATA* d)
{
    static bool logged = false;
    if (!logged) { logged = true; Log("[mctde-companion] Present fired"); }

    // DSFix does not wrap surfaces: the back buffer's owner is the REAL device. Draw
    // through it directly (bypassing DSFix's wrapper RT/draw interception) onto the real
    // back buffer that is about to be flipped.
    IDirect3DSurface9* bb = NULL;
    if (SUCCEEDED(wrapper->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb)
    {
        IDirect3DDevice9* real = NULL;
        if (SUCCEEDED(bb->GetDevice(&real)) && real)
        {
            if (real != g_device)
            {
                ReleaseObjs();
                g_device = real;
                char m[80]; wsprintfA(m, "[mctde-companion] real device=%p", real); Log(m);
            }
            if (!g_sb) real->CreateStateBlock(D3DSBT_ALL, &g_sb);
            PullAndUpload(real);

            IDirect3DSurface9* oldRT = NULL;
            real->GetRenderTarget(0, &oldRT);
            real->SetRenderTarget(0, bb);
            if (g_hasDraw && g_sb && SUCCEEDED(real->BeginScene()))
            {
                DrawQuad(real); // the overlay
                real->EndScene();
            }
            if (oldRT) { real->SetRenderTarget(0, oldRT); oldRT->Release(); }
            real->Release();
        }
        bb->Release();
    }
    return g_origPresent(wrapper, a, b, c, d);
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* wrapper, D3DPRESENT_PARAMETERS* pp)
{
    ReleaseObjs(); // our default-pool texture + stateblock must be freed before the reset
    return g_origReset(wrapper, pp);
}

static DWORD WINAPI InitThread(LPVOID)
{
    // Resolve the bitmap export from d3d9.dll (mctde-Link).
    for (int i = 0; i < 200 && !g_getBitmap; ++i)
    {
        HMODULE h = GetModuleHandleA("d3d9.dll");
        if (h) g_getBitmap = (GetBitmap_t)GetProcAddress(h, "McTDE_GetOverlayBitmap");
        if (!g_getBitmap) Sleep(50);
    }
    if (!g_getBitmap) { Log("[mctde-companion] McTDE_GetOverlayBitmap not found"); return 0; }

    // Throwaway device just to reach the d3d9 device vtable, then hook Present/Reset.
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { Log("[mctde-companion] Direct3DCreate9 failed"); return 0; }

    WNDCLASSEXA wc; ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "MCTDE_Companion_Dummy";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "x", WS_OVERLAPPED, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);

    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp, sizeof(pp));
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hwnd ? hwnd : GetDesktopWindow();

    IDirect3DDevice9* dummy = NULL;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, pp.hDeviceWindow,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dummy);
    if (FAILED(hr) || !dummy)
    {
        char m[64]; wsprintfA(m, "[mctde-companion] CreateDevice failed 0x%08X", (unsigned)hr); Log(m);
        if (hwnd) DestroyWindow(hwnd);
        d3d->Release();
        return 0;
    }

    bool p = PatchVT(dummy, VT_PRESENT, (void*)&Hook_Present, (void**)&g_origPresent);
    bool r = PatchVT(dummy, VT_RESET,   (void*)&Hook_Reset,   (void**)&g_origReset);

    dummy->Release();
    if (hwnd) DestroyWindow(hwnd);
    d3d->Release();

    if (p && r) { g_hooked = true; Log("[mctde-companion] Present/Reset hooked"); }
    else Log("[mctde-companion] vtable patch failed");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
