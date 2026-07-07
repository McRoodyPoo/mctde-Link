// DSFont.cpp -- see DSFont.h. Renderer core validated byte-exact offline (BC3 decode vs texconv;
// glyph rects vs the atlas; mixed-script render Latin/Cyrillic/CJK/Hangul at 16/24/32px).
#include "DSFont.h"
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cmath>

namespace {

struct GlyphRec { uint16_t w, h; int16_t xoff, yoff, adv; uint32_t bmpOff; };

std::vector<uint8_t> g_blob;
std::unordered_map<uint32_t, GlyphRec> g_map;
int g_lineHeight = 0, g_base = 0, g_maxH = 0;
float g_scaleOverride = 0.0f;   // >0 = render at this exact scale (set from render resolution)
bool g_smooth = true;           // true = bilinear (matches the game's filtered UI font); false = point
bool g_ready = false;

inline uint32_t U32(const uint8_t* p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
inline uint16_t U16(const uint8_t* p){ return (uint16_t)(p[0]|(p[1]<<8)); }
inline int16_t  S16(const uint8_t* p){ return (int16_t)(p[0]|(p[1]<<8)); }

// Glyph coverage sample. Smooth (bilinear) matches the game's GPU-filtered UI font -- clean/sharp at
// a small upscale. Point sampling is available for a hard-pixel look, but at a fractional scale it
// doubles pixels unevenly and looks blocky, so smooth is the default.
inline int Sample(const uint8_t* a, int w, int h, float sx, float sy){
    if(!g_smooth){
        int x = (int)sx, y = (int)sy;
        if(x<0)x=0; if(y<0)y=0; if(x>=w)x=w-1; if(y>=h)y=h-1;
        return a[y*w+x];
    }
    sx -= 0.5f; sy -= 0.5f;
    int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
    float fx = sx - x0, fy = sy - y0;
    auto at = [&](int x, int y)->int{ if(x<0||y<0||x>=w||y>=h) return 0; return a[y*w+x]; };
    float top = at(x0,y0)*(1-fx) + at(x0+1,y0)*fx;
    float bot = at(x0,y0+1)*(1-fx) + at(x0+1,y0+1)*fx;
    return (int)(top*(1-fy) + bot*fy + 0.5f);
}

void BlitGlyph(uint32_t* bits, int W, int H, int stride, int px, int py,
               const GlyphRec& g, float scale, int r, int gg, int b){
    const uint8_t* a = &g_blob[g.bmpOff];
    int dw = (int)(g.w*scale + 0.5f), dh = (int)(g.h*scale + 0.5f);
    if(dw < 1 || dh < 1) return;
    int ox = (int)(g.xoff*scale + 0.5f), oy = (int)(g.yoff*scale + 0.5f);
    for(int dy=0; dy<dh; ++dy) for(int dx=0; dx<dw; ++dx){
        int cov = Sample(a, g.w, g.h, (dx+0.5f)/scale, (dy+0.5f)/scale);
        if(cov <= 0) continue;
        float al = cov / 255.0f;
        int X = px+ox+dx, Y = py+oy+dy;
        if(X<0 || Y<0 || X>=W || Y>=H) continue;
        uint32_t* d = &bits[(size_t)Y*stride + X];
        int dr=(*d>>16)&255, dgc=(*d>>8)&255, db=*d&255, da=(*d>>24)&255;
        // straight-alpha src-over (D3DOverlay blends SRCALPHA/INVSRCALPHA, i.e. non-premultiplied)
        float dA = da/255.0f;
        float outA = al + dA*(1.0f-al);
        int nr, ng, nb, na;
        if(outA <= 0.0001f){ nr=ng=nb=0; na=0; }
        else {
            nr=(int)((r*al + dr*dA*(1.0f-al))/outA + 0.5f);
            ng=(int)((gg*al + dgc*dA*(1.0f-al))/outA + 0.5f);
            nb=(int)((b*al + db*dA*(1.0f-al))/outA + 0.5f);
            na=(int)(outA*255.0f + 0.5f);
        }
        *d = ((uint32_t)na<<24)|((uint32_t)nr<<16)|((uint32_t)ng<<8)|(uint32_t)nb;
    }
}

// Decode next code point from UTF-8; advances i. Falls back to raw byte on malformed input.
uint32_t NextUTF8(const char* s, int len, int& i){
    unsigned char c = (unsigned char)s[i];
    if(c < 0x80){ i += 1; return c; }
    if((c>>5)==0x6 && i+1<len){ uint32_t cp=((c&0x1F)<<6)|((unsigned char)s[i+1]&0x3F); i+=2; return cp; }
    if((c>>4)==0xE && i+2<len){ uint32_t cp=((c&0x0F)<<12)|(((unsigned char)s[i+1]&0x3F)<<6)|((unsigned char)s[i+2]&0x3F); i+=3; return cp; }
    if((c>>3)==0x1E && i+3<len){ uint32_t cp=((c&0x07)<<18)|(((unsigned char)s[i+1]&0x3F)<<12)|(((unsigned char)s[i+2]&0x3F)<<6)|((unsigned char)s[i+3]&0x3F); i+=4; return cp; }
    i += 1; return c;
}

int IntScaleFor(int pxHeight){ if(g_maxH<=0) return 1; int s=(int)((float)pxHeight/g_maxH + 0.5f); return s<1?1:s; }
// Fractional scale is fine because sampling is nearest-neighbour (point). An explicit override lets
// the overlay render at the game's exact UI scale without integer-pixel rounding error.
float ScaleFor(int pxHeight){
    if(g_scaleOverride > 0.0f) return g_scaleOverride;
    return g_maxH > 0 ? (float)pxHeight / (float)g_maxH : 1.0f;
}

} // namespace

bool DSFont_Load(const wchar_t* path){
    if(g_ready) return true;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if(!GetFileSizeEx(h, &sz) || sz.QuadPart < 28 || sz.QuadPart >(LONGLONG)64*1024*1024){ CloseHandle(h); return false; }
    g_blob.resize((size_t)sz.QuadPart);
    size_t done = 0; DWORD rd = 0; bool ok = true;
    while(done < g_blob.size()){
        if(!ReadFile(h, g_blob.data()+done, (DWORD)(g_blob.size()-done), &rd, nullptr) || rd==0){ ok=false; break; }
        done += rd;
    }
    CloseHandle(h);
    if(!ok || memcmp(g_blob.data(), "MCTDEFNT", 8) != 0){ g_blob.clear(); return false; }
    g_lineHeight = U16(&g_blob[16]); g_base = U16(&g_blob[18]); g_maxH = U16(&g_blob[20]);
    uint32_t count = U32(&g_blob[24]); size_t p = 28;
    g_map.reserve(count);
    for(uint32_t k=0; k<count; ++k){
        if(p+16 > g_blob.size()){ g_map.clear(); g_blob.clear(); return false; }
        GlyphRec g; uint32_t code = U32(&g_blob[p]);
        g.w=U16(&g_blob[p+4]); g.h=U16(&g_blob[p+6]);
        g.xoff=S16(&g_blob[p+8]); g.yoff=S16(&g_blob[p+10]); g.adv=S16(&g_blob[p+12]);
        g.bmpOff=(uint32_t)(p+16);
        size_t next = p + 16 + (size_t)g.w*g.h;
        if(next > g_blob.size()){ g_map.clear(); g_blob.clear(); return false; }
        g_map[code] = g; p = next;
    }
    g_ready = true;
    return true;
}

bool DSFont_Ready(){ return g_ready; }
int  DSFont_CellHeight(){ return g_maxH; }
void DSFont_SetScale(float s){ g_scaleOverride = s; }
void DSFont_SetSmooth(bool s){ g_smooth = s; }
// The height the font will actually render at for a requested px (snapped to an integer scale),
// so the overlay can size its layout to match and keep everything crisp.
int  DSFont_SnapHeight(int pxHeight){ return IntScaleFor(pxHeight) * g_maxH; }

int DSFont_MeasureW(const wchar_t* s, int len, int pxHeight){
    if(!g_ready) return 0;
    float sc = ScaleFor(pxHeight); int w = 0;
    for(int i=0;i<len;++i){ auto it=g_map.find((uint32_t)s[i]); if(it!=g_map.end()) w += (int)(it->second.adv*sc+0.5f); }
    return w;
}
int DSFont_MeasureUTF8(const char* s, int len, int pxHeight){
    if(!g_ready) return 0;
    float sc = ScaleFor(pxHeight); int w = 0;
    for(int i=0;i<len;){ uint32_t cp=NextUTF8(s,len,i); auto it=g_map.find(cp); if(it!=g_map.end()) w += (int)(it->second.adv*sc+0.5f); }
    return w;
}

static void DrawRun(uint32_t* bits,int W,int H,int stride,int x,int y,
                    const uint32_t* cps,int nc,COLORREF fill,int pxHeight,
                    COLORREF outline,int outlineRadius){
    float sc = ScaleFor(pxHeight);
    int fr=GetRValue(fill), fg=GetGValue(fill), fb=GetBValue(fill);
    int orr=GetRValue(outline), og=GetGValue(outline), ob=GetBValue(outline);
    int rad = outlineRadius;
    // outline pass (ring), then fill pass -- both advance identically so glyphs line up
    for(int pass=0; pass<2; ++pass){
        if(pass==0 && rad<=0) continue;
        int cx = x;
        for(int k=0;k<nc;++k){
            auto it = g_map.find(cps[k]);
            if(it != g_map.end()){
                const GlyphRec& g = it->second;
                if(pass==0){
                    for(int ry=-rad; ry<=rad; ++ry) for(int rx=-rad; rx<=rad; ++rx){
                        if((rx==0&&ry==0) || rx*rx+ry*ry > rad*rad) continue;
                        BlitGlyph(bits,W,H,stride, cx+rx, y+ry, g, sc, orr,og,ob);
                    }
                } else {
                    BlitGlyph(bits,W,H,stride, cx, y, g, sc, fr,fg,fb);
                }
                cx += (int)(g.adv*sc + 0.5f);
            }
        }
    }
}

void DSFont_DrawW(uint32_t* bits,int W,int H,int stride,int x,int y,const wchar_t* s,int len,
                  COLORREF fill,int pxHeight,COLORREF outline,int outlineRadius){
    if(!g_ready || len<=0) return;
    std::vector<uint32_t> cps(len);
    for(int i=0;i<len;++i) cps[i]=(uint32_t)s[i];
    DrawRun(bits,W,H,stride,x,y,cps.data(),len,fill,pxHeight,outline,outlineRadius);
}
void DSFont_DrawUTF8(uint32_t* bits,int W,int H,int stride,int x,int y,const char* s,int len,
                     COLORREF fill,int pxHeight,COLORREF outline,int outlineRadius){
    if(!g_ready || len<=0) return;
    std::vector<uint32_t> cps; cps.reserve(len);
    for(int i=0;i<len;){ cps.push_back(NextUTF8(s,len,i)); }
    DrawRun(bits,W,H,stride,x,y,cps.data(),(int)cps.size(),fill,pxHeight,outline,outlineRadius);
}
