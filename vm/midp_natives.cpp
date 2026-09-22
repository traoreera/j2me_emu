// midp_natives.cpp
// Native APIs MIDP/CLDC : midlet, lcdui (Display/Canvas/GameCanvas/Graphics/
// Font/Image). S'appuie sur le HAL display/input. Les classes natives sont
// enregistrées dans le Runtime ; leurs méthodes routent vers ces handlers.
//
// Rendu : les Graphics dessinent soit sur un buffer ARGB (Image), soit sur
// l'écran RGB565 (hal) pour le paint système, soit sur un buffer RGB565
// partagé par les GameCanvas (double buffering) qui est présenté par
// flushGraphics().

#include "midp.h"
#include "native.h"
#include "interpreter.h"
#include "../hal/display.h"
#include "../hal/input.h"
#include "../hal/jar_reader.h"
#include "../hal/png.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace jvm
{
const std::vector<Obj *> &jme_threads();
void jme_threadForget(Obj *r);
bool jme_threadResume(Obj *r, Interpreter *interp, ClassInfo *cls);

namespace midp
{

namespace
{

Runtime *g_rt = nullptr;
Interpreter *g_interp = nullptr;

Obj *g_midlet = nullptr;
Obj *g_display = nullptr;
Obj *g_current = nullptr;
bool g_paintRequested = false;
bool g_destroyed = false;

std::vector<std::pair<std::string, std::string>> g_appProps;

Obj *g_screenGfx = nullptr;
Obj *g_fontCache[4][3] = {};

// Buffer RGB565 hors-heap pour les GameCanvas, dimensionné à la résolution
// écran effective (cf. init(), appelé après hal::display_init). Allocation
// unique au démarrage -- pas de (re)allocation en cours de partie.
static uint16_t *g_canvas565 = nullptr;

uint32_t g_keyStates = 0; // masque MIDP getKeyStates()
int g_tickN = 0;
int jme_tickCount() { return g_tickN; }

inline int screenW() { auto *fb = hal::display_get_framebuffer(); return fb ? fb->width : 240; }
inline int screenH() { auto *fb = hal::display_get_framebuffer(); return fb ? fb->height : 320; }
} // namespace

// ---------------------------------------------------------------------
// Accès simples aux args (position des paramètres dans args[])
// ---------------------------------------------------------------------

namespace
{

Obj *argRef(NativeContext *ctx, int i)
{
    return (i >= 0 && i < ctx->nargs) ? ctx->args[i].o : nullptr;
}

int32_t argInt(NativeContext *ctx, int i)
{
    return (i >= 0 && i < ctx->nargs) ? ctx->args[i].i : 0;
}

void setInt(NativeContext *ctx, int32_t v)
{
    if (ctx->result) *ctx->result = Value::fromInt(v);
}

void setLong(NativeContext *ctx, int64_t v)
{
    if (ctx->result) *ctx->result = Value::fromLong(v);
}

void setRef(NativeContext *ctx, Obj *o)
{
    if (ctx->result) *ctx->result = Value::fromRef(o);
}

Obj *makeInstance(const char *internalName)
{
    ClassInfo *c = g_rt ? g_rt->lookupNativeClass(internalName) : nullptr;
    return c ? g_rt->heap().newInstance(c) : nullptr;
}

bool isSubclassOf(Obj *o, const char *internalName)
{
    if (!o || o->kind != ObjKind::Instance) return false;
    ClassInfo *c = o->cls;
    while (c)
    {
        if (c->name == internalName) return true;
        c = c->super;
    }
    return false;
}

uint16_t argb565(uint32_t c)
{
    return static_cast<uint16_t>((((c >> 16) & 0xFF) >> 3) << 11) |
           static_cast<uint16_t>((((c >> 8) & 0xFF) >> 2) << 5) |
           static_cast<uint16_t>((c & 0xFF) >> 3);
}

// ---------------------------------------------------------------------
// Cibles de dessin (Graphics)
// ---------------------------------------------------------------------

enum
{
    GM_IMAGE_ARGB = 0,
    GM_SCREEN_565 = 1,
    GM_CANVAS_565 = 2,
};

enum
{
    G_COLOR = 0, G_FONT = 1, G_TX = 2, G_TY = 3,
    G_CLIPX = 4, G_CLIPY = 5, G_CLIPW = 6, G_CLIPH = 7,
    G_MODE = 8, G_TW = 9, G_TH = 10, G_STRIDE = 11, G_BUF = 12,
};

enum { GC_FULLSCREEN = 0, GC_GFX = 1 };
enum { IMG_W = 0, IMG_H = 1, IMG_MUT = 2, IMG_BUF = 3, IMG_GFX = 4 };
enum { F_FACE = 0, F_STYLE = 1, F_SIZE = 2 };

Obj *makeGraphics(uint32_t mode, Obj *buf, int w, int h, int stride)
{
    Obj *g = makeInstance("javax/microedition/lcdui/Graphics");
    if (!g) return nullptr;
    g->cells[G_COLOR] = Value::fromInt(0);
    g->cells[G_FONT] = Value::fromRef(nullptr);
    g->cells[G_TX] = Value::fromInt(0);
    g->cells[G_TY] = Value::fromInt(0);
    g->cells[G_MODE] = Value::fromInt(static_cast<int32_t>(mode));
    g->cells[G_TW] = Value::fromInt(w);
    g->cells[G_TH] = Value::fromInt(h);
    g->cells[G_STRIDE] = Value::fromInt(stride);
    g->cells[G_BUF] = Value::fromRef(buf);
    g->cells[G_CLIPX] = Value::fromInt(0);
    g->cells[G_CLIPY] = Value::fromInt(0);
    g->cells[G_CLIPW] = Value::fromInt(w);
    g->cells[G_CLIPH] = Value::fromInt(h);
    return g;
}

Obj *screenGraphics()
{
    if (g_screenGfx)
    {
        g_screenGfx->cells[G_CLIPW] = Value::fromInt(screenW());
        g_screenGfx->cells[G_CLIPH] = Value::fromInt(screenH());
        return g_screenGfx;
    }
    g_screenGfx = makeGraphics(GM_SCREEN_565, nullptr, screenW(), screenH(), screenW());
    return g_screenGfx;
}

Obj *canvasGfx(Obj *gc)
{
    if (gc && gc->cells[GC_GFX].o)
        return gc->cells[GC_GFX].o;
    Obj *g = makeGraphics(GM_CANVAS_565, nullptr, screenW(), screenH(), screenW());
    if (gc) gc->cells[GC_GFX] = Value::fromRef(g);
    return g;
}

// Gwrape le target RGB565 pour écrire un pixel en tenant compte du clip.
class Pix
{
public:
    Pix(Obj *g)
    {
        color = static_cast<uint32_t>(g->cells[G_COLOR].u) | 0xFF000000u;
        tx = g->cells[G_TX].i; ty = g->cells[G_TY].i;
        clipX = g->cells[G_CLIPX].i; clipY = g->cells[G_CLIPY].i;
        clipW = g->cells[G_CLIPW].i; clipH = g->cells[G_CLIPH].i;
        tw = g->cells[G_TW].i; th = g->cells[G_TH].i;
        stride = g->cells[G_STRIDE].i;
        mode = g->cells[G_MODE].i;
        buf = g->cells[G_BUF].o;
        u565 = nullptr;
        if (mode == GM_CANVAS_565)
            u565 = g_canvas565;
    }

    void put(int x, int y, uint32_t argb)
    {
        int rx = x - tx, ry = y - ty;
        if (rx < clipX || ry < clipY) return;
        if (rx >= clipX + clipW || ry >= clipY + clipH) return;
        if (rx < 0 || ry < 0 || rx >= tw || ry >= th) return;

        uint32_t srcA = (argb >> 24) & 0xFF;
        if (mode == GM_IMAGE_ARGB)
        {
            if (!buf) return;
            int idx = ry * stride + rx;
            if (srcA == 0xFF || !(buf->cells[idx].u & 0xFF000000))
            {
                buf->cells[idx].u = argb;
                return;
            }
            uint32_t dst = buf->cells[idx].u;
            uint32_t da = (dst >> 24) & 0xFF;
            uint32_t a = srcA + (da * (255 - srcA)) / 255;
            if (a == 0) { buf->cells[idx].u = 0; return; }
            uint32_t sa = (srcA * 255) / a;
            uint32_t sr = ((argb >> 16) & 0xFF) * sa, sg = ((argb >> 8) & 0xFF) * sa, sb = (argb & 0xFF) * sa;
            uint32_t dr = ((dst >> 16) & 0xFF) * (255 - sa), dg = ((dst >> 8) & 0xFF) * (255 - sa), db = (dst & 0xFF) * (255 - sa);
            buf->cells[idx].u = (a << 24) | (((sr + dr) / 255) << 16) | (((sg + dg) / 255) << 8) | ((sb + db) / 255);
            return;
        }

        uint16_t *dst = (mode == GM_SCREEN_565) ? (hal::display_get_framebuffer() ? hal::display_get_framebuffer()->pixels : nullptr) : u565;
        if (!dst) return;
        int idx = ry * stride + rx;
        if (srcA == 0)
            return;
        if (srcA == 0xFF)
        {
            dst[idx] = argb565(argb);
            return;
        }
        uint16_t cur = dst[idx];
        uint32_t r = ((cur >> 11) & 0x1F) * 255 / 31;
        uint32_t gg = ((cur >> 5) & 0x3F) * 255 / 63;
        uint32_t b = (cur & 0x1F) * 255 / 31;
        dst[idx] = static_cast<uint16_t>((((((argb >> 16) & 0xFF) * srcA + r * (255 - srcA)) / 255) >> 3) << 11) |
                   static_cast<uint16_t>((((((argb >> 8) & 0xFF) * srcA + gg * (255 - srcA)) / 255) >> 2) << 5) |
                   static_cast<uint16_t>((((argb & 0xFF) * srcA + b * (255 - srcA)) / 255) >> 3);
    }

    uint32_t color;
    int tx, ty, clipX, clipY, clipW, clipH;
    int tw, th, stride;
    uint32_t mode;
    Obj *buf;
    uint16_t *u565;
};

void hline(Pix &p, int x0, int x1, int y, uint32_t c)
{
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) p.put(x, y, c);
}

void vline(Pix &p, int x, int y0, int y1, uint32_t c)
{
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) p.put(x, y, c);
}

// Font 5x7 (même table que hal/display.cpp).
static const uint8_t kFont5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x4F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x14,0x14,0x00,0x00},
    {0x00,0x40,0x34,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08},
};

int strWidth(const char *s)
{
    int w = 0;
    while (s && *s) { w += 6; s++; }
    return w;
}

// ---------------------------------------------------------------------
// Graphics natives
// ---------------------------------------------------------------------

void g_setColorI(NativeContext *ctx)
{
    uint32_t rgb = static_cast<uint32_t>(argInt(ctx, 1)) & 0xFFFFFF;
    ctx->thisObj->cells[G_COLOR] = Value::fromInt(static_cast<int32_t>(0xFF000000 | rgb));
}
void g_setColorRGB(NativeContext *ctx)
{
    uint32_t r = static_cast<uint32_t>(argInt(ctx, 1)) & 0xFF;
    uint32_t g2 = static_cast<uint32_t>(argInt(ctx, 2)) & 0xFF;
    uint32_t b = static_cast<uint32_t>(argInt(ctx, 3)) & 0xFF;
    ctx->thisObj->cells[G_COLOR] = Value::fromInt(static_cast<int32_t>(0xFF000000 | (r << 16) | (g2 << 8) | b));
}
void g_getColor(NativeContext *ctx)
{
    setInt(ctx, static_cast<int32_t>(static_cast<uint32_t>(ctx->thisObj->cells[G_COLOR].u) & 0xFFFFFF));
}
void g_setGray(NativeContext *ctx)
{
    uint32_t v = static_cast<uint32_t>(argInt(ctx, 1)) & 0xFF;
    ctx->thisObj->cells[G_COLOR] = Value::fromInt(static_cast<int32_t>(0xFF000000 | (v << 16) | (v << 8) | v));
}

void g_fillRect(NativeContext *ctx)
{
    Pix p(ctx->thisObj);
    int x = argInt(ctx, 1), y = argInt(ctx, 2), w = argInt(ctx, 3), h = argInt(ctx, 4);
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "gfx fillRect x=%d y=%d w=%d h=%d mode=%d\n", x, y, w, h, (int)ctx->thisObj->cells[G_MODE].i);
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            p.put(x + xx, y + yy, p.color);
}
void g_drawRect(NativeContext *ctx)
{
    Pix p(ctx->thisObj);
    int x = argInt(ctx, 1), y = argInt(ctx, 2), w = argInt(ctx, 3), h = argInt(ctx, 4);
    hline(p, x, x + w - 1, y, p.color);
    hline(p, x, x + w - 1, y + h - 1, p.color);
    vline(p, x, y, y + h - 1, p.color);
    vline(p, x + w - 1, y, y + h - 1, p.color);
}
void g_drawLine(NativeContext *ctx)
{
    Pix p(ctx->thisObj);
    int x0 = argInt(ctx, 1), y0 = argInt(ctx, 2), x1 = argInt(ctx, 3), y1 = argInt(ctx, 4);
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;)
    {
        p.put(x0, y0, p.color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}
void g_fillTriangle(NativeContext *ctx)
{
    Pix p(ctx->thisObj);
    int x1 = argInt(ctx, 1), y1 = argInt(ctx, 2), x2 = argInt(ctx, 3), y2 = argInt(ctx, 4), x3 = argInt(ctx, 5), y3 = argInt(ctx, 6);
    int minX = x1, maxX = x1;
    if (x2 < minX) minX = x2; if (x2 > maxX) maxX = x2;
    if (x3 < minX) minX = x3; if (x3 > maxX) maxX = x3;
    int minY = y1, maxY = y1;
    if (y2 < minY) minY = y2; if (y2 > maxY) maxY = y2;
    if (y3 < minY) minY = y3; if (y3 > maxY) maxY = y3;
    auto area2 = [&](int ax, int ay, int bx, int by, int cx, int cy) {
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    };
    int a2 = area2(x1, y1, x2, y2, x3, y3);
    if (a2 == 0) return;
    for (int yy = minY; yy <= maxY; yy++)
        for (int xx = minX; xx <= maxX; xx++)
        {
            int w1 = area2(x2, y2, x3, y3, xx, yy);
            int w2 = area2(x3, y3, x1, y1, xx, yy);
            int w3 = area2(x1, y1, x2, y2, xx, yy);
            bool inside = (a2 > 0) ? (w1 >= 0 && w2 >= 0 && w3 >= 0) : (w1 <= 0 && w2 <= 0 && w3 <= 0);
            if (inside) p.put(xx, yy, p.color);
        }
}
void g_drawArc(NativeContext *ctx)
{
    Pix p(ctx->thisObj);
    int x = argInt(ctx, 1), y = argInt(ctx, 2), w = argInt(ctx, 3), h = argInt(ctx, 4);
    int a0 = argInt(ctx, 5), a1 = argInt(ctx, 6);
    float cx = x + w / 2.0f, cy = y + h / 2.0f;
    float rx = w / 2.0f, ry = h / 2.0f;
    constexpr float Pi180 = 3.14159265f / 180.0f;
    for (int deg = a0; deg <= a0 + a1; deg++)
    {
        float rad = deg * Pi180;
        int px = static_cast<int>(cx + std::cos(rad) * rx);
        int py = static_cast<int>(cy + std::sin(rad) * ry);
        p.put(px, py, p.color);
    }
}
void g_fillArc(NativeContext *ctx)
{
    Pix p(ctx->thisObj);
    int x = argInt(ctx, 1), y = argInt(ctx, 2), w = argInt(ctx, 3), h = argInt(ctx, 4);
    int a0 = argInt(ctx, 5), a1 = argInt(ctx, 6);
    float cx = x + w / 2.0f, cy = y + h / 2.0f;
    float rx = w / 2.0f, ry = h / 2.0f;
    constexpr float Pi180 = 3.14159265f / 180.0f;
    if (a1 >= 360)
    {
        for (int yy = y; yy <= y + h; yy++)
            for (int xx = x; xx <= x + w; xx++)
            {
                float nx = (xx - cx) / rx, ny = (yy - cy) / ry;
                if (nx * nx + ny * ny <= 1.0f)
                    p.put(xx, yy, p.color);
            }
        return;
    }
    for (int deg = a0; deg <= a0 + a1; deg++)
    {
        float rad = deg * Pi180;
        float dx = std::cos(rad) * rx, dy = std::sin(rad) * ry;
        hline(p, static_cast<int>(cx), static_cast<int>(cx + dx), static_cast<int>(cy + dy), p.color);
    }
}
void g_fillRoundRect(NativeContext *ctx)
{
    Pix p(ctx->thisObj);
    int x = argInt(ctx, 1), y = argInt(ctx, 2), w = argInt(ctx, 3), h = argInt(ctx, 4);
    int aw = argInt(ctx, 5), ah = argInt(ctx, 6);
    (void)aw; (void)ah;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            p.put(xx, yy, p.color);
}
void g_drawRoundRect(NativeContext *ctx)
{
    Pix p(ctx->thisObj);
    int x = argInt(ctx, 1), y = argInt(ctx, 2), w = argInt(ctx, 3), h = argInt(ctx, 4);
    int aw = argInt(ctx, 5), ah = argInt(ctx, 6);
    (void)aw; (void)ah;
    hline(p, x, x + w - 1, y, p.color);
    hline(p, x, x + w - 1, y + h - 1, p.color);
    vline(p, x, y, y + h - 1, p.color);
    vline(p, x + w - 1, y, y + h - 1, p.color);
}

void g_setFont(NativeContext *ctx)
{
    ctx->thisObj->cells[G_FONT] = Value::fromRef(argRef(ctx, 1));
}
void g_getFont(NativeContext *ctx)
{
    Obj *f = ctx->thisObj->cells[G_FONT].o;
    if (!f)
    {
        f = g_fontCache[0][1];
        if (!f)
        {
            f = makeInstance("javax/microedition/lcdui/Font");
            if (f)
            {
                f->cells[F_FACE] = Value::fromInt(0);
                f->cells[F_STYLE] = Value::fromInt(0);
                f->cells[F_SIZE] = Value::fromInt(0);
                g_fontCache[0][1] = f;
            }
        }
    }
    setRef(ctx, f);
}
void g_drawString(NativeContext *ctx)
{
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "gfx drawString(this=%p)\n", (void *)ctx->thisObj);
    Pix p(ctx->thisObj);
    Obj *s = argRef(ctx, 1);
    int x = argInt(ctx, 2), y = argInt(ctx, 3), anchor = argInt(ctx, 4);
    const char *text = (s && s->kind == ObjKind::String) ? s->str.c_str() : "";
    int wlen = strWidth(text);
    if (anchor & 0x01) x -= wlen / 2;      // HCENTER
    else if (anchor & 0x08) x -= wlen;     // RIGHT
    if (anchor & 0x02) y -= 3;             // VCENTER
    else if (anchor & 0x20) y -= 6;        // BASELINE
    else if (anchor & 0x40) y -= 7;        // BOTTOM
    while (*text)
    {
        unsigned char ch = static_cast<unsigned char>(*text);
        if (ch < 0x20 || ch > 0x7F) ch = '?';
        const uint8_t *glyph = kFont5x7[ch - 0x20];
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (glyph[col] & (1 << row))
                    p.put(x + col, y + row, p.color);
        x += 6;
        text++;
    }
}
void g_drawChar(NativeContext *ctx)
{
    char buf[2] = { static_cast<char>(argInt(ctx, 1)), 0 };
    ctx->args[1] = Value::fromRef(g_rt->heap().newString(buf));
    ctx->nargs = 5;
    g_drawString(ctx);
}
void g_drawChars(NativeContext *ctx)
{
    Obj *ca = argRef(ctx, 1);
    int off = argInt(ctx, 2), len = argInt(ctx, 3);
    int x = argInt(ctx, 4), y = argInt(ctx, 5), anchor = argInt(ctx, 6);
    if (!ca || off < 0) return;
    std::string t;
    for (int i = 0; i < len; i++)
        if (off + i < ca->arrayLen)
            t += static_cast<char>(ca->cells[off + i].u & 0xFF);
    ctx->args[1] = Value::fromRef(g_rt->heap().newString(t));
    ctx->args[2] = Value::fromInt(x);
    ctx->args[3] = Value::fromInt(y);
    ctx->args[4] = Value::fromInt(anchor);
    ctx->nargs = 5;
    g_drawString(ctx);
}
void g_drawImage(NativeContext *ctx)
{
    Obj *img = argRef(ctx, 1);
    int x = argInt(ctx, 2), y = argInt(ctx, 3), anchor = argInt(ctx, 4);
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "gfx drawImage img=%p kind=%d x=%d y=%d anc=%d\n",
                (void *)img, img ? (int)img->kind : -1, x, y, anchor);
    if (!img || img->kind != ObjKind::Instance) return;
    int iw = img->cells[IMG_W].i, ih = img->cells[IMG_H].i;
    Obj *buf = img->cells[IMG_BUF].o;
    if (!buf) return;
    int ox = 0, oy = 0;
    if (anchor & 0x01) ox = iw / 2;
    else if (anchor & 0x08) ox = iw;
    if (anchor & 0x02) oy = ih / 2;
    else if (anchor & 0x08) oy = ih;
    else if (anchor & 0x40) oy = ih;
    Pix p(ctx->thisObj);
    for (int yy = 0; yy < ih; yy++)
        for (int xx = 0; xx < iw; xx++)
            p.put(x - ox + xx, y - oy + yy, buf->cells[yy * iw + xx].u);
}

void g_drawRegion(NativeContext *ctx)
{
    Obj *img = argRef(ctx, 1);
    int xs = argInt(ctx, 2), ys = argInt(ctx, 3);
    int w = argInt(ctx, 4), h = argInt(ctx, 5);
    int tfm = argInt(ctx, 6);
    int x = argInt(ctx, 7), y = argInt(ctx, 8), anchor = argInt(ctx, 9);
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "gfx drawRegion img=%p src=(%d,%d %dx%d) tfm=%d dst=(%d,%d) anc=%d\n",
                (void *)img, xs, ys, w, h, tfm, x, y, anchor);
    if (!img || img->kind != ObjKind::Instance) return;
    int iw = img->cells[IMG_W].i, ih = img->cells[IMG_H].i;
    Obj *buf = img->cells[IMG_BUF].o;
    if (!buf) return;
    // Région ramenée dans les bornes de la source (CLDC : découpe au clip source).
    if (xs < 0) { w += xs; xs = 0; }
    if (ys < 0) { h += ys; ys = 0; }
    if (w > iw - xs) w = iw - xs;
    if (h > ih - ys) h = ih - ys;
    if (w <= 0 || h <= 0) return;

    // Dimensions du bloc destination : les rotations échangent largeur/hauteur.
    bool swap = (tfm == 5 || tfm == 6 || tfm == 7 || tfm == 4); // ROT90/270, MIRROR_ROT*
    int Wp = swap ? h : w, Hp = swap ? w : h;

    int ox = 0, oy = 0;
    if (anchor & 0x01) ox = Wp / 2;
    else if (anchor & 0x08) ox = Wp;
    if (anchor & 0x02) oy = Hp / 2;
    else if (anchor & 0x20) oy = Hp;
    else if (anchor & 0x40) oy = Hp;

    Pix p(ctx->thisObj);
    for (int dy = 0; dy < Hp; dy++)
    {
        for (int dx = 0; dx < Wp; dx++)
        {
            int sx, sy;
            switch (tfm)
            {
                case 3: sx = w - 1 - dx; sy = h - 1 - dy; break;            // ROT180
                case 2: sx = w - 1 - dx; sy = dy; break;                    // MIRROR
                case 1: sx = dx; sy = h - 1 - dy; break;                    // MIRROR_ROT180
                case 4: sx = dy; sy = dx; break;                            // MIRROR_ROT270
                case 5: sx = dy; sy = w - 1 - dx; break;                    // ROT90
                case 6: sx = h - 1 - dy; sy = dx; break;                    // ROT270
                case 7: sx = w - 1 - dy; sy = h - 1 - dx; break;            // MIRROR_ROT90
                default: sx = dx; sy = dy; break;                           // NONE / inconnu
            }
            p.put(x - ox + dx, y - oy + dy, buf->cells[(ys + sy) * iw + (xs + sx)].u);
        }
    }
}

void g_drawRGB(NativeContext *ctx)
{
    Obj *rgb = argRef(ctx, 1);
    int off = argInt(ctx, 2), scan = argInt(ctx, 3);
    int x = argInt(ctx, 4), y = argInt(ctx, 5), w = argInt(ctx, 6), h = argInt(ctx, 7);
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "gfx drawRGB rgb=%p x=%d y=%d w=%d h=%d sample=0x%08x\n",
                (void*)rgb, x, y, w, h,
                (rgb && rgb->kind == ObjKind::IntArray && rgb->arrayLen > 0) ? rgb->cells[0].u : 0);
    if (!rgb || rgb->kind != ObjKind::IntArray || w <= 0 || h <= 0) return;
    Pix p(ctx->thisObj);
    int idx = off;
    if (scan <= 0) scan = w;
    for (int j = 0; j < h; j++)
    {
        int row = idx;
        for (int i = 0; i < w; i++)
            p.put(x + i, y + j, static_cast<uint32_t>(rgb->cells[row + i].u));
        idx += scan;
    }
}
void g_setClipXYWH(NativeContext *ctx)
{
    Obj *g = ctx->thisObj;
    g->cells[G_CLIPX] = Value::fromInt(argInt(ctx, 1));
    g->cells[G_CLIPY] = Value::fromInt(argInt(ctx, 2));
    g->cells[G_CLIPW] = Value::fromInt(argInt(ctx, 3));
    g->cells[G_CLIPH] = Value::fromInt(argInt(ctx, 4));
}
void g_clipRect(NativeContext *ctx)
{
    Obj *g = ctx->thisObj;
    int cx = g->cells[G_CLIPX].i, cy = g->cells[G_CLIPY].i, cw = g->cells[G_CLIPW].i, chh = g->cells[G_CLIPH].i;
    int nx = cx + argInt(ctx, 1), ny = cy + argInt(ctx, 2);
    int nw = argInt(ctx, 3), nh = argInt(ctx, 4);
    int x0 = nx > cx ? nx : cx, y0 = ny > cy ? ny : cy;
    int x1 = (nx + nw) < (cx + cw) ? (nx + nw) : (cx + cw);
    int y1 = (ny + nh) < (cy + chh) ? (ny + nh) : (cy + chh);
    g->cells[G_CLIPX] = Value::fromInt(x0);
    g->cells[G_CLIPY] = Value::fromInt(y0);
    g->cells[G_CLIPW] = Value::fromInt(x1 - x0 > 0 ? x1 - x0 : 0);
    g->cells[G_CLIPH] = Value::fromInt(y1 - y0 > 0 ? y1 - y0 : 0);
}
void g_getClipX(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_CLIPX].i); }
void g_getClipY(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_CLIPY].i); }
void g_getClipW(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_CLIPW].i); }
void g_getClipH(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_CLIPH].i); }
void g_translate(NativeContext *ctx)
{
    Obj *g = ctx->thisObj;
    g->cells[G_TX] = Value::fromInt(g->cells[G_TX].i + argInt(ctx, 1));
    g->cells[G_TY] = Value::fromInt(g->cells[G_TY].i + argInt(ctx, 2));
}
void g_getTranslateX(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_TX].i); }
void g_getTranslateY(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_TY].i); }

// ---------------------------------------------------------------------
// Font natives
// ---------------------------------------------------------------------

void f_getFont(NativeContext *ctx)
{
    int face = argInt(ctx, 0), style = argInt(ctx, 1), size = argInt(ctx, 2);
    int si = size == 8 ? 0 : (size == 16 ? 2 : 1);
    int st = style & 0x03;
    Obj *f = g_fontCache[st][si];
    if (!f)
    {
        f = makeInstance("javax/microedition/lcdui/Font");
        if (!f) { setRef(ctx, nullptr); return; }
        f->cells[F_FACE] = Value::fromInt(face);
        f->cells[F_STYLE] = Value::fromInt(style);
        f->cells[F_SIZE] = Value::fromInt(size);
        g_fontCache[st][si] = f;
    }
    setRef(ctx, f);
}
void f_getHeight(NativeContext *ctx) { (void)ctx; setInt(ctx, 7); }
void f_getBaseline(NativeContext *ctx) { (void)ctx; setInt(ctx, 6); }
void f_getFace(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[F_FACE].i); }
void f_getStyle(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[F_STYLE].i); }
void f_getSize(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[F_SIZE].i); }
void f_stringWidth(NativeContext *ctx)
{
    const std::string &s = (argRef(ctx, 1) && argRef(ctx, 1)->kind == ObjKind::String) ? argRef(ctx, 1)->str : *new std::string("");
    setInt(ctx, strWidth(s.c_str()));
}
void f_charWidth(NativeContext *ctx) { (void)ctx; setInt(ctx, 6); }
void f_charsWidth(NativeContext *ctx) { (void)ctx; setInt(ctx, 6 * argInt(ctx, 3)); }

// ---------------------------------------------------------------------
// Image natives
// ---------------------------------------------------------------------

Obj *decodePng(const uint8_t *data, size_t len); // défini plus bas, décode en Image immuable

Obj *makeImage(int w, int h, bool mutable_, Obj *buf)
{
    Obj *img = makeInstance("javax/microedition/lcdui/Image");
    if (!img) return nullptr;
    img->cells[IMG_W] = Value::fromInt(w);
    img->cells[IMG_H] = Value::fromInt(h);
    img->cells[IMG_MUT] = Value::fromInt(mutable_ ? 1 : 0);
    img->cells[IMG_BUF] = Value::fromRef(buf);
    img->cells[IMG_GFX] = Value::fromRef(nullptr);
    return img;
}

void img_createWH(NativeContext *ctx)
{
    int w = argInt(ctx, 0), h = argInt(ctx, 1);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    Obj *buf = g_rt->heap().newArray(ObjKind::IntArray, w * h);
    if (!buf) { g_rt->reportOom(); setRef(ctx, nullptr); return; }
    for (int i = 0; i < w * h; i++) buf->cells[i].u = 0x00000000;
    setRef(ctx, makeImage(w, h, true, buf));
}
void img_createRGB(NativeContext *ctx)
{
    Obj *rgb = argRef(ctx, 0);
    int w = argInt(ctx, 1), h = argInt(ctx, 2);
    bool processAlpha = argInt(ctx, 3) != 0;
    if (w < 1) w = 1; if (h < 1) h = 1;
    Obj *buf = g_rt->heap().newArray(ObjKind::IntArray, w * h);
    if (!buf) { g_rt->reportOom(); setRef(ctx, nullptr); return; }
    for (int i = 0; i < w * h; i++)
    {
        uint32_t c = (rgb && i < rgb->arrayLen) ? static_cast<uint32_t>(rgb->cells[i].u) : 0;
        if (!processAlpha) c |= 0xFF000000;
        buf->cells[i].u = c;
    }
    setRef(ctx, makeImage(w, h, true, buf));
}
void img_copy(NativeContext *ctx)
{
    Obj *src = argRef(ctx, 0);
    if (!src || src->kind != ObjKind::Instance) { setRef(ctx, nullptr); return; }
    int w = src->cells[IMG_W].i, h = src->cells[IMG_H].i;
    Obj *buf = g_rt->heap().newArray(ObjKind::IntArray, w * h);
    if (!buf) { g_rt->reportOom(); setRef(ctx, nullptr); return; }
    Obj *sb = src->cells[IMG_BUF].o;
    for (int i = 0; i < w * h; i++)
        buf->cells[i].u = sb ? sb->cells[i].u : 0;
    setRef(ctx, makeImage(w, h, true, buf));
}
void img_createString(NativeContext *ctx)
{
    std::string path = (argRef(ctx, 0) && argRef(ctx, 0)->kind == ObjKind::String) ? argRef(ctx, 0)->str : "";
    while (!path.empty() && path[0] == '/')
        path.erase(0, 1);
    jme::JarReader *jar = ctx->rt->jar();
    jme::JarEntry e;
    if (jar && !path.empty() && jar->findEntry(path, e) && e.uncompressedSize <= 4u * 1024 * 1024)
    {
        uint8_t *tmp = static_cast<uint8_t *>(std::malloc(e.uncompressedSize ? e.uncompressedSize : 1));
        if (tmp)
        {
            size_t n = jar->extractEntry(path, tmp, e.uncompressedSize);
            Obj *img = (n > 0) ? decodePng(tmp, n) : nullptr;
            std::free(tmp);
            if (img)
            {
                if (getenv("JME_DEBUG"))
                    fprintf(stderr, "[midp] createImage(\"%s\") : decode OK (%zu octets)\n", path.c_str(), n);
                setRef(ctx, img);
                return;
            }
        }
    }
    fprintf(stderr, "[midp] createImage(\"%s\") : introuvable ou décodage échoué\n", path.c_str());
    Obj *buf = g_rt->heap().newArray(ObjKind::IntArray, 1);
    if (buf) buf->cells[0].u = 0xFFFFFFFF;
    setRef(ctx, makeImage(1, 1, false, buf));
}
void img_getGraphics(NativeContext *ctx)
{
    Obj *img = ctx->thisObj;
    if (img->cells[IMG_GFX].o)
    {
        setRef(ctx, img->cells[IMG_GFX].o);
        return;
    }
    Obj *buf = img->cells[IMG_BUF].o;
    int w = img->cells[IMG_W].i, h = img->cells[IMG_H].i;
    Obj *g = makeGraphics(GM_IMAGE_ARGB, buf, w, h, w);
    if (g) img->cells[IMG_GFX] = Value::fromRef(g);
    setRef(ctx, g);
}
void img_getWidth(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[IMG_W].i); }
void img_getHeight(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[IMG_H].i); }
void img_isMutable(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[IMG_MUT].i); }
void img_getRGB(NativeContext *ctx)
{
    Obj *img = ctx->thisObj;
    Obj *dst = argRef(ctx, 1);
    int off = argInt(ctx, 2), scan = argInt(ctx, 3);
    int sx = argInt(ctx, 4), sy = argInt(ctx, 5);
    int w = argInt(ctx, 6), h = argInt(ctx, 7);
    if (!dst) return;
    Obj *buf = img->cells[IMG_BUF].o;
    int iw = img->cells[IMG_W].i, ih = img->cells[IMG_H].i;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
        {
            int si = (sy + yy) * iw + (sx + xx);
            int di = off + yy * scan + xx;
            if (di >= 0 && di < dst->arrayLen)
                dst->cells[di].u = (buf && si >= 0 && si < iw * ih) ? buf->cells[si].u : 0;
        }
}

// ---------------------------------------------------------------------
// Canvas / GameCanvas natives
// ---------------------------------------------------------------------

void gc_init(NativeContext *ctx)
{
    if (ctx->thisObj)
        ctx->thisObj->cells[GC_FULLSCREEN] = Value::fromInt(argInt(ctx, 1));
}
void gc_setFullScreen(NativeContext *ctx)
{
    if (ctx->thisObj)
        ctx->thisObj->cells[GC_FULLSCREEN] = Value::fromInt(argInt(ctx, 1));
}
void gc_getGraphics(NativeContext *ctx)
{
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "gfx getGraphics(this=%p)\n", (void *)ctx->thisObj);
    setRef(ctx, canvasGfx(ctx->thisObj));
}
void gc_flushGraphics(NativeContext *ctx)
{
    if (!ctx->thisObj) return;
    auto *fb = hal::display_get_framebuffer();
    if (fb && fb->pixels)
        if (getenv("JME_DEBUG"))
            fprintf(stderr, "flushGraphics()\n");
        std::memcpy(fb->pixels, g_canvas565, static_cast<size_t>(screenH()) * fb->stride * sizeof(uint16_t));
    hal::display_present(fb);
}
void gc_flushRegion(NativeContext *ctx)
{
    gc_flushGraphics(ctx);
}
void gc_getKeyStates(NativeContext *ctx)
{
    (void)ctx;
    setInt(ctx, static_cast<int32_t>(g_keyStates));
}

void cv_getWidth(NativeContext *ctx) { (void)ctx; setInt(ctx, screenW()); }
void cv_getHeight(NativeContext *ctx) { (void)ctx; setInt(ctx, screenH()); }
void cv_isDoubleBuffered(NativeContext *ctx) { (void)ctx; setInt(ctx, 1); }
void cv_repaint(NativeContext *ctx) { (void)ctx; g_paintRequested = true; }
void cv_repaintRegion(NativeContext *ctx) { (void)ctx; g_paintRequested = true; }
void cv_service(NativeContext *ctx) { (void)ctx; }
void cv_showNotify(NativeContext *ctx) { (void)ctx; }
void cv_hideNotify(NativeContext *ctx) { (void)ctx; }

// Constantes standard MIDP Canvas.{UP,DOWN,LEFT,RIGHT,FIRE,GAME_A..D}. Notre
// HAL (halKeyToMidp, plus bas) envoie déjà directement ces valeurs comme
// "keyCode" pour le D-pad/FIRE -- getGameAction est donc quasi l'identité
// pour ces touches, plus le mapping des touches numériques 1/3/7/9 vers les
// GAME_A..D (convention MIDP courante sur clavier téléphone).
int cv_gameActionFor(int keyCode)
{
    switch (keyCode)
    {
    case 1: case 2: case 5: case 6: case 8: return keyCode; // UP/LEFT/RIGHT/DOWN/FIRE
    case '1': return 9;  // GAME_A
    case '3': return 10; // GAME_B
    case '7': return 11; // GAME_C
    case '9': return 12; // GAME_D
    default: return 0;
    }
}
void cv_getGameAction(NativeContext *ctx) { setInt(ctx, cv_gameActionFor(argInt(ctx, 1))); }
void cv_getKeyCode(NativeContext *ctx)
{
    switch (argInt(ctx, 1))
    {
    case 1: case 2: case 5: case 6: case 8: setInt(ctx, argInt(ctx, 1)); return;
    case 9: setInt(ctx, '1'); return;
    case 10: setInt(ctx, '3'); return;
    case 11: setInt(ctx, '7'); return;
    case 12: setInt(ctx, '9'); return;
    default: setInt(ctx, 0); return;
    }
}

// ---------------------------------------------------------------------
// Display natives
// ---------------------------------------------------------------------

void d_getDisplay(NativeContext *ctx)
{
    if (!g_display) g_display = makeInstance("javax/microedition/lcdui/Display");
    setRef(ctx, g_display);
}
void d_getCurrent(NativeContext *ctx)
{
    setRef(ctx, g_current);
}
void d_setCurrent(NativeContext *ctx)
{
    Obj *d = argRef(ctx, 1);
    if (d && d->kind == ObjKind::Instance)
    {
        if (getenv("JME_DEBUG"))
            fprintf(stderr, "DISPLAY.setCurrent(%s)\n", d->cls ? d->cls->name.c_str() : "?");
        if (g_current != d)
        {
            g_current = d;
            g_paintRequested = true;
            for (int i = 0; i < screenW() * screenH(); i++) g_canvas565[i] = 0;
        }
    }
}
void d_setCurrentAlert(NativeContext *ctx)
{
    Obj *alert = argRef(ctx, 1);
    Obj *next = argRef(ctx, 2);
    g_current = (alert && alert->kind == ObjKind::Instance) ? alert : next;
    g_paintRequested = true;
}
void d_isColor(NativeContext *ctx) { (void)ctx; setInt(ctx, 1); }
void d_numColors(NativeContext *ctx) { (void)ctx; setInt(ctx, 65536); }
void d_numAlpha(NativeContext *ctx) { (void)ctx; setInt(ctx, 256); }
void d_flashBacklight(NativeContext *ctx) { (void)ctx; }
void d_repaint(NativeContext *ctx) { (void)ctx; g_paintRequested = true; }
void d_callSerially(NativeContext *ctx) { (void)ctx; }
void d_setCurrentItem(NativeContext *ctx) { (void)ctx; }

// ---------------------------------------------------------------------
// MIDlet natives
// ---------------------------------------------------------------------

void mid_init(NativeContext *ctx)
{
    g_midlet = ctx->thisObj;
}
void mid_getAppProperty(NativeContext *ctx)
{
    const std::string &key = (argRef(ctx, 1) && argRef(ctx, 1)->kind == ObjKind::String) ? argRef(ctx, 1)->str : "";
    std::string val;
    for (const auto &kv : g_appProps)
        if (kv.first == key) { val = kv.second; break; }
    setRef(ctx, g_rt->heap().newString(val));
}
void mid_notifyDestroyed(NativeContext *ctx)
{
    (void)ctx;
    g_destroyed = true;
}
void mid_notifyPaused(NativeContext *ctx) { (void)ctx; }
void mid_resumeRequest(NativeContext *ctx) { (void)ctx; setInt(ctx, 0); }

// ---------------------------------------------------------------------
// Stubs pour les UI ponctuelles
// ---------------------------------------------------------------------

void ui_noop(NativeContext *ctx) { (void)ctx; }
void ui_true(NativeContext *ctx) { (void)ctx; setInt(ctx, 1); }

// ---------------------------------------------------------------------
// I/O natives : flux octets (array-backed), getResourceAsStream, PNG.
// Layout d'un InputStream (cells entiers) : 0 = données [B, 1 = pos,
// 2 = limite. ByteArrayInputStream partage ce layout ; DataInputStream
// garde son flux sous-jacent en cells[0].
// ---------------------------------------------------------------------

Obj *makeStream(Obj *bytes, int limit)
{
    Obj *s = makeInstance("java/io/InputStream");
    if (!s) return nullptr;
    s->cells[0] = Value::fromRef(bytes);
    s->cells[1] = Value::fromInt(0);
    s->cells[2] = Value::fromInt(limit);
    return s;
}

int64_t argLongL(NativeContext *ctx, int i)
{
    return (i >= 0 && i < ctx->nargs) ? ctx->args[i].l : 0;
}

int32_t streamByte(Obj *s)
{
    if (!s || s->cellCount < 3) return -1;
    Obj *data = s->cells[0].o;
    int pos = s->cells[1].i, lim = s->cells[2].i;
    if (!data || pos >= lim) return -1;
    s->cells[1] = Value::fromInt(pos + 1);
    return static_cast<int32_t>(data->cells[pos].u & 0xFF);
}

int32_t streamFill(Obj *s, Obj *dst, int off, int len)
{
    if (!s || !dst || dst->kind != ObjKind::ByteArray) return -1;
    if (off < 0 || len < 0 || off + len > dst->arrayLen) return -1;
    Obj *data = s->cells[0].o;
    int pos = s->cells[1].i, lim = s->cells[2].i;
    int n = lim - pos;
    if (n > len) n = len;
    if (n < 0) n = 0;
    if (data) for (int i = 0; i < n; i++)
        dst->cells[off + i].u = data->cells[pos + i].u;
    s->cells[1] = Value::fromInt(pos + n);
    return n;
}

void is_read(NativeContext *ctx) { setInt(ctx, streamByte(ctx->thisObj)); }
void is_readArr(NativeContext *ctx)
{
    Obj *d = argRef(ctx, 1);
    setInt(ctx, streamFill(ctx->thisObj, d, 0, d ? d->arrayLen : 0));
}
void is_readArrII(NativeContext *ctx)
{
    setInt(ctx, streamFill(ctx->thisObj, argRef(ctx, 1), argInt(ctx, 2), argInt(ctx, 3)));
}
void is_available(NativeContext *ctx)
{
    Obj *s = ctx->thisObj;
    setInt(ctx, (s && s->cellCount >= 3) ? (s->cells[2].i - s->cells[1].i) : 0);
}
void is_skip(NativeContext *ctx)
{
    Obj *s = ctx->thisObj;
    if (!s || s->cellCount < 3) { setLong(ctx, 0); return; }
    int64_t n = argLongL(ctx, 1);
    int32_t avail = s->cells[2].i - s->cells[1].i;
    int64_t k = (n > avail) ? avail : n;
    if (k < 0) k = 0;
    s->cells[1] = Value::fromInt(s->cells[1].i + static_cast<int32_t>(k));
    setLong(ctx, k);
}
void is_close(NativeContext *ctx) { (void)ctx; }
void is_markSupported(NativeContext *ctx) { setInt(ctx, 0); }
void bais_markSupported(NativeContext *ctx) { setInt(ctx, 1); }

void bais_init(NativeContext *ctx)
{
    Obj *bytes = argRef(ctx, 1);
    if (!bytes) { setRef(ctx, nullptr); return; }
    int n = bytes->arrayLen;
    ctx->thisObj->cells[0] = Value::fromRef(bytes);
    ctx->thisObj->cells[1] = Value::fromInt(0);
    ctx->thisObj->cells[2] = Value::fromInt(n);
}
void bais_mark(NativeContext *ctx) { (void)ctx; }
void bais_reset(NativeContext *ctx) { if (ctx->thisObj) ctx->thisObj->cells[1] = Value::fromInt(0); }

void dis_init(NativeContext *ctx) { ctx->thisObj->cells[0] = Value::fromRef(argRef(ctx, 1)); }

int32_t diByte(Obj *self) { Obj *in = self && self->cellCount >= 1 ? self->cells[0].o : nullptr; return in ? streamByte(in) : -1; }
int32_t diFill(Obj *self, Obj *dst, int off, int len) { Obj *in = self && self->cellCount >= 1 ? self->cells[0].o : nullptr; return in ? streamFill(in, dst, off, len) : -1; }

void di_read(NativeContext *ctx) { setInt(ctx, diByte(ctx->thisObj)); }
void di_readArr(NativeContext *ctx) { Obj *d = argRef(ctx, 1); setInt(ctx, diFill(ctx->thisObj, d, 0, d ? d->arrayLen : 0)); }
void di_readArrII(NativeContext *ctx) { setInt(ctx, diFill(ctx->thisObj, argRef(ctx, 1), argInt(ctx, 2), argInt(ctx, 3))); }
void di_readBoolean(NativeContext *ctx) { setInt(ctx, diByte(ctx->thisObj) != 0 ? 1 : 0); }
void di_readByte(NativeContext *ctx) { setInt(ctx, static_cast<int8_t>(diByte(ctx->thisObj))); }
void di_readUnsignedByte(NativeContext *ctx) { setInt(ctx, diByte(ctx->thisObj)); }

int64_t diReadN(Obj *self, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
    {
        int b = diByte(self);
        if (b < 0) return -1;
        v = (v << 8) | static_cast<uint64_t>(b);
    }
    return static_cast<int64_t>(v);
}
void di_readShort(NativeContext *ctx) { setInt(ctx, static_cast<int16_t>(diReadN(ctx->thisObj, 2))); }
void di_readUnsignedShort(NativeContext *ctx) { setInt(ctx, static_cast<int32_t>(diReadN(ctx->thisObj, 2) & 0xFFFF)); }
void di_readChar(NativeContext *ctx) { setInt(ctx, static_cast<int32_t>(diReadN(ctx->thisObj, 2) & 0xFFFF)); }
void di_readInt(NativeContext *ctx) { setInt(ctx, static_cast<int32_t>(diReadN(ctx->thisObj, 4))); }
void di_readLong(NativeContext *ctx) { setLong(ctx, diReadN(ctx->thisObj, 8)); }
void di_readFully(NativeContext *ctx) { Obj *d = argRef(ctx, 1); if (d) diFill(ctx->thisObj, d, 0, d->arrayLen); }
void di_readFullyII(NativeContext *ctx) { diFill(ctx->thisObj, argRef(ctx, 1), argInt(ctx, 2), argInt(ctx, 3)); }

void di_readUTF(NativeContext *ctx)
{
    int lenH = diByte(ctx->thisObj), lenL = diByte(ctx->thisObj);
    if (lenH < 0 || lenL < 0) { setRef(ctx, g_rt->heap().newString("")); return; }
    int len = (lenH << 8) | lenL;
    std::string out;
    out.reserve(static_cast<size_t>(len) * 2);
    for (int i = 0; i < len;)
    {
        int b = diByte(ctx->thisObj);
        if (b < 0) break;
        if (b == 0xC0)
        {
            int b2 = diByte(ctx->thisObj);
            if (b2 < 0) break;
            if (b == 0xC0 && b2 == 0x80)
                out += '\0';
            else
            {
                out += static_cast<char>(b);
                out += static_cast<char>(b2);
            }
            i += 2;
        }
        else if (b < 0x80)
        {
            out += static_cast<char>(b);
            i += 1;
        }
        else
        {
            int need = (b < 0xE0) ? 2 : 3;
            std::string seq;
            seq += static_cast<char>(b);
            int ok = 1;
            for (int k = 1; k < need; k++)
            {
                int b2 = diByte(ctx->thisObj);
                if (b2 < 0) { ok = 0; break; }
                seq += static_cast<char>(b2);
            }
            if (ok) out += seq;
            i += need;
        }
    }
    setRef(ctx, g_rt->heap().newString(out));
}

void di_skipBytes(NativeContext *ctx)
{
    Obj *in = ctx->thisObj && ctx->thisObj->cellCount >= 1 ? ctx->thisObj->cells[0].o : nullptr;
    if (!in || in->cellCount < 3) { setInt(ctx, 0); return; }
    int n = argInt(ctx, 1);
    int32_t avail = in->cells[2].i - in->cells[1].i;
    if (n > avail) n = avail;
    if (n < 0) n = 0;
    in->cells[1] = Value::fromInt(in->cells[1].i + n);
    setInt(ctx, n);
}

// --- Récupération de ressources depuis le JAR ---

void cl_getResourceAsStream(NativeContext *ctx)
{
    Obj *name = argRef(ctx, 1);
    std::string path = (name && name->kind == ObjKind::String) ? name->str : "";
    while (!path.empty() && path[0] == '/')
        path.erase(0, 1);
    jme::JarReader *jar = ctx->rt->jar();
    if (!jar || path.empty())
    {
        setRef(ctx, nullptr);
        return;
    }
    jme::JarEntry e;
    if (!jar->findEntry(path, e) || e.uncompressedSize > 4u * 1024 * 1024)
    {
        if (getenv("JME_DEBUG"))
            fprintf(stderr, "[midp] res introuvable: \"%s\"\n", path.c_str());
        setRef(ctx, nullptr);
        return;
    }
    uint8_t *tmp = static_cast<uint8_t *>(std::malloc(e.uncompressedSize ? e.uncompressedSize : 1));
    if (!tmp) { setRef(ctx, nullptr); return; }
    size_t n = jar->extractEntry(path, tmp, e.uncompressedSize);
    if (n == 0) { std::free(tmp); setRef(ctx, nullptr); return; }
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "[midp] res \"%s\" : %zu octets\n", path.c_str(), n);
    Obj *bytes = g_rt->heap().newArray(ObjKind::ByteArray, static_cast<int32_t>(n));
    if (!bytes)
    {
        std::free(tmp);
        g_rt->reportOom();
        setRef(ctx, nullptr);
        return;
    }
    for (size_t i = 0; i < n; i++)
        bytes->cells[i].u = tmp[i];
    std::free(tmp);
    setRef(ctx, makeStream(bytes, static_cast<int>(n)));
}

// --- décodage PNG vers une Image immuable ---

Obj *decodePng(const uint8_t *data, size_t len)
{
    jme::PngHeader hdr;
    if (!jme::pngHeader(data, len, hdr) || hdr.idatLen + hdr.rawLen > 8u * 1024 * 1024)
        return nullptr;
    size_t wlen = hdr.idatLen + hdr.rawLen;
    uint8_t *work = static_cast<uint8_t *>(std::malloc(wlen));
    if (!work) return nullptr;
    int32_t *pxbuf = static_cast<int32_t *>(std::malloc(static_cast<size_t>(hdr.w) * hdr.h * 4));
    if (!pxbuf) { std::free(work); return nullptr; }
    bool ok = jme::pngPixels(data, len, hdr, work, wlen, pxbuf);
    std::free(work);
    if (!ok) { std::free(pxbuf); return nullptr; }
    Obj *px = g_rt->heap().newArray(ObjKind::IntArray, static_cast<int32_t>(hdr.w) * hdr.h);
    if (!px) { std::free(pxbuf); return nullptr; }
    for (int i = 0; i < hdr.w * hdr.h; i++)
        px->cells[i].u = static_cast<uint32_t>(pxbuf[i]);
    std::free(pxbuf);
    return makeImage(hdr.w, hdr.h, false, px);
}

void img_createStream(NativeContext *ctx)
{
    Obj *src = argRef(ctx, 0);
    if (!src || src->cellCount < 3 || !src->cells[0].o)
    {
        setRef(ctx, nullptr);
        return;
    }
    Obj *data = src->cells[0].o;
    int pos = src->cells[1].i, lim = src->cells[2].i;
    if (data->kind != ObjKind::ByteArray || pos < 0 || lim > data->arrayLen || pos >= lim)
    {
        setRef(ctx, nullptr);
        return;
    }
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "[midp] Image.createImage(InputStream, %d octets)\n", lim - pos);
    uint8_t *buf = static_cast<uint8_t *>(std::malloc(size_t(lim - pos)));
    if (!buf) { setRef(ctx, nullptr); return; }
    for (int i = pos; i < lim; i++)
        buf[i - pos] = static_cast<uint8_t>(data->cells[i].u);
    Obj *img = decodePng(buf, size_t(lim - pos));
    std::free(buf);
    setRef(ctx, img);
}

void img_createBytes(NativeContext *ctx)
{
    Obj *data = argRef(ctx, 0);
    int off = argInt(ctx, 1), len = argInt(ctx, 2);
    if (!data || data->kind != ObjKind::ByteArray || off < 0 || len < 0 || off + len > data->arrayLen)
    {
        setRef(ctx, nullptr);
        return;
    }
    uint8_t *buf = static_cast<uint8_t *>(std::malloc(size_t(len ? len : 1)));
    if (!buf) { setRef(ctx, nullptr); return; }
    for (int i = 0; i < len; i++)
        buf[i] = static_cast<uint8_t>(data->cells[off + i].u);
    Obj *img = decodePng(buf, size_t(len));
    std::free(buf);
    setRef(ctx, img);
}

// ---------------------------------------------------------------------
// Enregistrement des classes natives
// ---------------------------------------------------------------------

struct N { const char *name; const char *desc; NativeFn fn; };

void mp_createPlayerIS(NativeContext *ctx)
{
    Obj *p = makeInstance("javax/microedition/media/Player");
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "[midp] Manager.createPlayer(InputStream, type) -> %p (stub)\n", (void *)p);
    setRef(ctx, p);
}
void mp_createPlayerStr(NativeContext *ctx)
{
    Obj *p = makeInstance("javax/microedition/media/Player");
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "[midp] Manager.createPlayer(locator=%s) -> %p (stub)\n",
                (argRef(ctx, 0) && argRef(ctx, 0)->kind == ObjKind::String) ? argRef(ctx, 0)->str.c_str() : "",
                (void *)p);
    setRef(ctx, p);
}
void mp_noop(NativeContext *ctx) { (void)ctx; }
void mp_getInt(NativeContext *ctx) { setInt(ctx, 400); }
void mp_getLong(NativeContext *ctx) { setLong(ctx, 1000); }
void mp_getContentType(NativeContext *ctx)
{
    setRef(ctx, g_rt->heap().newString(""));
}

void regClass(Runtime *rt, const char *name, const char *super,
              std::initializer_list<std::pair<const char *, const char *>> methods,
              std::initializer_list<std::pair<const char *, const char *>> fields)
{
    std::vector<std::pair<std::string, std::string>> m;
    for (auto &kv : methods) m.push_back({kv.first, kv.second});
    std::vector<std::pair<std::string, std::string>> f;
    for (auto &kv : fields) f.push_back({kv.first, kv.second});
    rt->registerNativeClass(name, super ? super : "", m, f);
}

void regN(const char *key, NativeFn fn)
{
    registerNative(key, fn);
}

} // namespace

// ---------------------------------------------------------------------
// public : init / tick
// ---------------------------------------------------------------------

void setAppProperty(const std::string &key, const std::string &value)
{
    g_appProps.push_back({key, value});
}

bool midletDestroyed() { return g_destroyed; }
void resetDestroyed() { g_destroyed = false; }
Obj *currentDisplayable() { return g_current; }

int halKeyToMidp(hal::KeyCode kc)
{
    switch (kc)
    {
    case hal::KEY_UP: return 1;
    case hal::KEY_DOWN: return 6;
    case hal::KEY_LEFT: return 2;
    case hal::KEY_RIGHT: return 5;
    case hal::KEY_FIRE: return 8;
    case hal::KEY_0: return '0';
    case hal::KEY_1: return '1';
    case hal::KEY_2: return '2';
    case hal::KEY_3: return '3';
    case hal::KEY_4: return '4';
    case hal::KEY_5: return '5';
    case hal::KEY_6: return '6';
    case hal::KEY_7: return '7';
    case hal::KEY_8: return '8';
    case hal::KEY_9: return '9';
    case hal::KEY_STAR: return '*';
    case hal::KEY_HASH: return '#';
    case hal::KEY_SOFT1: return -6;
    case hal::KEY_SOFT2: return -7;
    default: return 0;
    }
}

uint32_t halKeyState(hal::KeyCode kc)
{
    switch (kc)
    {
    case hal::KEY_UP: return 0x02;
    case hal::KEY_DOWN: return 0x40;
    case hal::KEY_LEFT: return 0x04;
    case hal::KEY_RIGHT: return 0x20;
    case hal::KEY_FIRE: return 0x100;
    default: return 0;
    }
}

void sendKeyEvent(Obj *target, const char *method, int keyCode)
{
    if (!target || !g_interp) return;
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "KEY %s keyCode=%d target=%s\n", method, keyCode,
                target->cls ? target->cls->name.c_str() : "?");
    Value args[2];
    args[0] = Value::fromRef(target);
    args[1] = Value::fromInt(keyCode);
    Value res;
    g_interp->invokeVirtual(target->cls, method, "(I)V", target, args, 2, res);
}

void updateKeyState(uint32_t held)
{
    uint32_t st = 0;
    for (int b = 0; b < 19; b++)
    {
        if (held & (1u << b))
            st |= halKeyState(static_cast<hal::KeyCode>(1u << b));
    }
    g_keyStates = st;
}

void simulatePointer(int x, int y)
{
    if (!g_interp || !g_current) return;
    Obj *cur = g_current;
    if (!cur->cls || !isSubclassOf(cur, "javax/microedition/lcdui/Canvas"))
        return;
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "PTR press+release (%d,%d) target=%s\n", x, y,
                cur->cls->name.c_str());
    Value args[3];
    args[0] = Value::fromRef(cur);
    args[1] = Value::fromInt(x);
    args[2] = Value::fromInt(y);
    Value res;
    g_interp->invokeVirtual(cur->cls, "pointerPressed", "(II)V", cur, args, 3, res);
    g_interp->invokeVirtual(cur->cls, "pointerReleased", "(II)V", cur, args, 3, res);
}

void tick(uint32_t pressedMask, uint32_t justPressedMask, uint32_t justReleasedMask)
{
    g_tickN++;
    advanceVirtualMillis(16);
    if (getenv("JME_VTRACE") && g_tickN % 10 == 0)
        fprintf(stderr, "VTRACE frame=%d vms=%lld\n", g_tickN, (long long)virtualMillis());
    if (!g_rt || !g_interp) return;
    updateKeyState(pressedMask);
    Obj *cur = g_current;
    if (!cur || cur->kind != ObjKind::Instance)
        return;

    // Chaque thread tourne dans sa propre fibre (ucontext, cf. jme_threadResume
    // dans natives.cpp) : Thread.sleep()/yield(), ou l'épuisement du budget
    // d'instructions ci-dessous, suspend RÉELLEMENT son exécution (pc,
    // locales, pile d'appel C++ intacts) et la reprend exactement là à la
    // trame suivante -- au lieu de relancer run() depuis le début à chaque
    // trame (ce qui empêchait toute progression pour les jeux dont la
    // boucle principale dépasse le budget par trame, ex. limiteurs de FPS
    // en bytecode qui font des dizaines de milliers d'itérations entre deux
    // Thread.sleep()). Le budget ci-dessous n'est donc plus qu'un
    // garde-fou anti-boucle-infinie-sans-yield, pas la seule source de
    // suspension.
    constexpr int64_t kThreadInstrBudget = 200000;

    const std::vector<Obj *> &threads = jme_threads();
    std::vector<Obj *> done;
    for (Obj *r : threads)
    {
        const MethodRecord *rm = r->cls->findMethodVirtual("run", "()V");
        if (getenv("JME_DEBUG") && !rm)
            fprintf(stderr, "run()V introuvable sur %s (runnable=%p)\n", r->cls->name.c_str(), (void *)r);
        g_interp->setInstrBudget(kThreadInstrBudget);
        bool finished = jme_threadResume(r, g_interp, r->cls);
        g_interp->setInstrBudget(-1);
        if (getenv("JME_DEBUG") && jme_tickCount())
            fprintf(stderr, "run_frame=%d fmt=%zu finished=%d\n",
                    jme_tickCount(), threads.size(), finished ? 1 : 0);
        if (finished)
            done.push_back(r);
    }
    for (Obj *r : done) jme_threadForget(r);

    bool isCanvas = isSubclassOf(cur, "javax/microedition/lcdui/Canvas");
    bool isGameCanvas = isSubclassOf(cur, "javax/microedition/lcdui/game/GameCanvas");

    if (isCanvas && !isGameCanvas)
    {
        for (int b = 0; b < 19; b++)
        {
            if (justPressedMask & (1u << b))
                sendKeyEvent(cur, "keyPressed", halKeyToMidp(static_cast<hal::KeyCode>(1u << b)));
            if (justReleasedMask & (1u << b))
                sendKeyEvent(cur, "keyReleased", halKeyToMidp(static_cast<hal::KeyCode>(1u << b)));
        }
    }

    if (g_paintRequested && isCanvas && !isGameCanvas)
    {
        if (getenv("JME_DEBUG"))
            fprintf(stderr, "TICK paint sur %s\n", cur->cls ? cur->cls->name.c_str() : "?");
        Value args[2];
        args[0] = Value::fromRef(cur);
        args[1] = Value::fromRef(screenGraphics());
        Value res;
        g_interp->invokeVirtual(cur->cls, "paint", "(Ljavax/microedition/lcdui/Graphics;)V", cur, args, 2, res);
        g_paintRequested = false;
        hal::display_present(hal::display_get_framebuffer());
    }
}

void init(Runtime *rt, Interpreter *interp)
{
    g_rt = rt;
    g_interp = interp;

    delete[] g_canvas565;
    g_canvas565 = new uint16_t[static_cast<size_t>(screenW()) * static_cast<size_t>(screenH())]();

    const std::initializer_list<std::pair<const char *, const char *>> none = {};

    // --- java.lang ---
    regClass(rt, "java/lang/Object", nullptr,
             {{"<init>", "()V"}, {"getClass", "()Ljava/lang/Class;"}, {"equals", "(Ljava/lang/Object;)Z"}, {"hashCode", "()I"}, {"toString", "()Ljava/lang/String;"}},
             none);
    regClass(rt, "java/lang/String", "java/lang/Object",
             {{"<init>", "()V"}, {"<init>", "(Ljava/lang/String;)V"}, {"<init>", "([BLjava/lang/String;)V"}, {"<init>", "([B)V"}, {"<init>", "([BII)V"}, {"length", "()I"}, {"charAt", "(I)C"}, {"toCharArray", "()[C"}, {"concat", "(Ljava/lang/String;)Ljava/lang/String;"}, {"equals", "(Ljava/lang/Object;)Z"}, {"substring", "(I)Ljava/lang/String;"}, {"substring", "(II)Ljava/lang/String;"}, {"indexOf", "(Ljava/lang/String;)I"}, {"indexOf", "(Ljava/lang/String;I)I"}, {"indexOf", "(I)I"}, {"indexOf", "(II)I"}, {"trim", "()Ljava/lang/String;"}, {"toLowerCase", "()Ljava/lang/String;"}, {"toUpperCase", "()Ljava/lang/String;"}, {"compareTo", "(Ljava/lang/String;)I"}, {"startsWith", "(Ljava/lang/String;)Z"}, {"endsWith", "(Ljava/lang/String;)Z"}, {"equalsIgnoreCase", "(Ljava/lang/String;)Z"}},
             none);
    regClass(rt, "java/lang/Math", "java/lang/Object",
             {{"abs", "(I)I"}, {"abs", "(J)J"}, {"min", "(II)I"}, {"min", "(JJ)J"}, {"max", "(II)I"}, {"max", "(JJ)J"}, {"sqrt", "(D)D"}, {"floor", "(D)D"}, {"ceil", "(D)D"}, {"round", "(D)J"}, {"pow", "(DD)D"}, {"random", "()D"}},
             none);
    regClass(rt, "java/lang/System", "java/lang/Object",
             {{"currentTimeMillis", "()J"}, {"arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V"}, {"gc", "()V"}, {"identityHashCode", "(Ljava/lang/Object;)I"}, {"getProperty", "(Ljava/lang/String;)Ljava/lang/String;"}},
             {{"out", "Ljava/io/PrintStream;"}});
    regClass(rt, "java/io/PrintStream", "java/lang/Object",
             {{"println", "(Ljava/lang/String;)V"}, {"println", "(I)V"}, {"println", "()V"}, {"print", "(Ljava/lang/String;)V"}, {"print", "(I)V"}, {"flush", "()V"}},
             none);
    regClass(rt, "java/lang/Class", "java/lang/Object",
             {{"getName", "()Ljava/lang/String;"}, {"forName", "(Ljava/lang/String;)Ljava/lang/Class;"}, {"getResourceAsStream", "(Ljava/lang/String;)Ljava/io/InputStream;"}},
             none);

    // --- java.lang compléments CLDC basiques ---
    regClass(rt, "java/lang/Integer", "java/lang/Object",
             {{"<init>", "(I)V"}, {"intValue", "()I"}, {"byteValue", "()B"}, {"shortValue", "()S"}, {"longValue", "()J"}, {"hashCode", "()I"}, {"toString", "()Ljava/lang/String;"}, {"toString", "(I)Ljava/lang/String;"}, {"equals", "(Ljava/lang/Object;)Z"}, {"compareTo", "(Ljava/lang/Integer;)I"}, {"valueOf", "(I)Ljava/lang/Integer;"}, {"parseInt", "(Ljava/lang/String;)I"}},
             {{"value", "I"}});
    regClass(rt, "java/lang/StringBuffer", "java/lang/Object",
             {{"<init>", "()V"}, {"<init>", "(Ljava/lang/String;)V"}, {"<init>", "(I)V"}, {"append", "(Ljava/lang/String;)Ljava/lang/StringBuffer;"}, {"append", "(I)Ljava/lang/StringBuffer;"}, {"append", "(C)Ljava/lang/StringBuffer;"}, {"append", "(J)Ljava/lang/StringBuffer;"}, {"append", "(Z)Ljava/lang/StringBuffer;"}, {"append", "(Ljava/lang/Object;)Ljava/lang/StringBuffer;"}, {"append", "(F)Ljava/lang/StringBuffer;"}, {"append", "(D)Ljava/lang/StringBuffer;"}, {"toString", "()Ljava/lang/String;"}, {"length", "()I"}, {"charAt", "(I)C"}, {"setCharAt", "(IC)V"}, {"setLength", "(I)V"}, {"delete", "(II)Ljava/lang/StringBuffer;"}},
             {{"str", "Ljava/lang/String;"}});
    regClass(rt, "java/lang/Thread", "java/lang/Object",
             {{"<init>", "(Ljava/lang/Runnable;)V"}, {"run", "()V"}, {"start", "()V"}, {"sleep", "(J)V"}, {"yield", "()V"}, {"currentThread", "()Ljava/lang/Thread;"}, {"setPriority", "(I)V"}, {"interrupt", "()V"}, {"isAlive", "()Z"}, {"join", "()V"}},
             {{"r", "Ljava/lang/Runnable;"}});
    regClass(rt, "java/util/Hashtable", "java/lang/Object",
             {{"<init>", "()V"}, {"get", "(Ljava/lang/Object;)Ljava/lang/Object;"}, {"put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"}, {"remove", "(Ljava/lang/Object;)Ljava/lang/Object;"}, {"containsKey", "(Ljava/lang/Object;)Z"}, {"clear", "()V"}, {"size", "()I"}, {"isEmpty", "()Z"}},
             {{"count", "I"}, {"table", "[Ljava/lang/Object;"}});
    regClass(rt, "java/util/Random", "java/lang/Object",
             {{"<init>", "(J)V"}, {"<init>", "()V"}, {"setSeed", "(J)V"}, {"nextInt", "()I"}, {"nextInt", "(I)I"}, {"nextLong", "()J"}, {"nextDouble", "()D"}, {"nextFloat", "()F"}, {"nextBoolean", "()Z"}},
             {{"seed", "J"}});
    regClass(rt, "java/util/Vector", "java/lang/Object",
             {{"<init>", "()V"}, {"<init>", "(I)V"},
              {"addElement", "(Ljava/lang/Object;)V"}, {"elementAt", "(I)Ljava/lang/Object;"},
              {"setElementAt", "(Ljava/lang/Object;I)V"}, {"insertElementAt", "(Ljava/lang/Object;I)V"},
              {"removeElement", "(Ljava/lang/Object;)Z"}, {"removeElementAt", "(I)V"},
              {"removeAllElements", "()V"}, {"size", "()I"}, {"isEmpty", "()Z"},
              {"contains", "(Ljava/lang/Object;)Z"}, {"indexOf", "(Ljava/lang/Object;)I"},
              {"firstElement", "()Ljava/lang/Object;"}, {"lastElement", "()Ljava/lang/Object;"}},
             {{"elementCount", "I"}, {"elementData", "[Ljava/lang/Object;"}});

    // --- java.io ---
    regClass(rt, "java/io/InputStream", "java/lang/Object",
             {{"read", "()I"}, {"read", "([B)I"}, {"read", "([BII)I"}, {"available", "()I"}, {"skip", "(J)J"}, {"close", "()V"}, {"markSupported", "()Z"}, {"mark", "(I)V"}, {"reset", "()V"}},
             {{"data", "[B"}, {"pos", "I"}, {"limit", "I"}});
    regClass(rt, "java/io/ByteArrayInputStream", "java/io/InputStream",
             {{"<init>", "([B)V"}, {"read", "()I"}, {"read", "([B)I"}, {"read", "([BII)I"}, {"available", "()I"}, {"skip", "(J)J"}, {"close", "()V"}, {"markSupported", "()Z"}, {"mark", "(I)V"}, {"reset", "()V"}},
             {{"data", "[B"}, {"pos", "I"}, {"limit", "I"}});
    regClass(rt, "java/io/DataInputStream", "java/lang/Object",
             {{"<init>", "(Ljava/io/InputStream;)V"}, {"read", "()I"}, {"read", "([B)I"}, {"read", "([BII)I"}, {"readBoolean", "()Z"}, {"readByte", "()B"}, {"readUnsignedByte", "()I"}, {"readShort", "()S"}, {"readUnsignedShort", "()I"}, {"readChar", "()C"}, {"readInt", "()I"}, {"readLong", "()J"}, {"readFully", "([B)V"}, {"readFully", "([BII)V"}, {"readUTF", "()Ljava/lang/String;"}, {"skipBytes", "(I)I"}, {"available", "()I"}, {"close", "()V"}},
             {{"in", "Ljava/io/InputStream;"}});

    // --- MIDlet ---
    regClass(rt, "javax/microedition/midlet/MIDlet", "java/lang/Object",
             {{"<init>", "()V"}, {"getAppProperty", "(Ljava/lang/String;)Ljava/lang/String;"}, {"notifyDestroyed", "()V"}, {"notifyPaused", "()V"}, {"resumeRequest", "()Z"}},
             none);

    // --- lcdui ---
    regClass(rt, "javax/microedition/lcdui/Displayable", "java/lang/Object",
             {{"setTitle", "(Ljava/lang/String;)V"}, {"getTitle", "()Ljava/lang/String;"}, {"addCommand", "(Ljavax/microedition/lcdui/Command;)V"}, {"setCommandListener", "(Ljavax/microedition/lcdui/CommandListener;)V"}, {"isShown", "()Z"}, {"getWidth", "()I"}, {"getHeight", "()I"}},
             none);
    // GC_FULLSCREEN/GC_GFX (cf. plus bas) indexent cells[0]/cells[1] de
    // l'instance via les accesseurs gc_* -- et CES ACCESSEURS SONT PARTAGÉS
    // entre Canvas ET GameCanvas (setFullScreenMode/getGraphics/
    // flushGraphics/getKeyStates sont enregistrés sur les DEUX classes, cf.
    // plus bas). Les 2 champs doivent donc être réservés ici, sur Canvas
    // lui-même (l'ancêtre commun), et non sur GameCanvas seul : un jeu qui
    // étend directement Canvas (ou FullCanvas, qui étend Canvas) et appelle
    // setFullScreenMode()/getGraphics() sans jamais passer par GameCanvas
    // écrivait sinon directement dans cells[0]/cells[1] de l'instance --
    // silencieusement aliasés avec les 2 premiers champs applicatifs de la
    // sous-classe concrète (observé sur games/prince.jar : cells[0] d'un
    // Canvas obfusqué, son propre champ "bi" (référence vers le MIDlet),
    // écrasé par le booléen passé à setFullScreenMode(true), provoquant un
    // crash différé bien plus tard sur un getfield lisant ce même champ).
    regClass(rt, "javax/microedition/lcdui/Canvas", "javax/microedition/lcdui/Displayable",
             {{"getWidth", "()I"}, {"getHeight", "()I"}, {"isDoubleBuffered", "()Z"}, {"repaint", "()V"}, {"repaint", "(IIII)V"}, {"serviceRepaints", "()V"}, {"showNotify", "()V"}, {"hideNotify", "()V"}, {"setFullScreenMode", "(Z)V"}, {"getGraphics", "()Ljavax/microedition/lcdui/Graphics;"}, {"flushGraphics", "()V"}, {"flushGraphics", "(IIII)V"}, {"getKeyStates", "()I"}, {"getGameAction", "(I)I"}, {"getKeyCode", "(I)I"}, {"keyPressed", "(I)V"}, {"keyReleased", "(I)V"}, {"keyRepeated", "(I)V"}, {"pointerPressed", "(II)V"}, {"pointerReleased", "(II)V"}, {"pointerDragged", "(II)V"}},
             {{"__fullscreen", "Z"}, {"__gfx", "Ljavax/microedition/lcdui/Graphics;"}});
    regClass(rt, "javax/microedition/lcdui/game/GameCanvas", "javax/microedition/lcdui/Canvas",
             {{"<init>", "(Z)V"}, {"setFullScreenMode", "(Z)V"}, {"getGraphics", "()Ljavax/microedition/lcdui/Graphics;"}, {"flushGraphics", "()V"}, {"flushGraphics", "(IIII)V"}, {"getKeyStates", "()I"}},
             none);
    // com.nokia.mid.ui.FullCanvas : extension Nokia UI API (pas MIDP
    // standard), très utilisée par les jeux ciblant les téléphones Nokia de
    // l'époque à la place de javax.microedition.lcdui.Canvas -- observé sur
    // games/mission.jar (GloftMI3). Son API (paint/keyPressed/keyReleased/
    // getWidth/getHeight/repaint/setFullScreenMode/...) est un sur-ensemble
    // de Canvas ; on la fait donc hériter de notre Canvas natif pour
    // réutiliser telles quelles toutes ses natives. Les constantes de touche
    // Nokia (KEY_SOFTKEY1, KEY_UP_ARROW, ...) sont `static final int` côté
    // Java : javac les inline en littéraux au site d'appel, donc aucun champ
    // à déclarer ici pour qu'un getstatic les résolve.
    regClass(rt, "com/nokia/mid/ui/FullCanvas", "javax/microedition/lcdui/Canvas",
             none, none);
    // javax.microedition.rms.RecordStore : magasin d'enregistrements MIDP
    // (sauvegardes/scores/préférences). Implémentation minimale en mémoire
    // côté C++ (natives.cpp, rsRegistry()) -- non persistante entre
    // lancements, mais suffisante pour le chemin "aucune sauvegarde
    // existante" (getNumRecords()==0) qu'exercent la plupart des jeux au
    // premier lancement.
    regClass(rt, "javax/microedition/rms/RecordStore", "java/lang/Object",
             {{"openRecordStore", "(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;"},
              {"getNumRecords", "()I"}, {"getRecord", "(I[BI)I"}, {"getRecordSize", "(I)I"},
              {"setRecord", "(I[BII)V"}, {"addRecord", "([BII)I"}, {"closeRecordStore", "()V"},
              {"deleteRecordStore", "(Ljava/lang/String;)V"}},
             {{"__name", "Ljava/lang/String;"}});
    regClass(rt, "javax/microedition/lcdui/Graphics", "java/lang/Object",
             {{"setColor", "(I)V"}, {"setColor", "(III)V"}, {"getColor", "()I"}, {"setGrayScale", "(I)V"}, {"getGrayScale", "()I"}, {"fillRect", "(IIII)V"}, {"drawRect", "(IIII)V"}, {"drawLine", "(IIII)V"}, {"fillTriangle", "(IIIIII)V"}, {"drawArc", "(IIIIII)V"}, {"fillArc", "(IIIIII)V"}, {"fillRoundRect", "(IIIIII)V"}, {"drawRoundRect", "(IIIIII)V"}, {"setFont", "(Ljavax/microedition/lcdui/Font;)V"}, {"getFont", "()Ljavax/microedition/lcdui/Font;"}, {"drawString", "(Ljava/lang/String;II)V"}, {"drawString", "(Ljava/lang/String;III)V"}, {"drawChar", "(CII)V"}, {"drawChars", "([CIIII)V"}, {"drawImage", "(Ljavax/microedition/lcdui/Image;III)V"}, {"drawRegion", "(Ljavax/microedition/lcdui/Image;IIIIIIII)V"}, {"setClip", "(IIII)V"}, {"clipRect", "(IIII)V"}, {"getClipX", "()I"}, {"getClipY", "()I"}, {"getClipWidth", "()I"}, {"getClipHeight", "()I"}, {"translate", "(II)V"}, {"getTranslateX", "()I"}, {"getTranslateY", "()I"}, {"drawRGB", "([IIIIIIIZ)V"}},
             {{"color", "I"}, {"font", "Ljavax/microedition/lcdui/Font;"}, {"translateX", "I"}, {"translateY", "I"}, {"clipX", "I"}, {"clipY", "I"}, {"clipW", "I"}, {"clipH", "I"}, {"mode", "I"}, {"targetW", "I"}, {"targetH", "I"}, {"stride", "I"}, {"buf", "[I"}});
    regClass(rt, "javax/microedition/lcdui/Font", "java/lang/Object",
             {{"getFont", "(III)Ljavax/microedition/lcdui/Font;"}, {"getHeight", "()I"}, {"getBaselinePosition", "()I"}, {"getFace", "()I"}, {"getStyle", "()I"}, {"getSize", "()I"}, {"stringWidth", "(Ljava/lang/String;)I"}, {"charWidth", "(C)I"}, {"charsWidth", "([CII)I"}},
             {{"face", "I"}, {"style", "I"}, {"size", "I"}});
    regClass(rt, "javax/microedition/lcdui/Image", "java/lang/Object",
             {{"createImage", "(II)Ljavax/microedition/lcdui/Image;"}, {"createImage", "(Ljava/lang/String;)Ljavax/microedition/lcdui/Image;"}, {"createImage", "(Ljava/io/InputStream;)Ljavax/microedition/lcdui/Image;"}, {"createImage", "([BII)Ljavax/microedition/lcdui/Image;"}, {"createImage", "(Ljavax/microedition/lcdui/Image;)Ljavax/microedition/lcdui/Image;"}, {"createRGBImage", "([IIIZ)Ljavax/microedition/lcdui/Image;"}, {"getGraphics", "()Ljavax/microedition/lcdui/Graphics;"}, {"getWidth", "()I"}, {"getHeight", "()I"}, {"isMutable", "()Z"}, {"getRGB", "([IIIIII)V"}},
             {{"width", "I"}, {"height", "I"}, {"mutable", "Z"}, {"buf", "[I"}, {"gfx", "Ljavax/microedition/lcdui/Graphics;"}});
    regClass(rt, "javax/microedition/lcdui/Display", "java/lang/Object",
             {{"getDisplay", "(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;"}, {"getCurrent", "()Ljavax/microedition/lcdui/Displayable;"}, {"setCurrent", "(Ljavax/microedition/lcdui/Displayable;)V"}, {"setCurrent", "(Ljavax/microedition/lcdui/Alert;Ljavax/microedition/lcdui/Displayable;)V"}, {"setCurrentItem", "(Ljavax/microedition/lcdui/Item;)V"}, {"isColor", "()Z"}, {"numColors", "()I"}, {"numAlphaLevels", "()I"}, {"flashBacklight", "(I)V"}, {"repaint", "()V"}, {"callSerially", "(Ljava/lang/Runnable;)V"}},
             none);

    // --- Stubs UI (Alert/Form/Command/List/...) ---
    regClass(rt, "javax/microedition/lcdui/Alert", "javax/microedition/lcdui/Displayable",
             {{"<init>", "(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;)V"}, {"<init>", "(Ljava/lang/String;)V"}, {"setTimeout", "(I)V"}, {"setString", "(Ljava/lang/String;)V"}},
             none);
    regClass(rt, "javax/microedition/lcdui/AlertType", "java/lang/Object",
             {{"<init>", "()V"}}, none);
    regClass(rt, "javax/microedition/lcdui/Command", "java/lang/Object",
             {{"<init>", "(Ljava/lang/String;II)V"}, {"<init>", "(Ljava/lang/String;Ljava/lang/String;II)V"}, {"getLabel", "()Ljava/lang/String;"}},
             none);
    regClass(rt, "javax/microedition/lcdui/Item", "java/lang/Object",
             {{"getLabel", "()Ljava/lang/String;"}, {"setLabel", "(Ljava/lang/String;)V"}}, none);
    regClass(rt, "javax/microedition/lcdui/Form", "javax/microedition/lcdui/Displayable",
             {{"<init>", "(Ljava/lang/String;)V"}, {"<init>", "(Ljava/lang/String;Ljavax/microedition/lcdui/Item;)V"}, {"append", "(Ljavax/microedition/lcdui/Item;)I"}, {"append", "(Ljava/lang/String;)I"}, {"size", "()I"}, {"set", "(ILjavax/microedition/lcdui/Item;)V"}},
             none);
    regClass(rt, "javax/microedition/lcdui/List", "javax/microedition/lcdui/Displayable",
             {{"<init>", "(Ljava/lang/String;I[Ljava/lang/String;Ljavax/microedition/lcdui/Image;)V"}, {"<init>", "(Ljava/lang/String;I)V"}, {"setSelectedIndex", "(IZ)V"}, {"setSelectCommand", "(Ljavax/microedition/lcdui/Command;)V"}, {"append", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I"}, {"getSelectedIndex", "()I"}, {"size", "()I"}},
             none);
    regClass(rt, "javax/microedition/lcdui/TextBox", "javax/microedition/lcdui/Displayable",
             {{"<init>", "(Ljava/lang/String;Ljava/lang/String;II)V"}, {"setString", "(Ljava/lang/String;)V"}, {"getString", "()Ljava/lang/String;"}},
             none);
    regClass(rt, "javax/microedition/lcdui/TextField", "javax/microedition/lcdui/Item",
             {{"<init>", "(Ljava/lang/String;Ljava/lang/String;II)V"}, {"setString", "(Ljava/lang/String;)V"}, {"getString", "()Ljava/lang/String;"}},
             none);
    regClass(rt, "javax/microedition/lcdui/StringItem", "javax/microedition/lcdui/Item",
             {{"<init>", "(Ljava/lang/String;Ljava/lang/String;)V"}, {"setText", "(Ljava/lang/String;)V"}, {"getText", "()Ljava/lang/String;"}},
             none);
    regClass(rt, "javax/microedition/lcdui/ImageItem", "javax/microedition/lcdui/Item",
             {{"<init>", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;)V"}}, none);
    regClass(rt, "javax/microedition/lcdui/Gauge", "javax/microedition/lcdui/Item",
             {{"<init>", "(Ljava/lang/String;ZII)V"}, {"setValue", "(I)V"}, {"getValue", "()I"}}, none);
    regClass(rt, "javax/microedition/lcdui/ChoiceGroup", "javax/microedition/lcdui/Item",
             {{"<init>", "(Ljava/lang/String;I[Ljava/lang/String;Ljavax/microedition/lcdui/Image;)V"}, {"append", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I"}, {"getSelectedIndex", "()I"}, {"size", "()I"}},
             none);
    regClass(rt, "javax/microedition/lcdui/Choice", "java/lang/Object", none, none);
    regClass(rt, "javax/microedition/lcdui/Ticker", "java/lang/Object",
             {{"<init>", "(Ljava/lang/String;)V"}}, none);
    regClass(rt, "javax/microedition/lcdui/Screen", "javax/microedition/lcdui/Displayable", none, none);
    regClass(rt, "javax/microedition/lcdui/ItemCommandListener", "java/lang/Object", none, none);
    regClass(rt, "javax/microedition/lcdui/CommandListener", "java/lang/Object", none, none);
    regClass(rt, "javax/microedition/lcdui/DisplayableCommandListener", "java/lang/Object", none, none);
    regClass(rt, "javax/microedition/lcdui/CustomItem", "javax/microedition/lcdui/Item",
             {{"<init>", "(Ljava/lang/String;)V"}, {"getMinContentWidth", "()I"}, {"getMinContentHeight", "()I"}, {"getPrefContentWidth", "(I)I"}, {"getPrefContentHeight", "(I)I"}, {"repaint", "()V"}},
             none);

    // --- MMAPI (stub audio : pas de son, débloque Manager.createPlayer) ---
    regClass(rt, "javax/microedition/media/Player", "java/lang/Object",
             {{"realize", "()V"}, {"prefetch", "()V"}, {"start", "()V"}, {"stop", "()V"},
              {"deallocate", "()V"}, {"close", "()V"}, {"setLoopCount", "(I)V"},
              {"getState", "()I"}, {"getDuration", "()J"}, {"getMediaTime", "()J"},
              {"setMediaTime", "(J)J"}, {"getContentType", "()Ljava/lang/String;"},
              {"addPlayerListener", "(Ljavax/microedition/media/PlayerListener;)V"},
              {"removePlayerListener", "(Ljavax/microedition/media/PlayerListener;)V"}},
             none);
    regClass(rt, "javax/microedition/media/Manager", "java/lang/Object",
             {{"createPlayer", "(Ljava/lang/String;)Ljavax/microedition/media/Player;"},
              {"createPlayer", "(Ljava/io/InputStream;Ljava/lang/String;)Ljavax/microedition/media/Player;"}},
             none);
    regClass(rt, "javax/microedition/media/PlayerListener", "java/lang/Object", none, none);

    // ---- Handlers de base (java.lang) ----
    // ATTENTION : natives.cpp (initNatives(), appelé avant midp::init() —
    // cf. main.cpp) enregistre déjà des implémentations réelles pour
    // Object/String/Math/System/PrintStream/Class.getName+forName.
    // registerNative() fait un simple écrasement de map (registry()[key]=fn) :
    // réenregistrer ces mêmes clés ici avec `ui_noop` les neutralisait
    // SILENCIEUSEMENT (aucune erreur, juste un no-op qui ne renseigne jamais
    // le résultat). Bug sévère et large spectre — entre autres,
    // System.currentTimeMillis() retournait toujours 0 et System.arraycopy()
    // ne faisait plus rien pour absolument tous les MIDlets, symptôme
    // observé indirectement dans plusieurs diagnostics précédents sur ce
    // dépôt. Seule Class.getResourceAsStream n'est PAS dupliquée par
    // natives.cpp (elle a besoin d'accéder au jar, propre à cette couche
    // midp) : c'est la seule entrée qui doit rester ici.
    regN("java/io/PrintStream.<init>:(Ljava/io/OutputStream;)V", ui_noop);
    regN("java/lang/Class.getResourceAsStream:(Ljava/lang/String;)Ljava/io/InputStream;", cl_getResourceAsStream);

    // ---- Handlers java.io ----
    regN("java/io/InputStream.read:()I", is_read);
    regN("java/io/InputStream.read:([B)I", is_readArr);
    regN("java/io/InputStream.read:([BII)I", is_readArrII);
    regN("java/io/InputStream.available:()I", is_available);
    regN("java/io/InputStream.skip:(J)J", is_skip);
    regN("java/io/InputStream.close:()V", is_close);
    regN("java/io/InputStream.markSupported:()Z", is_markSupported);
    regN("java/io/InputStream.mark:(I)V", bais_mark);
    regN("java/io/InputStream.reset:()V", bais_reset);
    regN("java/io/ByteArrayInputStream.<init>:([B)V", bais_init);
    regN("java/io/ByteArrayInputStream.read:()I", is_read);
    regN("java/io/ByteArrayInputStream.read:([B)I", is_readArr);
    regN("java/io/ByteArrayInputStream.read:([BII)I", is_readArrII);
    regN("java/io/ByteArrayInputStream.available:()I", is_available);
    regN("java/io/ByteArrayInputStream.skip:(J)J", is_skip);
    regN("java/io/ByteArrayInputStream.close:()V", is_close);
    regN("java/io/ByteArrayInputStream.markSupported:()Z", bais_markSupported);
    regN("java/io/ByteArrayInputStream.mark:(I)V", bais_mark);
    regN("java/io/ByteArrayInputStream.reset:()V", bais_reset);
    regN("java/io/DataInputStream.<init>:(Ljava/io/InputStream;)V", dis_init);
    regN("java/io/DataInputStream.read:()I", di_read);
    regN("java/io/DataInputStream.read:([B)I", di_readArr);
    regN("java/io/DataInputStream.read:([BII)I", di_readArrII);
    regN("java/io/DataInputStream.readBoolean:()Z", di_readBoolean);
    regN("java/io/DataInputStream.readByte:()B", di_readByte);
    regN("java/io/DataInputStream.readUnsignedByte:()I", di_readUnsignedByte);
    regN("java/io/DataInputStream.readShort:()S", di_readShort);
    regN("java/io/DataInputStream.readUnsignedShort:()I", di_readUnsignedShort);
    regN("java/io/DataInputStream.readChar:()C", di_readChar);
    regN("java/io/DataInputStream.readInt:()I", di_readInt);
    regN("java/io/DataInputStream.readLong:()J", di_readLong);
    regN("java/io/DataInputStream.readFully:([B)V", di_readFully);
    regN("java/io/DataInputStream.readFully:([BII)V", di_readFullyII);
    regN("java/io/DataInputStream.readUTF:()Ljava/lang/String;", di_readUTF);
    regN("java/io/DataInputStream.close:()V", is_close);
    regN("java/io/DataInputStream.skipBytes:(I)I", di_skipBytes);

    // ---- Handlers lcdui ----
    regN("javax/microedition/lcdui/Graphics.setColor:(I)V", g_setColorI);
    regN("javax/microedition/lcdui/Graphics.setColor:(III)V", g_setColorRGB);
    regN("javax/microedition/lcdui/Graphics.getColor:()I", g_getColor);
    regN("javax/microedition/lcdui/Graphics.setGrayScale:(I)V", g_setGray);
    regN("javax/microedition/lcdui/Graphics.getGrayScale:()I", g_getColor);
    regN("javax/microedition/lcdui/Graphics.fillRect:(IIII)V", g_fillRect);
    regN("javax/microedition/lcdui/Graphics.drawRect:(IIII)V", g_drawRect);
    regN("javax/microedition/lcdui/Graphics.drawLine:(IIII)V", g_drawLine);
    regN("javax/microedition/lcdui/Graphics.fillTriangle:(IIIIII)V", g_fillTriangle);
    regN("javax/microedition/lcdui/Graphics.drawArc:(IIIIII)V", g_drawArc);
    regN("javax/microedition/lcdui/Graphics.fillArc:(IIIIII)V", g_fillArc);
    regN("javax/microedition/lcdui/Graphics.fillRoundRect:(IIIIII)V", g_fillRoundRect);
    regN("javax/microedition/lcdui/Graphics.drawRoundRect:(IIIIII)V", g_drawRoundRect);
    regN("javax/microedition/lcdui/Graphics.setFont:(Ljavax/microedition/lcdui/Font;)V", g_setFont);
    regN("javax/microedition/lcdui/Graphics.getFont:()Ljavax/microedition/lcdui/Font;", g_getFont);
    regN("javax/microedition/lcdui/Graphics.fillRect:(IIII)V", g_fillRect);
    regN("javax/microedition/lcdui/Graphics.drawRect:(IIII)V", g_drawRect);
    regN("javax/microedition/lcdui/Graphics.drawLine:(IIII)V", g_drawLine);
    regN("javax/microedition/lcdui/Graphics.fillTriangle:(IIIIII)V", g_fillTriangle);
    regN("javax/microedition/lcdui/Graphics.drawArc:(IIIIII)V", g_drawArc);
    regN("javax/microedition/lcdui/Graphics.fillArc:(IIIIII)V", g_fillArc);
    regN("javax/microedition/lcdui/Graphics.fillRoundRect:(IIIIII)V", g_fillRoundRect);
    regN("javax/microedition/lcdui/Graphics.drawRoundRect:(IIIIII)V", g_drawRoundRect);
    regN("javax/microedition/lcdui/Graphics.drawString:(Ljava/lang/String;II)V", g_drawString);
    regN("javax/microedition/lcdui/Graphics.drawString:(Ljava/lang/String;III)V", g_drawString);
    regN("javax/microedition/lcdui/Graphics.drawChar:(CII)V", g_drawChar);
    regN("javax/microedition/lcdui/Graphics.drawChars:([CIIII)V", g_drawChars);
    regN("javax/microedition/lcdui/Graphics.drawImage:(Ljavax/microedition/lcdui/Image;III)V", g_drawImage);
    regN("javax/microedition/lcdui/Graphics.drawRegion:(Ljavax/microedition/lcdui/Image;IIIIIIII)V", g_drawRegion);
    regN("javax/microedition/lcdui/Graphics.setClip:(IIII)V", g_setClipXYWH);
    regN("javax/microedition/lcdui/Graphics.clipRect:(IIII)V", g_clipRect);
    regN("javax/microedition/lcdui/Graphics.getClipX:()I", g_getClipX);
    regN("javax/microedition/lcdui/Graphics.getClipY:()I", g_getClipY);
    regN("javax/microedition/lcdui/Graphics.getClipWidth:()I", g_getClipW);
    regN("javax/microedition/lcdui/Graphics.getClipHeight:()I", g_getClipH);
    regN("javax/microedition/lcdui/Graphics.translate:(II)V", g_translate);
    regN("javax/microedition/lcdui/Graphics.getTranslateX:()I", g_getTranslateX);
    regN("javax/microedition/lcdui/Graphics.getTranslateY:()I", g_getTranslateY);
    regN("javax/microedition/lcdui/Graphics.drawRGB:([IIIIIIIZ)V", g_drawRGB);
    regN("javax/microedition/lcdui/Graphics.drawString:(Ljava/lang/String;II)V", g_drawString);
    regN("javax/microedition/lcdui/Graphics.drawString:(Ljava/lang/String;III)V", g_drawString);
    regN("javax/microedition/lcdui/Graphics.drawChar:(CII)V", g_drawChar);
    regN("javax/microedition/lcdui/Graphics.drawChars:([CIIII)V", g_drawChars);
    regN("javax/microedition/lcdui/Graphics.drawImage:(Ljavax/microedition/lcdui/Image;III)V", g_drawImage);
    regN("javax/microedition/lcdui/Graphics.drawRegion:(Ljavax/microedition/lcdui/Image;IIIIIIII)V", g_drawRegion);
    regN("javax/microedition/lcdui/Graphics.setClip:(IIII)V", g_setClipXYWH);
    regN("javax/microedition/lcdui/Graphics.clipRect:(IIII)V", g_clipRect);
    regN("javax/microedition/lcdui/Graphics.getClipX:()I", g_getClipX);
    regN("javax/microedition/lcdui/Graphics.getClipY:()I", g_getClipY);
    regN("javax/microedition/lcdui/Graphics.getClipWidth:()I", g_getClipW);
    regN("javax/microedition/lcdui/Graphics.getClipHeight:()I", g_getClipH);
    regN("javax/microedition/lcdui/Graphics.translate:(II)V", g_translate);
    regN("javax/microedition/lcdui/Graphics.getTranslateX:()I", g_getTranslateX);
    regN("javax/microedition/lcdui/Graphics.getTranslateY:()I", g_getTranslateY);

    regN("javax/microedition/lcdui/Font.getFont:(III)Ljavax/microedition/lcdui/Font;", f_getFont);
    regN("javax/microedition/lcdui/Font.getHeight:()I", f_getHeight);
    regN("javax/microedition/lcdui/Font.getBaselinePosition:()I", f_getBaseline);
    regN("javax/microedition/lcdui/Font.getFace:()I", f_getFace);
    regN("javax/microedition/lcdui/Font.getStyle:()I", f_getStyle);
    regN("javax/microedition/lcdui/Font.getSize:()I", f_getSize);
    regN("javax/microedition/lcdui/Font.stringWidth:(Ljava/lang/String;)I", f_stringWidth);
    regN("javax/microedition/lcdui/Font.charWidth:(C)I", f_charWidth);
    regN("javax/microedition/lcdui/Font.charsWidth:([CII)I", f_charsWidth);

    regN("javax/microedition/lcdui/Image.createImage:(II)Ljavax/microedition/lcdui/Image;", img_createWH);
    regN("javax/microedition/lcdui/Image.createImage:(Ljava/lang/String;)Ljavax/microedition/lcdui/Image;", img_createString);
    regN("javax/microedition/lcdui/Image.createImage:(Ljava/io/InputStream;)Ljavax/microedition/lcdui/Image;", img_createStream);
    regN("javax/microedition/lcdui/Image.createImage:([BII)Ljavax/microedition/lcdui/Image;", img_createBytes);
    regN("javax/microedition/lcdui/Image.createImage:(Ljavax/microedition/lcdui/Image;)Ljavax/microedition/lcdui/Image;", img_copy);
    regN("javax/microedition/lcdui/Image.createRGBImage:([IIIZ)Ljavax/microedition/lcdui/Image;", img_createRGB);
    regN("javax/microedition/lcdui/Image.getGraphics:()Ljavax/microedition/lcdui/Graphics;", img_getGraphics);
    regN("javax/microedition/lcdui/Image.getWidth:()I", img_getWidth);
    regN("javax/microedition/lcdui/Image.getHeight:()I", img_getHeight);
    regN("javax/microedition/lcdui/Image.isMutable:()Z", img_isMutable);
    regN("javax/microedition/lcdui/Image.getRGB:([IIIIII)V", img_getRGB);

    regN("javax/microedition/lcdui/game/GameCanvas.<init>:(Z)V", gc_init);
    regN("javax/microedition/lcdui/game/GameCanvas.setFullScreenMode:(Z)V", gc_setFullScreen);
    regN("javax/microedition/lcdui/game/GameCanvas.getGraphics:()Ljavax/microedition/lcdui/Graphics;", gc_getGraphics);
    regN("javax/microedition/lcdui/game/GameCanvas.flushGraphics:()V", gc_flushGraphics);
    regN("javax/microedition/lcdui/game/GameCanvas.flushGraphics:(IIII)V", gc_flushRegion);
    regN("javax/microedition/lcdui/game/GameCanvas.getKeyStates:()I", gc_getKeyStates);

    regN("javax/microedition/lcdui/Canvas.getWidth:()I", cv_getWidth);
    regN("javax/microedition/lcdui/Canvas.getHeight:()I", cv_getHeight);
    regN("javax/microedition/lcdui/Canvas.isDoubleBuffered:()Z", cv_isDoubleBuffered);
    regN("javax/microedition/lcdui/Canvas.repaint:()V", cv_repaint);
    regN("javax/microedition/lcdui/Canvas.repaint:(IIII)V", cv_repaintRegion);
    regN("javax/microedition/lcdui/Canvas.setFullScreenMode:(Z)V", gc_setFullScreen);
    regN("javax/microedition/lcdui/Canvas.getGraphics:()Ljavax/microedition/lcdui/Graphics;", gc_getGraphics);
    regN("javax/microedition/lcdui/Canvas.flushGraphics:()V", gc_flushGraphics);
    regN("javax/microedition/lcdui/Canvas.flushGraphics:(IIII)V", gc_flushRegion);
    regN("javax/microedition/lcdui/Canvas.getKeyStates:()I", gc_getKeyStates);
    regN("javax/microedition/lcdui/Canvas.serviceRepaints:()V", cv_service);
    regN("javax/microedition/lcdui/Canvas.showNotify:()V", cv_showNotify);
    regN("javax/microedition/lcdui/Canvas.hideNotify:()V", cv_hideNotify);
    regN("javax/microedition/lcdui/Canvas.getGameAction:(I)I", cv_getGameAction);
    regN("javax/microedition/lcdui/Canvas.getKeyCode:(I)I", cv_getKeyCode);
    // Implémentations par défaut (no-op) : keyPressed/keyReleased/keyRepeated
    // et le pointeur tactile sont optionnels en MIDP -- une sous-classe qui
    // n'en surcharge qu'une partie (ex. keyPressed sans keyReleased) ne doit
    // pas faire échouer invokeVirtual quand tick() appelle l'autre.
    regN("javax/microedition/lcdui/Canvas.keyPressed:(I)V", ui_noop);
    regN("javax/microedition/lcdui/Canvas.keyReleased:(I)V", ui_noop);
    regN("javax/microedition/lcdui/Canvas.keyRepeated:(I)V", ui_noop);
    regN("javax/microedition/lcdui/Canvas.pointerPressed:(II)V", ui_noop);
    regN("javax/microedition/lcdui/Canvas.pointerReleased:(II)V", ui_noop);
    regN("javax/microedition/lcdui/Canvas.pointerDragged:(II)V", ui_noop);

    regN("javax/microedition/lcdui/Display.getDisplay:(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;", d_getDisplay);
    regN("javax/microedition/lcdui/Display.getCurrent:()Ljavax/microedition/lcdui/Displayable;", d_getCurrent);
    regN("javax/microedition/lcdui/Display.setCurrent:(Ljavax/microedition/lcdui/Displayable;)V", d_setCurrent);
    regN("javax/microedition/lcdui/Display.setCurrent:(Ljavax/microedition/lcdui/Alert;Ljavax/microedition/lcdui/Displayable;)V", d_setCurrentAlert);
    regN("javax/microedition/lcdui/Display.setCurrentItem:(Ljavax/microedition/lcdui/Item;)V", d_setCurrentItem);
    regN("javax/microedition/lcdui/Display.isColor:()Z", d_isColor);
    regN("javax/microedition/lcdui/Display.numColors:()I", d_numColors);
    regN("javax/microedition/lcdui/Display.numAlphaLevels:()I", d_numAlpha);
    regN("javax/microedition/lcdui/Display.flashBacklight:(I)V", d_flashBacklight);
    regN("javax/microedition/lcdui/Display.repaint:()V", d_repaint);
    regN("javax/microedition/lcdui/Display.callSerially:(Ljava/lang/Runnable;)V", d_callSerially);
    regN("javax/microedition/lcdui/Display.getBorderlessW:()I", ui_true);
    regN("javax/microedition/lcdui/Display.isFullscreen:()Z", ui_true);

    regN("javax/microedition/midlet/MIDlet.<init>:()V", mid_init);
    regN("javax/microedition/midlet/MIDlet.getAppProperty:(Ljava/lang/String;)Ljava/lang/String;", mid_getAppProperty);
    regN("javax/microedition/midlet/MIDlet.notifyDestroyed:()V", mid_notifyDestroyed);
    regN("javax/microedition/midlet/MIDlet.notifyPaused:()V", mid_notifyPaused);
    regN("javax/microedition/midlet/MIDlet.resumeRequest:()Z", mid_resumeRequest);

    // UI stubs
    regN("javax/microedition/lcdui/Alert.<init>:(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;)V", ui_noop);
    regN("javax/microedition/lcdui/Alert.<init>:(Ljava/lang/String;)V", ui_noop);
    regN("javax/microedition/lcdui/Alert.setTimeout:(I)V", ui_noop);
    regN("javax/microedition/lcdui/Alert.setString:(Ljava/lang/String;)V", ui_noop);
    regN("javax/microedition/lcdui/AlertType.<init>:()V", ui_noop);
    regN("javax/microedition/lcdui/Command.<init>:(Ljava/lang/String;II)V", ui_noop);
    regN("javax/microedition/lcdui/Command.<init>:(Ljava/lang/String;Ljava/lang/String;II)V", ui_noop);
    regN("javax/microedition/lcdui/Command.getLabel:()Ljava/lang/String;", ui_noop);
    regN("javax/microedition/lcdui/Item.getLabel:()Ljava/lang/String;", ui_noop);
    regN("javax/microedition/lcdui/Item.setLabel:(Ljava/lang/String;)V", ui_noop);
    regN("javax/microedition/lcdui/Form.<init>:(Ljava/lang/String;)V", ui_noop);
    regN("javax/microedition/lcdui/Form.<init>:(Ljava/lang/String;Ljavax/microedition/lcdui/Item;)V", ui_noop);
    regN("javax/microedition/lcdui/Form.append:(Ljavax/microedition/lcdui/Item;)I", ui_noop);
    regN("javax/microedition/lcdui/Form.append:(Ljava/lang/String;)I", ui_noop);
    regN("javax/microedition/lcdui/Form.size:()I", ui_noop);
    regN("javax/microedition/lcdui/Form.set:(ILjavax/microedition/lcdui/Item;)V", ui_noop);
    regN("javax/microedition/lcdui/List.<init>:(Ljava/lang/String;I[Ljava/lang/String;Ljavax/microedition/lcdui/Image;)V", ui_noop);
    regN("javax/microedition/lcdui/List.<init>:(Ljava/lang/String;I)V", ui_noop);
    regN("javax/microedition/lcdui/List.setSelectedIndex:(IZ)V", ui_noop);
    regN("javax/microedition/lcdui/List.setSelectCommand:(Ljavax/microedition/lcdui/Command;)V", ui_noop);
    regN("javax/microedition/lcdui/List.append:(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I", ui_noop);
    regN("javax/microedition/lcdui/List.getSelectedIndex:()I", ui_noop);
    regN("javax/microedition/lcdui/List.size:()I", ui_noop);
    regN("javax/microedition/lcdui/TextBox.<init>:(Ljava/lang/String;Ljava/lang/String;II)V", ui_noop);
    regN("javax/microedition/lcdui/TextBox.setString:(Ljava/lang/String;)V", ui_noop);
    regN("javax/microedition/lcdui/TextBox.getString:()Ljava/lang/String;", ui_noop);
    regN("javax/microedition/lcdui/TextField.<init>:(Ljava/lang/String;Ljava/lang/String;II)V", ui_noop);
    regN("javax/microedition/lcdui/TextField.setString:(Ljava/lang/String;)V", ui_noop);
    regN("javax/microedition/lcdui/TextField.getString:()Ljava/lang/String;", ui_noop);
    regN("javax/microedition/lcdui/StringItem.<init>:(Ljava/lang/String;Ljava/lang/String;)V", ui_noop);
    regN("javax/microedition/lcdui/StringItem.setText:(Ljava/lang/String;)V", ui_noop);
    regN("javax/microedition/lcdui/StringItem.getText:()Ljava/lang/String;", ui_noop);
    regN("javax/microedition/lcdui/ImageItem.<init>:(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;)V", ui_noop);
    regN("javax/microedition/lcdui/Gauge.<init>:(Ljava/lang/String;ZII)V", ui_noop);
    regN("javax/microedition/lcdui/Gauge.setValue:(I)V", ui_noop);
    regN("javax/microedition/lcdui/Gauge.getValue:()I", ui_noop);
    regN("javax/microedition/lcdui/ChoiceGroup.<init>:(Ljava/lang/String;I[Ljava/lang/String;Ljavax/microedition/lcdui/Image;)V", ui_noop);
    regN("javax/microedition/lcdui/ChoiceGroup.append:(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I", ui_noop);
    regN("javax/microedition/lcdui/ChoiceGroup.getSelectedIndex:()I", ui_noop);
    regN("javax/microedition/lcdui/ChoiceGroup.size:()I", ui_noop);
    regN("javax/microedition/lcdui/Ticker.<init>:(Ljava/lang/String;)V", ui_noop);
    regN("javax/microedition/lcdui/Displayable.setTitle:(Ljava/lang/String;)V", ui_noop);
    regN("javax/microedition/lcdui/Displayable.getTitle:()Ljava/lang/String;", ui_noop);
    regN("javax/microedition/lcdui/Displayable.addCommand:(Ljavax/microedition/lcdui/Command;)V", ui_noop);
    regN("javax/microedition/lcdui/Displayable.setCommandListener:(Ljavax/microedition/lcdui/CommandListener;)V", ui_noop);
    regN("javax/microedition/lcdui/Displayable.isShown:()Z", ui_true);

    // ---- Handlers MMAPI (stub : pas de son) ----
    regN("javax/microedition/media/Manager.createPlayer:(Ljava/io/InputStream;Ljava/lang/String;)Ljavax/microedition/media/Player;", mp_createPlayerIS);
    regN("javax/microedition/media/Manager.createPlayer:(Ljava/lang/String;)Ljavax/microedition/media/Player;", mp_createPlayerStr);
    regN("javax/microedition/media/Player.realize:()V", mp_noop);
    regN("javax/microedition/media/Player.prefetch:()V", mp_noop);
    regN("javax/microedition/media/Player.start:()V", mp_noop);
    regN("javax/microedition/media/Player.stop:()V", mp_noop);
    regN("javax/microedition/media/Player.deallocate:()V", mp_noop);
    regN("javax/microedition/media/Player.close:()V", mp_noop);
    regN("javax/microedition/media/Player.setLoopCount:(I)V", mp_noop);
    regN("javax/microedition/media/Player.getState:()I", mp_getInt);
    regN("javax/microedition/media/Player.getDuration:()J", mp_getLong);
    regN("javax/microedition/media/Player.getMediaTime:()J", mp_getLong);
    regN("javax/microedition/media/Player.setMediaTime:(J)J", mp_getLong);
    regN("javax/microedition/media/Player.getContentType:()Ljava/lang/String;", mp_getContentType);
    regN("javax/microedition/media/Player.addPlayerListener:(Ljavax/microedition/media/PlayerListener;)V", mp_noop);
    regN("javax/microedition/media/Player.removePlayerListener:(Ljavax/microedition/media/PlayerListener;)V", mp_noop);

    // java/lang/System.out : en Java réel, c'est le VM bootstrap qui
    // l'initialise avant tout <clinit> utilisateur -- les classes natives
    // n'ont pas de <clinit> ici pour le faire. Sans ceci, tout
    // System.out.println(...) plante sur "thisObj NULL" (out reste à sa
    // valeur par défaut = référence nulle).
    {
        ClassInfo *sysCls = rt->classInfoOfName("java/lang/System");
        ClassInfo *psCls = rt->classInfoOfName("java/io/PrintStream");
        const MethodRecord *outField = sysCls ? sysCls->findField("out", "Ljava/io/PrintStream;") : nullptr;
        if (sysCls && psCls && outField)
        {
            Obj *out = rt->heap().newInstance(psCls);
            if (out)
                sysCls->statics[outField->slot] = Value::fromRef(out);
        }
    }
}

} // namespace midp
} // namespace jvm