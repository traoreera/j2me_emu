// midp_graphics.cpp -- Graphics/Font/Image/Canvas/GameCanvas, DirectGraphics, rendu RGB565
// Découpé de l'ancien midp_natives.cpp ; état partagé : midp_internal.h.

#include "midp_internal.h"

namespace jvm
{
    namespace midp
    {
        namespace detail
        {


            static Obj *makeGraphics(uint32_t mode, Obj *buf, int w, int h, int stride)
            {
                Obj *g = makeInstance("javax/microedition/lcdui/Graphics");
                if (!g)
                    return nullptr;
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

            // Le Graphics passé à paint() doit avoir sa translation et son clip
            // réinitialisés par l'AMS AVANT CHAQUE appel (garantie MIDP standard :
            // origine alignée sur le coin haut-gauche du Canvas, clip = tout le
            // canvas, translation = (0,0)) -- ce n'est PAS optionnel ni laissé à
            // l'appréciation de l'appli. g_screenGfx étant un singleton persistant
            // (jamais recréé entre deux paint()), ne réinitialiser que CLIPW/CLIPH
            // ici laissait state->TX/TY et CLIPX/CLIPY s'accumuler d'un appel à
            // l'autre : un jeu qui fait `g.translate(dx,0); dessine; g.translate
            // (-dx,0);` de façon même légèrement asymétrique (ex. un retour
            // anticipé entre les deux translate() sur une branche d'état) voit son
            // décalage de caméra dériver indéfiniment plutôt que repartir de zéro
            // à la frame suivante -- symptôme observé : le même sprite de décor
            // redessiné en dizaines d'exemplaires décalés (le fond d'arène de
            // games/mortalkomb_9moadjwj.jar, un GameCanvas dont paint() n'était
            // jamais appelé avant le correctif ci-dessus -- ce bug préexistait
            // mais n'avait jamais été exercé). Couleur/police ne sont PAS
            // spécifiées comme réinitialisées par le standard MIDP, donc pas
            // touchées ici.
            Obj *screenGraphics()
            {
                if (g_screenGfx)
                {
                    g_screenGfx->cells[G_TX] = Value::fromInt(0);
                    g_screenGfx->cells[G_TY] = Value::fromInt(0);
                    g_screenGfx->cells[G_CLIPX] = Value::fromInt(0);
                    g_screenGfx->cells[G_CLIPY] = Value::fromInt(0);
                    g_screenGfx->cells[G_CLIPW] = Value::fromInt(screenW());
                    g_screenGfx->cells[G_CLIPH] = Value::fromInt(screenH());
                    return g_screenGfx;
                }
                g_screenGfx = makeGraphics(GM_SCREEN_565, nullptr, screenW(), screenH(), screenW());
                return g_screenGfx;
            }

            static Obj *canvasGfx(Obj *gc)
            {
                if (gc && gc->cells[GC_GFX].o)
                    return gc->cells[GC_GFX].o;
                Obj *g = makeGraphics(GM_CANVAS_565, nullptr, screenW(), screenH(), screenW());
                if (gc)
                    gc->cells[GC_GFX] = Value::fromRef(g);
                return g;
            }

            static void hline(Pix &p, int x0, int x1, int y, uint32_t c)
            {
                if (x1 < x0)
                {
                    int t = x0;
                    x0 = x1;
                    x1 = t;
                }
                for (int x = x0; x <= x1; x++)
                    p.put(x, y, c);
            }

            static void vline(Pix &p, int x, int y0, int y1, uint32_t c)
            {
                if (y1 < y0)
                {
                    int t = y0;
                    y0 = y1;
                    y1 = t;
                }
                for (int y = y0; y <= y1; y++)
                    p.put(x, y, c);
            }

            // Font 5x7 (même table que hal/display.cpp).
            static const uint8_t kFont5x7[96][5] = {
                {0x00, 0x00, 0x00, 0x00, 0x00},
                {0x00, 0x00, 0x4F, 0x00, 0x00},
                {0x00, 0x07, 0x00, 0x07, 0x00},
                {0x14, 0x7F, 0x14, 0x7F, 0x14},
                {0x24, 0x2A, 0x7F, 0x2A, 0x12},
                {0x23, 0x13, 0x08, 0x64, 0x62},
                {0x36, 0x49, 0x55, 0x22, 0x50},
                {0x00, 0x05, 0x03, 0x00, 0x00},
                {0x00, 0x1C, 0x22, 0x41, 0x00},
                {0x00, 0x41, 0x22, 0x1C, 0x00},
                {0x14, 0x08, 0x3E, 0x08, 0x14},
                {0x08, 0x08, 0x3E, 0x08, 0x08},
                {0x00, 0x50, 0x30, 0x00, 0x00},
                {0x08, 0x08, 0x08, 0x08, 0x08},
                {0x00, 0x60, 0x60, 0x00, 0x00},
                {0x20, 0x10, 0x08, 0x04, 0x02},
                {0x3E, 0x51, 0x49, 0x45, 0x3E},
                {0x00, 0x42, 0x7F, 0x40, 0x00},
                {0x42, 0x61, 0x51, 0x49, 0x46},
                {0x21, 0x41, 0x45, 0x4B, 0x31},
                {0x18, 0x14, 0x12, 0x7F, 0x10},
                {0x27, 0x45, 0x45, 0x45, 0x39},
                {0x3C, 0x4A, 0x49, 0x49, 0x30},
                {0x01, 0x71, 0x09, 0x05, 0x03},
                {0x36, 0x49, 0x49, 0x49, 0x36},
                {0x06, 0x49, 0x49, 0x29, 0x1E},
                {0x00, 0x14, 0x14, 0x00, 0x00},
                {0x00, 0x40, 0x34, 0x00, 0x00},
                {0x00, 0x08, 0x14, 0x22, 0x41},
                {0x14, 0x14, 0x14, 0x14, 0x14},
                {0x41, 0x22, 0x14, 0x08, 0x00},
                {0x02, 0x01, 0x51, 0x09, 0x06},
                {0x32, 0x49, 0x79, 0x41, 0x3E},
                {0x7E, 0x11, 0x11, 0x11, 0x7E},
                {0x7F, 0x49, 0x49, 0x49, 0x36},
                {0x3E, 0x41, 0x41, 0x41, 0x22},
                {0x7F, 0x41, 0x41, 0x22, 0x1C},
                {0x7F, 0x49, 0x49, 0x49, 0x41},
                {0x7F, 0x09, 0x09, 0x09, 0x01},
                {0x3E, 0x41, 0x49, 0x49, 0x7A},
                {0x7F, 0x08, 0x08, 0x08, 0x7F},
                {0x00, 0x41, 0x7F, 0x41, 0x00},
                {0x20, 0x40, 0x41, 0x3F, 0x01},
                {0x7F, 0x08, 0x14, 0x22, 0x41},
                {0x7F, 0x40, 0x40, 0x40, 0x40},
                {0x7F, 0x02, 0x0C, 0x02, 0x7F},
                {0x7F, 0x04, 0x08, 0x10, 0x7F},
                {0x3E, 0x41, 0x41, 0x41, 0x3E},
                {0x7F, 0x09, 0x09, 0x09, 0x06},
                {0x3E, 0x41, 0x51, 0x21, 0x5E},
                {0x7F, 0x09, 0x19, 0x29, 0x46},
                {0x46, 0x49, 0x49, 0x49, 0x31},
                {0x01, 0x01, 0x7F, 0x01, 0x01},
                {0x3F, 0x40, 0x40, 0x40, 0x3F},
                {0x1F, 0x20, 0x40, 0x20, 0x1F},
                {0x3F, 0x40, 0x38, 0x40, 0x3F},
                {0x63, 0x14, 0x08, 0x14, 0x63},
                {0x07, 0x08, 0x70, 0x08, 0x07},
                {0x61, 0x51, 0x49, 0x45, 0x43},
                {0x00, 0x7F, 0x41, 0x41, 0x00},
                {0x02, 0x04, 0x08, 0x10, 0x20},
                {0x00, 0x41, 0x41, 0x7F, 0x00},
                {0x04, 0x02, 0x01, 0x02, 0x04},
                {0x40, 0x40, 0x40, 0x40, 0x40},
                {0x00, 0x01, 0x02, 0x04, 0x00},
                {0x20, 0x54, 0x54, 0x54, 0x78},
                {0x7F, 0x48, 0x44, 0x44, 0x38},
                {0x38, 0x44, 0x44, 0x44, 0x20},
                {0x38, 0x44, 0x44, 0x48, 0x7F},
                {0x38, 0x54, 0x54, 0x54, 0x18},
                {0x08, 0x7E, 0x09, 0x01, 0x02},
                {0x0C, 0x52, 0x52, 0x52, 0x3E},
                {0x7F, 0x08, 0x04, 0x04, 0x78},
                {0x00, 0x44, 0x7D, 0x40, 0x00},
                {0x20, 0x40, 0x44, 0x3D, 0x00},
                {0x7F, 0x10, 0x28, 0x44, 0x00},
                {0x00, 0x41, 0x7F, 0x40, 0x00},
                {0x7C, 0x04, 0x18, 0x04, 0x78},
                {0x7C, 0x08, 0x04, 0x04, 0x78},
                {0x38, 0x44, 0x44, 0x44, 0x38},
                {0x7C, 0x14, 0x14, 0x14, 0x08},
                {0x08, 0x14, 0x14, 0x18, 0x7C},
                {0x7C, 0x08, 0x04, 0x04, 0x08},
                {0x48, 0x54, 0x54, 0x54, 0x20},
                {0x04, 0x3F, 0x44, 0x40, 0x20},
                {0x3C, 0x40, 0x40, 0x20, 0x7C},
                {0x1C, 0x20, 0x40, 0x20, 0x1C},
                {0x3C, 0x40, 0x30, 0x40, 0x3C},
                {0x44, 0x28, 0x10, 0x28, 0x44},
                {0x0C, 0x50, 0x50, 0x50, 0x3C},
                {0x44, 0x64, 0x54, 0x4C, 0x44},
                {0x00, 0x08, 0x36, 0x41, 0x00},
                {0x00, 0x00, 0x7F, 0x00, 0x00},
                {0x00, 0x41, 0x36, 0x08, 0x00},
                {0x08, 0x04, 0x08, 0x10, 0x08},
            };

            static int strWidth(const char *s)
            {
                int w = 0;
                while (s && *s)
                {
                    w += 6;
                    s++;
                }
                return w;
            }

            // ---------------------------------------------------------------------
            // Graphics natives
            // ---------------------------------------------------------------------

            static void g_setColorI(NativeContext *ctx)
            {
                uint32_t rgb = static_cast<uint32_t>(argInt(ctx, 1)) & 0xFFFFFF;
                ctx->thisObj->cells[G_COLOR] = Value::fromInt(static_cast<int32_t>(0xFF000000 | rgb));
            }
            static void g_setColorRGB(NativeContext *ctx)
            {
                uint32_t r = static_cast<uint32_t>(argInt(ctx, 1)) & 0xFF;
                uint32_t g2 = static_cast<uint32_t>(argInt(ctx, 2)) & 0xFF;
                uint32_t b = static_cast<uint32_t>(argInt(ctx, 3)) & 0xFF;
                ctx->thisObj->cells[G_COLOR] = Value::fromInt(static_cast<int32_t>(0xFF000000 | (r << 16) | (g2 << 8) | b));
            }
            static void g_getColor(NativeContext *ctx)
            {
                setInt(ctx, static_cast<int32_t>(static_cast<uint32_t>(ctx->thisObj->cells[G_COLOR].u) & 0xFFFFFF));
            }
            static void g_setGray(NativeContext *ctx)
            {
                uint32_t v = static_cast<uint32_t>(argInt(ctx, 1)) & 0xFF;
                ctx->thisObj->cells[G_COLOR] = Value::fromInt(static_cast<int32_t>(0xFF000000 | (v << 16) | (v << 8) | v));
            }

            static void g_fillRect(NativeContext *ctx)
            {
                Pix p(ctx->thisObj);
                int x = argInt(ctx, 1), y = argInt(ctx, 2), w = argInt(ctx, 3), h = argInt(ctx, 4);
                if (jvm::jmeDebug())
                    fprintf(stderr, "gfx fillRect x=%d y=%d w=%d h=%d mode=%d\n", x, y, w, h, (int)ctx->thisObj->cells[G_MODE].i);
                for (int yy = 0; yy < h; yy++)
                    for (int xx = 0; xx < w; xx++)
                        p.put(x + xx, y + yy, p.color);
            }
            static void g_drawRect(NativeContext *ctx)
            {
                Pix p(ctx->thisObj);
                int x = argInt(ctx, 1), y = argInt(ctx, 2), w = argInt(ctx, 3), h = argInt(ctx, 4);
                hline(p, x, x + w - 1, y, p.color);
                hline(p, x, x + w - 1, y + h - 1, p.color);
                vline(p, x, y, y + h - 1, p.color);
                vline(p, x + w - 1, y, y + h - 1, p.color);
            }
            static void g_drawLine(NativeContext *ctx)
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
                    if (x0 == x1 && y0 == y1)
                        break;
                    int e2 = 2 * err;
                    if (e2 > -dy)
                    {
                        err -= dy;
                        x0 += sx;
                    }
                    if (e2 < dx)
                    {
                        err += dx;
                        y0 += sy;
                    }
                }
            }
            static void g_fillTriangle(NativeContext *ctx)
            {
                Pix p(ctx->thisObj);
                int x1 = argInt(ctx, 1), y1 = argInt(ctx, 2), x2 = argInt(ctx, 3), y2 = argInt(ctx, 4), x3 = argInt(ctx, 5), y3 = argInt(ctx, 6);
                int minX = x1, maxX = x1;
                if (x2 < minX)
                    minX = x2;
                if (x2 > maxX)
                    maxX = x2;
                if (x3 < minX)
                    minX = x3;
                if (x3 > maxX)
                    maxX = x3;
                int minY = y1, maxY = y1;
                if (y2 < minY)
                    minY = y2;
                if (y2 > maxY)
                    maxY = y2;
                if (y3 < minY)
                    minY = y3;
                if (y3 > maxY)
                    maxY = y3;
                auto area2 = [&](int ax, int ay, int bx, int by, int cx, int cy)
                {
                    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                };
                int a2 = area2(x1, y1, x2, y2, x3, y3);
                if (a2 == 0)
                    return;
                for (int yy = minY; yy <= maxY; yy++)
                    for (int xx = minX; xx <= maxX; xx++)
                    {
                        int w1 = area2(x2, y2, x3, y3, xx, yy);
                        int w2 = area2(x3, y3, x1, y1, xx, yy);
                        int w3 = area2(x1, y1, x2, y2, xx, yy);
                        bool inside = (a2 > 0) ? (w1 >= 0 && w2 >= 0 && w3 >= 0) : (w1 <= 0 && w2 <= 0 && w3 <= 0);
                        if (inside)
                            p.put(xx, yy, p.color);
                    }
            }
            static void g_drawArc(NativeContext *ctx)
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
            static void g_fillArc(NativeContext *ctx)
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
            static void g_fillRoundRect(NativeContext *ctx)
            {
                Pix p(ctx->thisObj);
                int x = argInt(ctx, 1), y = argInt(ctx, 2), w = argInt(ctx, 3), h = argInt(ctx, 4);
                int aw = argInt(ctx, 5), ah = argInt(ctx, 6);
                (void)aw;
                (void)ah;
                for (int yy = y; yy < y + h; yy++)
                    for (int xx = x; xx < x + w; xx++)
                        p.put(xx, yy, p.color);
            }
            static void g_drawRoundRect(NativeContext *ctx)
            {
                Pix p(ctx->thisObj);
                int x = argInt(ctx, 1), y = argInt(ctx, 2), w = argInt(ctx, 3), h = argInt(ctx, 4);
                int aw = argInt(ctx, 5), ah = argInt(ctx, 6);
                (void)aw;
                (void)ah;
                hline(p, x, x + w - 1, y, p.color);
                hline(p, x, x + w - 1, y + h - 1, p.color);
                vline(p, x, y, y + h - 1, p.color);
                vline(p, x + w - 1, y, y + h - 1, p.color);
            }

            static void g_setFont(NativeContext *ctx)
            {
                ctx->thisObj->cells[G_FONT] = Value::fromRef(argRef(ctx, 1));
            }
            static void g_getFont(NativeContext *ctx)
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
            static void g_drawString(NativeContext *ctx)
            {
                if (jvm::jmeDebug())
                    fprintf(stderr, "gfx drawString(this=%p)\n", (void *)ctx->thisObj);
                Pix p(ctx->thisObj);
                Obj *s = argRef(ctx, 1);
                int x = argInt(ctx, 2), y = argInt(ctx, 3), anchor = argInt(ctx, 4);
                const char *text = (s && s->kind == ObjKind::String) ? s->str.c_str() : "";
                int wlen = strWidth(text);
                if (anchor & 0x01)
                    x -= wlen / 2; // HCENTER
                else if (anchor & 0x08)
                    x -= wlen; // RIGHT
                if (anchor & 0x02)
                    y -= 3; // VCENTER
                else if (anchor & 0x20)
                    y -= 6; // BASELINE
                else if (anchor & 0x40)
                    y -= 7; // BOTTOM
                while (*text)
                {
                    unsigned char ch = static_cast<unsigned char>(*text);
                    if (ch < 0x20 || ch > 0x7F)
                        ch = '?';
                    const uint8_t *glyph = kFont5x7[ch - 0x20];
                    for (int col = 0; col < 5; col++)
                        for (int row = 0; row < 7; row++)
                            if (glyph[col] & (1 << row))
                                p.put(x + col, y + row, p.color);
                    x += 6;
                    text++;
                }
            }
            static void g_drawChar(NativeContext *ctx)
            {
                char buf[2] = {static_cast<char>(argInt(ctx, 1)), 0};
                ctx->args[1] = Value::fromRef(g_rt->heap().newString(buf));
                ctx->nargs = 5;
                g_drawString(ctx);
            }
            static void g_drawChars(NativeContext *ctx)
            {
                Obj *ca = argRef(ctx, 1);
                int off = argInt(ctx, 2), len = argInt(ctx, 3);
                int x = argInt(ctx, 4), y = argInt(ctx, 5), anchor = argInt(ctx, 6);
                if (!ca || off < 0)
                    return;
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
            static void g_drawImage(NativeContext *ctx)
            {
                Obj *img = argRef(ctx, 1);
                int x = argInt(ctx, 2), y = argInt(ctx, 3), anchor = argInt(ctx, 4);
                if (jvm::jmeDebug())
                    fprintf(stderr, "gfx drawImage img=%p kind=%d x=%d y=%d anc=%d size=%dx%d\n",
                            (void *)img, img ? (int)img->kind : -1, x, y, anchor,
                            img && img->kind == ObjKind::Instance ? img->cells[IMG_W].i : -1,
                            img && img->kind == ObjKind::Instance ? img->cells[IMG_H].i : -1);
                if (!img || img->kind != ObjKind::Instance)
                    return;
                int iw = img->cells[IMG_W].i, ih = img->cells[IMG_H].i;
                Obj *buf = img->cells[IMG_BUF].o;
                if (!buf)
                    return;
                int ox = 0, oy = 0;
                if (anchor & 0x01)
                    ox = iw / 2;
                else if (anchor & 0x08)
                    ox = iw;
                if (anchor & 0x02)
                    oy = ih / 2;
                // BOTTOM = 0x20, pas 0x08 (RIGHT, un bit HORIZONTAL) -- copié-collé
                // fautif qui laissait oy=0 (comportement TOP) pour tout appel
                // ancré BOTTOM. g_drawRegion (juste en dessous) avait la bonne
                // condition (`& 0x20`) depuis le début, seul g_drawImage avait la
                // faute. Conséquence concrète : `drawImage(bg, w/2, screenH,
                // HCENTER|BOTTOM)` (le patron standard pour une image de fond
                // plein écran ancrée en bas) se retrouvait dessiné entièrement
                // hors-écran (son coin haut-gauche AU LIEU de son coin bas-gauche
                // placé à y=screenH) -- observé sur games/mortalkomb_9moadjwj.jar :
                // `scr_img` (acilis_back.png) chargée avec succès mais jamais
                // visible, écran de menu totalement noir derrière le texte.
                else if (anchor & 0x20)
                    oy = ih;
                // Clamp aux bornes de l'écran réellement configuré (pas un
                // 800x480 en dur) pour que l'image reste au moins partiellement
                // visible même sur un profil de résolution différent.
                int sw = screenW(), sh = screenH();
                if (x > sw)
                    x = sw;
                if (y > sh)
                    y = sh;
                if (x + iw <= 0)
                    x = 1 - iw;
                if (y + ih <= 0)
                    y = 1 - ih;
                Pix p(ctx->thisObj);
                for (int yy = 0; yy < ih; yy++)
                    for (int xx = 0; xx < iw; xx++)
                        p.put(x - ox + xx, y - oy + yy, buf->cells[yy * iw + xx].u);
            }

            static void g_drawRegion(NativeContext *ctx)
            {
                Obj *img = argRef(ctx, 1);
                int xs = argInt(ctx, 2), ys = argInt(ctx, 3);
                int w = argInt(ctx, 4), h = argInt(ctx, 5);
                int tfm = argInt(ctx, 6);
                int x = argInt(ctx, 7), y = argInt(ctx, 8), anchor = argInt(ctx, 9);
                if (jvm::jmeDebug())
                    fprintf(stderr, "gfx drawRegion img=%p src=(%d,%d %dx%d) tfm=%d dst=(%d,%d) anc=%d\n",
                            (void *)img, xs, ys, w, h, tfm, x, y, anchor);
                if (!img || img->kind != ObjKind::Instance)
                    return;
                int iw = img->cells[IMG_W].i, ih = img->cells[IMG_H].i;
                Obj *buf = img->cells[IMG_BUF].o;
                if (!buf)
                    return;
                // Région ramenée dans les bornes de la source (CLDC : découpe au clip source).
                if (xs < 0)
                {
                    w += xs;
                    xs = 0;
                }
                if (ys < 0)
                {
                    h += ys;
                    ys = 0;
                }
                if (w > iw - xs)
                    w = iw - xs;
                if (h > ih - ys)
                    h = ih - ys;
                // Clamp aux bornes de l'écran réellement configuré (cf. même
                // correction dans g_drawImage juste au-dessus).
                {
                    int sw = screenW(), sh = screenH();
                    if (x > sw)
                        x = sw;
                    if (y > sh)
                        y = sh;
                }
                if (x + w <= 0)
                    x = 1 - w;
                if (y + h <= 0)
                    y = 1 - h;
                if (w <= 0 || h <= 0)
                    return;

                // Dimensions du bloc destination : les rotations échangent largeur/hauteur.
                bool swap = (tfm == 5 || tfm == 6 || tfm == 7 || tfm == 4); // ROT90/270, MIRROR_ROT*
                int Wp = swap ? h : w, Hp = swap ? w : h;

                int ox = 0, oy = 0;
                if (anchor & 0x01)
                    ox = Wp / 2;
                else if (anchor & 0x08)
                    ox = Wp;
                if (anchor & 0x02)
                    oy = Hp / 2;
                else if (anchor & 0x20)
                    oy = Hp;
                else if (anchor & 0x40)
                    oy = Hp;

                Pix p(ctx->thisObj);
                for (int dy = 0; dy < Hp; dy++)
                {
                    for (int dx = 0; dx < Wp; dx++)
                    {
                        int sx, sy;
                        switch (tfm)
                        {
                        case 3:
                            sx = w - 1 - dx;
                            sy = h - 1 - dy;
                            break; // ROT180
                        case 2:
                            sx = w - 1 - dx;
                            sy = dy;
                            break; // MIRROR
                        case 1:
                            sx = dx;
                            sy = h - 1 - dy;
                            break; // MIRROR_ROT180
                        case 4:
                            sx = dy;
                            sy = dx;
                            break; // MIRROR_ROT270
                        case 5:
                            sx = dy;
                            sy = w - 1 - dx;
                            break; // ROT90
                        case 6:
                            sx = h - 1 - dy;
                            sy = dx;
                            break; // ROT270
                        case 7:
                            sx = w - 1 - dy;
                            sy = h - 1 - dx;
                            break; // MIRROR_ROT90
                        default:
                            sx = dx;
                            sy = dy;
                            break; // NONE / inconnu
                        }
                        p.put(x - ox + dx, y - oy + dy, buf->cells[(ys + sy) * iw + (xs + sx)].u);
                    }
                }
            }

            static void g_drawRGB(NativeContext *ctx)
            {
                Obj *rgb = argRef(ctx, 1);
                int off = argInt(ctx, 2), scan = argInt(ctx, 3);
                int x = argInt(ctx, 4), y = argInt(ctx, 5), w = argInt(ctx, 6), h = argInt(ctx, 7);
                if (jvm::jmeDebug())
                    fprintf(stderr, "gfx drawRGB rgb=%p x=%d y=%d w=%d h=%d sample=0x%08x\n",
                            (void *)rgb, x, y, w, h,
                            (rgb && rgb->kind == ObjKind::IntArray && rgb->arrayLen > 0) ? rgb->cells[0].u : 0);
                if (!rgb || rgb->kind != ObjKind::IntArray || w <= 0 || h <= 0)
                    return;
                Pix p(ctx->thisObj);
                int idx = off;
                if (scan <= 0)
                    scan = w;
                for (int j = 0; j < h; j++)
                {
                    int row = idx;
                    for (int i = 0; i < w; i++)
                        p.put(x + i, y + j, static_cast<uint32_t>(rgb->cells[row + i].u));
                    idx += scan;
                }
            }
            static void g_setClipXYWH(NativeContext *ctx)
            {
                Obj *g = ctx->thisObj;
                g->cells[G_CLIPX] = Value::fromInt(argInt(ctx, 1));
                g->cells[G_CLIPY] = Value::fromInt(argInt(ctx, 2));
                g->cells[G_CLIPW] = Value::fromInt(argInt(ctx, 3));
                g->cells[G_CLIPH] = Value::fromInt(argInt(ctx, 4));
            }
            static void g_clipRect(NativeContext *ctx)
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
            static void g_getClipX(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_CLIPX].i); }
            static void g_getClipY(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_CLIPY].i); }
            static void g_getClipW(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_CLIPW].i); }
            static void g_getClipH(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_CLIPH].i); }
            static void g_translate(NativeContext *ctx)
            {
                Obj *g = ctx->thisObj;
                g->cells[G_TX] = Value::fromInt(g->cells[G_TX].i + argInt(ctx, 1));
                g->cells[G_TY] = Value::fromInt(g->cells[G_TY].i + argInt(ctx, 2));
            }
            static void g_getTranslateX(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_TX].i); }
            static void g_getTranslateY(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[G_TY].i); }

            // ---------------------------------------------------------------------
            // Font natives
            // ---------------------------------------------------------------------

            static void f_getFont(NativeContext *ctx)
            {
                int face = argInt(ctx, 0), style = argInt(ctx, 1), size = argInt(ctx, 2);
                int si = size == 8 ? 0 : (size == 16 ? 2 : 1);
                int st = style & 0x03;
                Obj *f = g_fontCache[st][si];
                if (!f)
                {
                    f = makeInstance("javax/microedition/lcdui/Font");
                    if (!f)
                    {
                        setRef(ctx, nullptr);
                        return;
                    }
                    f->cells[F_FACE] = Value::fromInt(face);
                    f->cells[F_STYLE] = Value::fromInt(style);
                    f->cells[F_SIZE] = Value::fromInt(size);
                    g_fontCache[st][si] = f;
                }
                setRef(ctx, f);
            }
            static void f_getHeight(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, 7);
            }
            static void f_getBaseline(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, 6);
            }
            static void f_getFace(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[F_FACE].i); }
            static void f_getStyle(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[F_STYLE].i); }
            static void f_getSize(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[F_SIZE].i); }
            static void f_stringWidth(NativeContext *ctx)
            {
                const std::string &s = (argRef(ctx, 1) && argRef(ctx, 1)->kind == ObjKind::String) ? argRef(ctx, 1)->str : *new std::string("");
                setInt(ctx, strWidth(s.c_str()));
            }
            static void f_charWidth(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, 6);
            }
            static void f_charsWidth(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, 6 * argInt(ctx, 3));
            }

            // ---------------------------------------------------------------------
            // Image natives
            // ---------------------------------------------------------------------

            Obj *decodePng(const uint8_t *data, size_t len); // défini plus bas, décode en Image immuable

            static void n_String_getChars(NativeContext *ctx)
            {
                Obj *self = argRef(ctx, 0); // La String elle-même
                int srcBegin = argInt(ctx, 1);
                int srcEnd = argInt(ctx, 2);
                Obj *dst = argRef(ctx, 3); // Le tableau de char destination ([C)
                int dstBegin = argInt(ctx, 4);

                if (!self || self->kind != ObjKind::String || !dst || dst->kind != ObjKind::CharArray)
                {
                    return;
                }

                const std::string &s = self->str;
                int len = srcEnd - srcBegin;

                // Sécurités contre les débordements (similaires aux exceptions Java)
                if (srcBegin < 0 || srcEnd > static_cast<int>(s.size()) || srcBegin > srcEnd)
                    return;
                if (dstBegin < 0 || dstBegin + len > dst->arrayLen)
                    return;

                // Copie des caractères dans les cellules du tableau Java
                for (int i = 0; i < len; i++)
                {
                    // En J2ME, les cellules de tableau stockent des uint64_t/uint32_t sous-jacents (Value.u)
                    dst->cells[dstBegin + i].u = static_cast<uint8_t>(s[srcBegin + i]);
                }
            }

            static Obj *makeImage(int w, int h, bool mutable_, Obj *buf)
            {
                Obj *img = makeInstance("javax/microedition/lcdui/Image");
                if (!img)
                    return nullptr;
                img->cells[IMG_W] = Value::fromInt(w);
                img->cells[IMG_H] = Value::fromInt(h);
                img->cells[IMG_MUT] = Value::fromInt(mutable_ ? 1 : 0);
                img->cells[IMG_BUF] = Value::fromRef(buf);
                img->cells[IMG_GFX] = Value::fromRef(nullptr);
                return img;
            }

            static void img_createWH(NativeContext *ctx)
            {
                int w = argInt(ctx, 0), h = argInt(ctx, 1);
                if (w < 1)
                    w = 1;
                if (h < 1)
                    h = 1;
                Obj *buf = g_rt->heap().newArray(ObjKind::IntArray, w * h);
                if (!buf)
                {
                    g_rt->reportOom();
                    setRef(ctx, nullptr);
                    return;
                }
                for (int i = 0; i < w * h; i++)
                    buf->cells[i].u = 0x00000000;
                setRef(ctx, makeImage(w, h, true, buf));
            }
            static void img_createRGB(NativeContext *ctx)
            {
                Obj *rgb = argRef(ctx, 0);
                int w = argInt(ctx, 1), h = argInt(ctx, 2);
                bool processAlpha = argInt(ctx, 3) != 0;
                if (w < 1)
                    w = 1;
                if (h < 1)
                    h = 1;
                Obj *buf = g_rt->heap().newArray(ObjKind::IntArray, w * h);
                if (!buf)
                {
                    g_rt->reportOom();
                    setRef(ctx, nullptr);
                    return;
                }
                for (int i = 0; i < w * h; i++)
                {
                    uint32_t c = (rgb && i < rgb->arrayLen) ? static_cast<uint32_t>(rgb->cells[i].u) : 0;
                    if (!processAlpha)
                        c |= 0xFF000000;
                    buf->cells[i].u = c;
                }
                setRef(ctx, makeImage(w, h, true, buf));
            }
            static void img_copy(NativeContext *ctx)
            {
                Obj *src = argRef(ctx, 0);
                if (!src || src->kind != ObjKind::Instance)
                {
                    setRef(ctx, nullptr);
                    return;
                }
                int w = src->cells[IMG_W].i, h = src->cells[IMG_H].i;
                Obj *buf = g_rt->heap().newArray(ObjKind::IntArray, w * h);
                if (!buf)
                {
                    g_rt->reportOom();
                    setRef(ctx, nullptr);
                    return;
                }
                Obj *sb = src->cells[IMG_BUF].o;
                for (int i = 0; i < w * h; i++)
                    buf->cells[i].u = sb ? sb->cells[i].u : 0;
                setRef(ctx, makeImage(w, h, true, buf));
            }
            static void img_createString(NativeContext *ctx)
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
                            if (jvm::jmeDebug())
                                fprintf(stderr, "[midp] createImage(\"%s\") : decode OK (%zu octets)\n", path.c_str(), n);
                            setRef(ctx, img);
                            return;
                        }
                    }
                }
                fprintf(stderr, "[midp] createImage(\"%s\") : introuvable ou décodage échoué\n", path.c_str());
                Obj *buf = g_rt->heap().newArray(ObjKind::IntArray, 1);
                if (buf)
                    buf->cells[0].u = 0xFFFFFFFF;
                setRef(ctx, makeImage(1, 1, false, buf));
            }
            static void img_getGraphics(NativeContext *ctx)
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
                if (g)
                    img->cells[IMG_GFX] = Value::fromRef(g);
                setRef(ctx, g);
            }
            static void img_getWidth(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[IMG_W].i); }
            static void img_getHeight(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[IMG_H].i); }
            static void img_isMutable(NativeContext *ctx) { setInt(ctx, ctx->thisObj->cells[IMG_MUT].i); }
            static void img_getRGB(NativeContext *ctx)
            {
                Obj *img = ctx->thisObj;
                Obj *dst = argRef(ctx, 1);
                int off = argInt(ctx, 2), scan = argInt(ctx, 3);
                int sx = argInt(ctx, 4), sy = argInt(ctx, 5);
                int w = argInt(ctx, 6), h = argInt(ctx, 7);
                if (!dst)
                    return;
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

            static void gc_init(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    ctx->thisObj->cells[GC_FULLSCREEN] = Value::fromInt(argInt(ctx, 1));
            }
            static void gc_setFullScreen(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    ctx->thisObj->cells[GC_FULLSCREEN] = Value::fromInt(argInt(ctx, 1));
            }
            static void gc_getGraphics(NativeContext *ctx)
            {
                if (jvm::jmeDebug())
                    fprintf(stderr, "gfx getGraphics(this=%p)\n", (void *)ctx->thisObj);
                setRef(ctx, canvasGfx(ctx->thisObj));
            }
            static void gc_flushGraphics(NativeContext *ctx)
            {
                if (!ctx->thisObj)
                    return;
                g_flushCalls++;
                auto *fb = hal::display_get_framebuffer();
                if (fb && fb->pixels)
                    if (jvm::jmeDebug())
                        fprintf(stderr, "flushGraphics()\n");
                std::memcpy(fb->pixels, g_canvas565, static_cast<size_t>(screenH()) * fb->stride * sizeof(uint16_t));
                hal::display_present(fb);
            }
            static void gc_flushRegion(NativeContext *ctx)
            {
                gc_flushGraphics(ctx);
            }
            static void gc_getKeyStates(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, static_cast<int32_t>(g_keyStates));
            }

            // ---------------------------------------------------------------------
            // javax.microedition.lcdui.game : Layer / Sprite / TiledLayer /
            // LayerManager
            // ---------------------------------------------------------------------
            // Rend une région (xs,ys,w,h) d'un array de pixels source vers la PDE
            // d'un Graphics, en appliquant la même tabl de transform que drawRegion
            // (0=NONE,1=MIRROR_ROT180,2=MIRROR,3=ROT180,4=MIRROR_ROT270,5=ROT90,
            // 6=ROT270,7=MIRROR_ROT90). bp doit être l'zone cible d'un Graphics.
            void drawRegionRaw(Pix &p, Obj *src, int iw, int xs, int ys, int w, int h, int tfm, int dx, int dy)
            {
                if (w <= 0 || h <= 0 || !src)
                    return;
                if (jvm::drawDbg())
                    fprintf(stderr, "DRAW src=%p tw=%d %d,%d %dx%d tfm=%d dst=%d,%d tx=%d ty=%d clip=%d,%d %dx%d\n",
                            (void *)src, iw, xs, ys, w, h, tfm, dx, dy, p.tx, p.ty, p.clipX, p.clipY, p.clipW, p.clipH);
                bool swap = (tfm == 4 || tfm == 5 || tfm == 6 || tfm == 7);
                int Wp = swap ? h : w, Hp = swap ? w : h;
                for (int dyy = 0; dyy < Hp; dyy++)
                {
                    for (int dxx = 0; dxx < Wp; dxx++)
                    {
                        int sx, sy;
                        switch (tfm)
                        {
                        case 3:
                            sx = w - 1 - dxx;
                            sy = h - 1 - dyy;
                            break;
                        case 2:
                            sx = w - 1 - dxx;
                            sy = dyy;
                            break;
                        case 1:
                            sx = dxx;
                            sy = h - 1 - dyy;
                            break;
                        case 4:
                            sx = dyy;
                            sy = dxx;
                            break;
                        case 5:
                            sx = dyy;
                            sy = w - 1 - dxx;
                            break;
                        case 6:
                            sx = h - 1 - dyy;
                            sy = dxx;
                            break;
                        case 7:
                            sx = w - 1 - dyy;
                            sy = h - 1 - dxx;
                            break;
                        default:
                            sx = dxx;
                            sy = dyy;
                            break;
                        }
                        p.put(dx + dxx, dy + dyy, src->cells[(ys + sy) * iw + (xs + sx)].u);
                    }
                }
            }

            // --- com.nokia.mid.ui.DirectGraphics/DirectUtils ---
            // API graphique Nokia utilisée par les jeux S60 (ex. games/mission.jar,
            // GloftMI3) : le canvas est dessiné pixel par pixel via
            // DirectGraphics.drawPixels (pas de Sprite/Image MIDP). DirectUtils
            // fournit le wrapper ; on implémente l'interface dans une classe
            // interne (DirectGraphicsWrapper) résolue au runtime par le receiver
            // (notre invokeVirtual ignore la classe déclarée par l'InterfaceMethodref).

            static void n_dg_getDirectGraphics(NativeContext *ctx)
            {
                Obj *g = argRef(ctx, 0);
                if (!g || !ctx->rt)
                    return;
                Obj *w = g_rt ? g_rt->heap().newInstance(ctx->rt->classInfoOfName("com/nokia/mid/ui/DirectGraphicsWrapper")) : nullptr;
                if (!w)
                    return;
                w->cells[DG_GFX] = Value::fromRef(g);
                setRef(ctx, w);
            }

            static void n_dg_drawPixels(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                Obj *g = self->cells[DG_GFX].o;
                Obj *pix = argRef(ctx, 1);
                if (!g || !pix || pix->kind != ObjKind::ShortArray)
                    return;
                bool transparency = argInt(ctx, 2) != 0;
                int offset = argInt(ctx, 3);
                int scan = argInt(ctx, 4);
                int x = argInt(ctx, 5), y = argInt(ctx, 6);
                int w = argInt(ctx, 7), h = argInt(ctx, 8);
                int manipulation = argInt(ctx, 9);
                int format = argInt(ctx, 10);
                if (w <= 0 || h <= 0 || scan <= 0)
                    return;
                Pix p(g);
                bool rot = (manipulation & 0x40) == 0x40; // ROTATE_90/180/270 de Nokia
                int outW = rot ? h : w, outH = rot ? w : h;
                for (int dy = 0; dy < outH; dy++)
                {
                    for (int dx = 0; dx < outW; dx++)
                    {
                        int sx, sy;
                        switch (manipulation & 0xf0)
                        {
                        case 0x10:
                            sx = w - 1 - dx;
                            sy = dy;
                            break;
                        case 0x20:
                            sx = dx;
                            sy = h - 1 - dy;
                            break;
                        case 0x40:
                            sx = dy;
                            sy = w - 1 - dx;
                            break;
                        case 0x50:
                            sx = h - 1 - dy;
                            sy = w - 1 - dx;
                            break;
                        case 0x60:
                            sx = h - 1 - dy;
                            sy = dx;
                            break;
                        case 0x80:
                            sx = w - 1 - dx;
                            sy = h - 1 - dy;
                            break;
                        case 0x90:
                            sx = dx;
                            sy = h - 1 - dy;
                            break;
                        case 0xa0:
                            sx = w - 1 - dx;
                            sy = dy;
                            break;
                        case 0xc0:
                            sx = h - 1 - dy;
                            sy = dx;
                            break;
                        case 0xd0:
                            sx = h - 1 - dy;
                            sy = w - 1 - dx;
                            break;
                        case 0xe0:
                            sx = dy;
                            sy = w - 1 - dx;
                            break;
                        default:
                            sx = dx;
                            sy = dy;
                            break;
                        }
                        if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                            continue;
                        int si = offset + sy * scan + sx;
                        if (si < 0 || si >= pix->arrayLen)
                            continue;
                        uint32_t v = static_cast<uint32_t>(pix->cells[si].i);
                        uint32_t argb = 0;
                        switch (format)
                        {
                        case 4444: // TYPE_INT_4444 Nokia : 0xARGB 4-4-4-4
                        {
                            uint8_t a = (v >> 12) & 0xF, r = (v >> 8) & 0xF, gg = (v >> 4) & 0xF, b = v & 0xF;
                            argb = (static_cast<uint32_t>(a) * 17u << 24) | (static_cast<uint32_t>(r) * 17u << 16) |
                                   (static_cast<uint32_t>(gg) * 17u << 8) | (static_cast<uint32_t>(b) * 17u);
                            break;
                        }
                        case 565: // TYPE_INT_565 : 0xRRRRRGGGGGGBBBBB
                        {
                            uint8_t r = (v >> 11) & 0x1F, gg = (v >> 5) & 0x3F, b = v & 0x1F;
                            argb = 0xFF000000u | (static_cast<uint32_t>(r) * 255u / 31u << 16) |
                                   (static_cast<uint32_t>(gg) * 255u / 63u << 8) | (static_cast<uint32_t>(b) * 255u / 31u);
                            break;
                        }
                        case 332: // TYPE_INT_332 : 0xRRRGGGBB
                            argb = 0xFF000000u | (((v >> 5) & 7) * 36u << 16) | (((v >> 2) & 7) * 36u << 8) | ((v & 3) * 85u);
                            break;
                        case 888: // TYPE_INT_888 : 0xRRGGBB
                            argb = 0xFF000000u | (v & 0xFFFFFFu);
                            break;
                        default: // 8888 ARGB / tout autre : pixel 32 bits tel quel
                            argb = v;
                            break;
                        }
                        if (!transparency)
                            argb |= 0xFF000000u;
                        p.put(x + dx, y + dy, argb);
                    }
                }
            }

            static void n_dc_createImage(NativeContext *ctx)
            {
                if (!ctx || !ctx->rt)
                    return;
                int w = argInt(ctx, 0);
                int h = argInt(ctx, 1);
                int argb = argInt(ctx, 2);
                Obj *img = g_rt->heap().newInstance(g_rt->classInfoOfName("javax/microedition/lcdui/Image"));
                if (!img)
                    return;
                // Set Image cells: width(0), height(1), mutable(2), buf(3), gfx(4)
                img->cells[0] = Value::fromInt(w);
                img->cells[1] = Value::fromInt(h);
                img->cells[2] = Value::fromInt(1); // mutable
                int bufLen = w * h;
                Obj *bufArr = g_rt->heap().newArray(ObjKind::IntArray, bufLen);
                if (bufArr && bufArr->cells)
                {
                    for (int k = 0; k < bufLen; k++)
                        bufArr->cells[k] = Value::fromInt(0);
                }
                img->cells[3] = bufArr ? Value::fromRef(bufArr) : Value::fromRef(nullptr);
                // cell[4] gfx stays null
                setRef(ctx, img);
            }
            static void n_dg_drawImage(NativeContext *ctx)
            {
                if (!ctx || !ctx->rt)
                    return;
                Obj *srcImg = argRef(ctx, 1);
                if (!srcImg || srcImg->kind != ObjKind::Instance)
                    return;
                Obj *g = ctx->thisObj->cells[DG_GFX].o;
                if (!g)
                    return;
                // Get source image dimensions
                int w = srcImg->cells[0].i;
                int h = srcImg->cells[1].i;
                // Minimal implementation: acknowledge call; actual blitting would follow
                (void)w;
                (void)h;
                setRef(ctx, nullptr);
            }

            // --- décodage PNG vers une Image immuable ---

            Obj *decodePng(const uint8_t *data, size_t len)
            {
                jme::PngHeader hdr;
                if (!jme::pngHeader(data, len, hdr) || hdr.idatLen + hdr.rawLen > 8u * 1024 * 1024)
                    return nullptr;
                size_t wlen = hdr.idatLen + hdr.rawLen;
                uint8_t *work = static_cast<uint8_t *>(std::malloc(wlen));
                if (!work)
                    return nullptr;
                int32_t *pxbuf = static_cast<int32_t *>(std::malloc(static_cast<size_t>(hdr.w) * hdr.h * 4));
                if (!pxbuf)
                {
                    std::free(work);
                    return nullptr;
                }
                bool ok = jme::pngPixels(data, len, hdr, work, wlen, pxbuf);
                std::free(work);
                if (!ok)
                {
                    std::free(pxbuf);
                    return nullptr;
                }
                Obj *px = g_rt->heap().newArray(ObjKind::IntArray, static_cast<int32_t>(hdr.w) * hdr.h);
                if (!px)
                {
                    std::free(pxbuf);
                    return nullptr;
                }
                for (int i = 0; i < hdr.w * hdr.h; i++)
                    px->cells[i].u = static_cast<uint32_t>(pxbuf[i]);
                std::free(pxbuf);
                return makeImage(hdr.w, hdr.h, false, px);
            }

            static void img_createStream(NativeContext *ctx)
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
                if (jvm::jmeDebug())
                    fprintf(stderr, "[midp] Image.createImage(InputStream, %d octets)\n", lim - pos);
                uint8_t *buf = static_cast<uint8_t *>(std::malloc(size_t(lim - pos)));
                if (!buf)
                {
                    setRef(ctx, nullptr);
                    return;
                }
                for (int i = pos; i < lim; i++)
                    buf[i - pos] = static_cast<uint8_t>(data->cells[i].u);
                Obj *img = decodePng(buf, size_t(lim - pos));
                std::free(buf);
                setRef(ctx, img);
            }

            static void img_createBytes(NativeContext *ctx)
            {
                Obj *data = argRef(ctx, 0);
                int off = argInt(ctx, 1), len = argInt(ctx, 2);
                if (!data || data->kind != ObjKind::ByteArray || off < 0 || len < 0 || off + len > data->arrayLen)
                {
                    setRef(ctx, nullptr);
                    return;
                }
                uint8_t *buf = static_cast<uint8_t *>(std::malloc(size_t(len ? len : 1)));
                if (!buf)
                {
                    setRef(ctx, nullptr);
                    return;
                }
                for (int i = 0; i < len; i++)
                    buf[i] = static_cast<uint8_t>(data->cells[off + i].u);
                Obj *img = decodePng(buf, size_t(len));
                std::free(buf);
                setRef(ctx, img);
            }
            void registerGraphicsNatives()
            {
                regN("com/nokia/mid/ui/DirectUtils.getDirectGraphics:(Ljavax/microedition/lcdui/Graphics;)Lcom/nokia/mid/ui/DirectGraphics;", n_dg_getDirectGraphics);
                regN("com/nokia/mid/ui/DirectUtils.createImage:(III)Ljavax/microedition/lcdui/Image;", n_dc_createImage);
                regN("com/nokia/mid/ui/DirectGraphicsWrapper.drawImage:(Ljavax/microedition/lcdui/Image;IIII)V", n_dg_drawImage);
                regN("com/nokia/mid/ui/DirectGraphicsWrapper.drawPixels:([SZIIIIIIII)V", n_dg_drawPixels);
                regN("java/lang/String.getChars:(II[CI)V", n_String_getChars);
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
                regN("javax/microedition/lcdui/Image.getRGB:([IIIIIII)V", img_getRGB);
                regN("javax/microedition/lcdui/game/GameCanvas.<init>:(Z)V", gc_init);
                regN("javax/microedition/lcdui/game/GameCanvas.setFullScreenMode:(Z)V", gc_setFullScreen);
                regN("javax/microedition/lcdui/game/GameCanvas.getGraphics:()Ljavax/microedition/lcdui/Graphics;", gc_getGraphics);
                regN("javax/microedition/lcdui/game/GameCanvas.flushGraphics:()V", gc_flushGraphics);
                regN("javax/microedition/lcdui/game/GameCanvas.flushGraphics:(IIII)V", gc_flushRegion);
                regN("javax/microedition/lcdui/game/GameCanvas.getKeyStates:()I", gc_getKeyStates);
                regN("javax/microedition/lcdui/Canvas.setFullScreenMode:(Z)V", gc_setFullScreen);
                regN("javax/microedition/lcdui/Canvas.getGraphics:()Ljavax/microedition/lcdui/Graphics;", gc_getGraphics);
                regN("javax/microedition/lcdui/Canvas.flushGraphics:()V", gc_flushGraphics);
                regN("javax/microedition/lcdui/Canvas.flushGraphics:(IIII)V", gc_flushRegion);
                regN("javax/microedition/lcdui/Canvas.getKeyStates:()I", gc_getKeyStates);
            }
        } // namespace detail
    } // namespace midp
} // namespace jvm
