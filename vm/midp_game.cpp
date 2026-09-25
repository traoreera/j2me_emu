// midp_game.cpp -- MIDP 2.0 Game API : Layer/Sprite/TiledLayer/LayerManager
// Découpé de l'ancien midp_natives.cpp ; état partagé : midp_internal.h.

#include "midp_internal.h"

namespace jvm
{
    namespace midp
    {
        namespace detail
        {

            static void lay_move(NativeContext *ctx)
            {
                if (!ctx->thisObj)
                    return;
                ctx->thisObj->cells[L_X].i += argInt(ctx, 1);
                ctx->thisObj->cells[L_Y].i += argInt(ctx, 2);
            }
            static void lay_setPosition(NativeContext *ctx)
            {
                if (!ctx->thisObj)
                    return;
                ctx->thisObj->cells[L_X] = Value::fromInt(argInt(ctx, 1));
                ctx->thisObj->cells[L_Y] = Value::fromInt(argInt(ctx, 2));
            }
            static void lay_setVisible(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    ctx->thisObj->cells[L_VIS] = Value::fromInt(argInt(ctx, 1));
            }
            static void lay_isVisible(NativeContext *ctx)
            {
                if (!ctx->thisObj)
                    return;
                setInt(ctx, ctx->thisObj->cells[L_VIS].i);
            }
            static void lay_getX(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    setInt(ctx, ctx->thisObj->cells[L_X].i);
            }
            static void lay_getY(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    setInt(ctx, ctx->thisObj->cells[L_Y].i);
            }
            static void lay_getWidth(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    setInt(ctx, ctx->thisObj->cells[L_W].i);
            }
            static void lay_getHeight(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    setInt(ctx, ctx->thisObj->cells[L_H].i);
            }

            // --- Sprite ---
            // (image) V et (image, fw, fh) V : image=arg1, fw=arg2, fh=arg3.
            static void spr_init(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                Obj *img = argRef(ctx, 1);
                int fw = argInt(ctx, 2), fh = argInt(ctx, 3);
                if (img)
                {
                    if (fw <= 0)
                        fw = img->cells[IMG_W].i;
                    if (fh <= 0)
                        fh = img->cells[IMG_H].i;
                }
                self->cells[SPR_IMG] = Value::fromRef(img);
                self->cells[SPR_FW] = Value::fromInt(fw);
                self->cells[SPR_FH] = Value::fromInt(fh);
                self->cells[SPR_SEQ] = Value::fromRef(nullptr);
                self->cells[SPR_FRAME] = Value::fromInt(0);
                self->cells[SPR_TFM] = Value::fromInt(0);
                self->cells[SPR_RX] = Value::fromInt(0);
                self->cells[SPR_RY] = Value::fromInt(0);
                self->cells[L_X] = Value::fromInt(0);
                self->cells[L_Y] = Value::fromInt(0);
                self->cells[L_W] = Value::fromInt(fw);
                self->cells[L_H] = Value::fromInt(fh);
                self->cells[L_VIS] = Value::fromInt(1);
            }
            static void spr_setFrame(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    ctx->thisObj->cells[SPR_FRAME] = Value::fromInt(argInt(ctx, 1));
            }
            static void spr_getFrame(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    setInt(ctx, ctx->thisObj->cells[SPR_FRAME].i);
            }
            static void spr_nextFrame(NativeContext *ctx)
            {
                if (!ctx->thisObj)
                    return;
                ctx->thisObj->cells[SPR_FRAME].i++;
            }
            static void spr_prevFrame(NativeContext *ctx)
            {
                if (!ctx->thisObj)
                    return;
                ctx->thisObj->cells[SPR_FRAME].i--;
            }
            static void spr_getFrameSequenceLength(NativeContext *ctx)
            {
                if (!ctx->thisObj)
                    return;
                Obj *seq = ctx->thisObj->cells[SPR_SEQ].o;
                if (seq && seq->kind == ObjKind::IntArray)
                    setInt(ctx, seq->arrayLen);
                else
                {
                    Obj *img = ctx->thisObj->cells[SPR_IMG].o;
                    int fw = ctx->thisObj->cells[SPR_FW].i, fh = ctx->thisObj->cells[SPR_FH].i;
                    int rawCount = (img && fw > 0 && fh > 0) ? (img->cells[IMG_W].i / fw) * (img->cells[IMG_H].i / fh) : 1;
                    setInt(ctx, rawCount > 0 ? rawCount : 1);
                }
            }
            static void spr_setFrameSequence(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                Obj *seq = argRef(ctx, 1);
                if (seq && seq->kind == ObjKind::IntArray && seq->arrayLen > 0)
                {
                    self->cells[SPR_SEQ] = Value::fromRef(seq);
                    self->cells[SPR_FRAME] = Value::fromInt(0);
                }
                else
                    self->cells[SPR_SEQ] = Value::fromRef(nullptr);
            }
            static void spr_setTransform(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    ctx->thisObj->cells[SPR_TFM] = Value::fromInt(argInt(ctx, 1));
            }
            static void spr_defineRefPixel(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                self->cells[SPR_RX] = Value::fromInt(argInt(ctx, 1));
                self->cells[SPR_RY] = Value::fromInt(argInt(ctx, 2));
            }
            static void spr_setRefPixelPosition(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                int rx = argInt(ctx, 1), ry = argInt(ctx, 2);
                self->cells[L_X] = Value::fromInt(rx - self->cells[SPR_RX].i);
                self->cells[L_Y] = Value::fromInt(ry - self->cells[SPR_RY].i);
            }
            static void spr_getRefPixelX(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    setInt(ctx, ctx->thisObj->cells[L_X].i + ctx->thisObj->cells[SPR_RX].i);
            }
            static void spr_getRefPixelY(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    setInt(ctx, ctx->thisObj->cells[L_Y].i + ctx->thisObj->cells[SPR_RY].i);
            }
            // Rectangle du sprite dans le monde (top-left de la frame, le pixel
            // de référence est décalé de SPR_RX/SPR_RY par rapport à ce coin).
            static void sprFrameRect(Obj *s, int &x0, int &y0, int &w, int &h)
            {
                Obj *img = s->cells[SPR_IMG].o;
                int fw = s->cells[SPR_FW].i, fh = s->cells[SPR_FH].i;
                int tfm = s->cells[SPR_TFM].i;
                if (img && fw <= 0)
                    fw = img->cells[IMG_W].i;
                if (img && fh <= 0)
                    fh = img->cells[IMG_H].i;
                bool swap = (tfm == 4 || tfm == 5 || tfm == 6 || tfm == 7);
                w = swap ? fh : fw;
                h = swap ? fw : fh;
                x0 = s->cells[L_X].i - s->cells[SPR_RX].i;
                y0 = s->cells[L_Y].i - s->cells[SPR_RY].i;
            }
            static bool rectsOverlap(int x0, int y0, int w0, int h0, int x1, int y1, int w1, int h1)
            {
                return x0 < x1 + w1 && x0 + w0 > x1 && y0 < y1 + h1 && y0 + h0 > y1;
            }
            static void spr_collides(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *other = argRef(ctx, 1);
                if (!self || !other || other->kind != ObjKind::Instance || !isSubclassOf(other, "javax/microedition/lcdui/game/Sprite"))
                {
                    setInt(ctx, 0);
                    return;
                }
                int ax, ay, aw, ah, bx, by, bw, bh;
                sprFrameRect(self, ax, ay, aw, ah);
                sprFrameRect(other, bx, by, bw, bh);
                if (jvm::jmeDebug())
                    fprintf(stderr, "collides self=(%d,%d %dx%d) other=(%d,%d %dx%d) => %d\n",
                            ax, ay, aw, ah, bx, by, bw, bh,
                            rectsOverlap(ax, ay, aw, ah, bx, by, bw, bh) ? 1 : 0);
                setInt(ctx, rectsOverlap(ax, ay, aw, ah, bx, by, bw, bh) ? 1 : 0);
            }
            static void spr_collidesImage(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *img = argRef(ctx, 1);
                int x = argInt(ctx, 2), y = argInt(ctx, 3);
                if (!self || !img || img->kind != ObjKind::Instance)
                {
                    setInt(ctx, 0);
                    return;
                }
                int ax, ay, aw, ah;
                sprFrameRect(self, ax, ay, aw, ah);
                int iw = img->cells[IMG_W].i, ih = img->cells[IMG_H].i;
                setInt(ctx, rectsOverlap(ax, ay, aw, ah, x, y, iw, ih) ? 1 : 0);
            }
            static void spr_collidesTiled(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *tl = argRef(ctx, 1);
                if (!self || !tl || tl->kind != ObjKind::Instance || !isSubclassOf(tl, "javax/microedition/lcdui/game/TiledLayer"))
                {
                    setInt(ctx, 0);
                    return;
                }
                int ax, ay, aw, ah;
                sprFrameRect(self, ax, ay, aw, ah);
                int tw = tl->cells[TL_TW].i, th = tl->cells[TL_TH].i;
                int cols = tl->cells[TL_COLS].i, rows = tl->cells[TL_ROWS].i;
                Obj *grid = tl->cells[TL_GRID].o;
                int bx = tl->cells[L_X].i, by = tl->cells[L_Y].i;
                bool hit = false;
                if (grid && grid->kind == ObjKind::IntArray && tw > 0 && th > 0)
                {
                    for (int r = 0; r < rows && !hit; r++)
                    {
                        for (int c = 0; c < cols && !hit; c++)
                        {
                            if (grid->cells[r * cols + c].i == 0)
                                continue;
                            if (rectsOverlap(ax, ay, aw, ah, bx + c * tw, by + r * th, tw, th))
                                hit = true;
                        }
                    }
                }
                setInt(ctx, hit ? 1 : 0);
            }
            static void spr_paint(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *g = argRef(ctx, 1);
                if (jvm::drawDbg())
                    fprintf(stderr, "SPR self=%p g=%p vis=%d img=%p x=%d y=%d\n", (void *)self, (void *)g,
                            self ? self->cells[L_VIS].i : -1, (void *)(self ? self->cells[SPR_IMG].o : nullptr),
                            self ? self->cells[L_X].i : 0, self ? self->cells[L_Y].i : 0);
                if (!self || !g || !self->cells[L_VIS].i)
                    return;
                Obj *img = self->cells[SPR_IMG].o;
                Obj *buf = (img && img->kind == ObjKind::Instance) ? img->cells[IMG_BUF].o : nullptr;
                if (jvm::drawDbg())
                    fprintf(stderr, "SPR2 img=%p buf=%p iw=%d ih=%d fw=%d fh=%d frame=%d\n", (void *)img, (void *)buf,
                            img ? img->cells[IMG_W].i : -1, img ? img->cells[IMG_H].i : -1,
                            self->cells[SPR_FW].i, self->cells[SPR_FH].i, self->cells[SPR_FRAME].i);
                if (!buf)
                    return;
                int iw = img->cells[IMG_W].i, ih = img->cells[IMG_H].i;
                int fw = self->cells[SPR_FW].i, fh = self->cells[SPR_FH].i;
                if (fw <= 0 || fh <= 0)
                    return;
                int perRow = iw / fw;
                int rawCount = (iw / fw) * (ih / fh);
                if (perRow <= 0 || rawCount <= 0)
                    return;
                int idx = self->cells[SPR_FRAME].i;
                Obj *seq = self->cells[SPR_SEQ].o;
                if (seq && seq->kind == ObjKind::IntArray && seq->arrayLen > 0)
                {
                    int sl = seq->arrayLen;
                    idx = ((idx % sl) + sl) % sl;
                    idx = seq->cells[idx].i;
                    idx = ((idx % rawCount) + rawCount) % rawCount;
                }
                else
                    idx = ((idx % rawCount) + rawCount) % rawCount;
                int xs = (idx % perRow) * fw, ys = (idx / perRow) * fh;
                Pix p(g);
                drawRegionRaw(p, buf, iw, xs, ys, fw, fh, self->cells[SPR_TFM].i,
                              self->cells[L_X].i, self->cells[L_Y].i);
            }

            // --- TiledLayer ---
            // ctor(int cols, int rows, Image, int tileW, int tileH)
            static void tl_init(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                int cols = argInt(ctx, 1), rows = argInt(ctx, 2);
                Obj *img = argRef(ctx, 3);
                int tw = argInt(ctx, 4), th = argInt(ctx, 5);
                self->cells[TL_COLS] = Value::fromInt(cols);
                self->cells[TL_ROWS] = Value::fromInt(rows);
                self->cells[TL_IMG] = Value::fromRef(img);
                self->cells[TL_TW] = Value::fromInt(tw);
                self->cells[TL_TH] = Value::fromInt(th);
                self->cells[L_X] = Value::fromInt(0);
                self->cells[L_Y] = Value::fromInt(0);
                self->cells[L_W] = Value::fromInt(cols * tw);
                self->cells[L_H] = Value::fromInt(rows * th);
                self->cells[L_VIS] = Value::fromInt(1);
                Obj *grid = g_rt ? g_rt->heap().newArray(ObjKind::IntArray, cols * rows) : nullptr;
                self->cells[TL_GRID] = Value::fromRef(grid);
                Obj *anim = g_rt ? g_rt->heap().newArray(ObjKind::IntArray, 16) : nullptr;
                self->cells[TL_ANIM] = Value::fromRef(anim);
            }
            static void tl_setCell(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                int col = argInt(ctx, 1), row = argInt(ctx, 2), t = argInt(ctx, 3);
                Obj *grid = self->cells[TL_GRID].o;
                int cols = self->cells[TL_COLS].i;
                if (!grid || grid->kind != ObjKind::IntArray || col < 0 || row < 0 || col >= cols)
                    return;
                if (row * cols + col < grid->arrayLen)
                    grid->cells[row * cols + col] = Value::fromInt(t);
            }
            static void tl_getCell(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                {
                    setInt(ctx, 0);
                    return;
                }
                int col = argInt(ctx, 1), row = argInt(ctx, 2);
                Obj *grid = self->cells[TL_GRID].o;
                int cols = self->cells[TL_COLS].i;
                if (!grid || grid->kind != ObjKind::IntArray || col < 0 || row < 0 || col >= cols || row * cols + col >= grid->arrayLen)
                {
                    setInt(ctx, 0);
                    return;
                }
                setInt(ctx, grid->cells[row * cols + col].i);
            }
            static void tl_fillCells(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                int col = argInt(ctx, 1), row = argInt(ctx, 2);
                int nc = argInt(ctx, 3), nr = argInt(ctx, 4);
                Obj *grid = self->cells[TL_GRID].o;
                int cols = self->cells[TL_COLS].i, rows = self->cells[TL_ROWS].i;
                if (!grid || grid->kind != ObjKind::IntArray)
                    return;
                for (int r = row; r < row + nr && r < rows; r++)
                    for (int c = col; c < col + nc && c < cols; c++)
                        if (r >= 0 && c >= 0 && r * cols + c < grid->arrayLen)
                            grid->cells[r * cols + c] = Value::fromInt(0);
            }
            static void tl_createAnimatedTile(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                {
                    setInt(ctx, -1);
                    return;
                }
                Obj *anim = self->cells[TL_ANIM].o;
                int t = argInt(ctx, 1);
                if (anim && anim->kind == ObjKind::IntArray)
                {
                    for (int i = 0; i < anim->arrayLen; i++)
                        if (anim->cells[i].i == 0)
                        {
                            anim->cells[i] = Value::fromInt(t);
                            setInt(ctx, -(i + 1));
                            return;
                        }
                }
                setInt(ctx, -1);
            }
            static void tl_setAnimatedTile(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                int a = argInt(ctx, 1);
                int idx = -a - 1;
                Obj *anim = self->cells[TL_ANIM].o;
                if (anim && anim->kind == ObjKind::IntArray && idx >= 0 && idx < anim->arrayLen)
                    anim->cells[idx] = Value::fromInt(argInt(ctx, 2));
            }
            static void tl_paint(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *g = argRef(ctx, 1);
                if (jvm::drawDbg())
                    fprintf(stderr, "TL self=%p vis=%d x=%d y=%d\n", (void *)self,
                            self ? self->cells[L_VIS].i : -1, self ? self->cells[L_X].i : 0, self ? self->cells[L_Y].i : 0);
                if (!self || !g || !self->cells[L_VIS].i)
                    return;
                Obj *img = self->cells[TL_IMG].o;
                Obj *buf = (img && img->kind == ObjKind::Instance) ? img->cells[IMG_BUF].o : nullptr;
                Obj *grid = self->cells[TL_GRID].o;
                Obj *anim = self->cells[TL_ANIM].o;
                if (jvm::drawDbg())
                    fprintf(stderr, "TL2 img=%p buf=%p iw=%d grid=%d cells\n", (void *)img, (void *)buf,
                            img ? img->cells[IMG_W].i : -1, grid && grid->kind == ObjKind::IntArray ? grid->arrayLen : -1);
                if (!buf || !grid || grid->kind != ObjKind::IntArray)
                    return;
                int cols = self->cells[TL_COLS].i, rows = self->cells[TL_ROWS].i;
                int tw = self->cells[TL_TW].i, th = self->cells[TL_TH].i;
                int bx = self->cells[L_X].i, by = self->cells[L_Y].i;
                if (cols <= 0 || rows <= 0 || tw <= 0 || th <= 0)
                    return;
                int iw = img->cells[IMG_W].i;
                int perRow = iw / tw;
                if (perRow <= 0)
                    return;
                Pix p(g);
                for (int r = 0; r < rows; r++)
                {
                    for (int c = 0; c < cols; c++)
                    {
                        int t = grid->cells[r * cols + c].i;
                        if (t == 0)
                            continue;
                        if (t < 0)
                        {
                            int idx = -t - 1;
                            if (!anim || anim->kind != ObjKind::IntArray || idx < 0 || idx >= anim->arrayLen)
                                continue;
                            t = anim->cells[idx].i;
                            if (t == 0)
                                continue;
                        }
                        int xs = (t % perRow) * tw, ys = (t / perRow) * th;
                        drawRegionRaw(p, buf, iw, xs, ys, tw, th, 0,
                                      bx + c * tw, by + r * th);
                    }
                }
            }

            // --- LayerManager ---
            static void lm_init(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                self->cells[LM_LAYERS] = Value::fromRef(nullptr);
                self->cells[LM_COUNT] = Value::fromInt(0);
                self->cells[LM_CAP] = Value::fromInt(0);
                self->cells[LM_VX] = Value::fromInt(0);
                self->cells[LM_VY] = Value::fromInt(0);
                self->cells[LM_VW] = Value::fromInt(screenW());
                self->cells[LM_VH] = Value::fromInt(screenH());
            }
            static void lm_append(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                Obj *layer = argRef(ctx, 1);
                if (!layer || layer->kind != ObjKind::Instance)
                    return;
                int count = self->cells[LM_COUNT].i, cap = self->cells[LM_CAP].i;
                Obj *layers = self->cells[LM_LAYERS].o;
                if (count >= cap || !layers)
                {
                    int ncap = cap ? cap * 2 : 8;
                    Obj *narr = g_rt ? g_rt->heap().newArray(ObjKind::ObjArray, ncap) : nullptr;
                    if (!narr)
                        return;
                    if (layers && layers->kind == ObjKind::ObjArray)
                        for (int i = 0; i < cap && i < layers->arrayLen; i++)
                            narr->cells[i] = layers->cells[i];
                    self->cells[LM_LAYERS] = Value::fromRef(narr);
                    self->cells[LM_CAP] = Value::fromInt(ncap);
                    layers = narr;
                }
                layers->cells[count] = Value::fromRef(layer);
                self->cells[LM_COUNT] = Value::fromInt(count + 1);
            }
            static void lm_setViewWindow(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                self->cells[LM_VX] = Value::fromInt(argInt(ctx, 1));
                self->cells[LM_VY] = Value::fromInt(argInt(ctx, 2));
                self->cells[LM_VW] = Value::fromInt(argInt(ctx, 3));
                self->cells[LM_VH] = Value::fromInt(argInt(ctx, 4));
            }
            static void lm_paint(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *g = argRef(ctx, 1);
                if (jvm::drawDbg())
                    fprintf(stderr, "LM self=%p g=%p count=%d x=%d y=%d\n", (void *)self, (void *)g,
                            self ? self->cells[LM_COUNT].i : -1, argInt(ctx, 2), argInt(ctx, 3));
                if (!self || !g || g->kind != ObjKind::Instance)
                    return;
                int x = argInt(ctx, 2), y = argInt(ctx, 3);
                Obj *layers = self->cells[LM_LAYERS].o;
                int count = self->cells[LM_COUNT].i;
                int vx = self->cells[LM_VX].i, vy = self->cells[LM_VY].i;
                int vw = self->cells[LM_VW].i, vh = self->cells[LM_VH].i;

                // Translation temporaire : le monde est décalé de -view + (x,y).
                int saveTx = g->cells[G_TX].i, saveTy = g->cells[G_TY].i;
                int saveCx = g->cells[G_CLIPX].i, saveCy = g->cells[G_CLIPY].i;
                int saveCw = g->cells[G_CLIPW].i, saveCh = g->cells[G_CLIPH].i;
                g->cells[G_TX] = Value::fromInt(saveTx + (vx - x));
                g->cells[G_TY] = Value::fromInt(saveTy + (vy - y));
                int nx = saveCx > x ? saveCx : x;
                int ny = saveCy > y ? saveCy : y;
                int ex = (saveCx + saveCw) < (x + vw) ? (saveCx + saveCw) : (x + vw);
                int ey = (saveCy + saveCh) < (y + vh) ? (saveCy + saveCh) : (y + vh);
                g->cells[G_CLIPX] = Value::fromInt(nx);
                g->cells[G_CLIPY] = Value::fromInt(ny);
                g->cells[G_CLIPW] = Value::fromInt(ex - nx > 0 ? ex - nx : 0);
                g->cells[G_CLIPH] = Value::fromInt(ey - ny > 0 ? ey - ny : 0);
                if (layers && layers->kind == ObjKind::ObjArray)
                {
                    Value args[2];
                    args[0] = Value::fromRef(g);
                    // Index 0 = calque le plus AU-DESSUS (MIDP LayerManager) : on peint
                    // donc du dernier au premier, le premier étant dessiné en dernier.
                    for (int i = (count < layers->arrayLen ? count : layers->arrayLen) - 1; i >= 0; i--)
                    {
                        Obj *layer = layers->cells[i].o;
                        if (!layer || layer->kind != ObjKind::Instance)
                            continue;
                        Value res;
                        Value argx[2];
                        argx[0] = Value::fromRef(layer);
                        argx[1] = Value::fromRef(g);
                        g_interp->invokeVirtual(layer->cls, "paint", "(Ljavax/microedition/lcdui/Graphics;)V", layer, argx, 2, res);
                    }
                }
                g->cells[G_TX] = Value::fromInt(saveTx);
                g->cells[G_TY] = Value::fromInt(saveTy);
                g->cells[G_CLIPX] = Value::fromInt(saveCx);
                g->cells[G_CLIPY] = Value::fromInt(saveCy);
                g->cells[G_CLIPW] = Value::fromInt(saveCw);
                g->cells[G_CLIPH] = Value::fromInt(saveCh);
            }
            // Retire `layer` du LayerManager s'il y est (décale les suivants) ;
            // renvoie l'index qu'il occupait, ou -1.
            static int lmRemoveLayer(Obj *self, Obj *layer)
            {
                Obj *layers = self->cells[LM_LAYERS].o;
                int count = self->cells[LM_COUNT].i;
                if (!layers || layers->kind != ObjKind::ObjArray)
                    return -1;
                for (int i = 0; i < count && i < layers->arrayLen; i++)
                {
                    if (layers->cells[i].o == layer)
                    {
                        for (int j = i; j + 1 < count; j++)
                            layers->cells[j] = layers->cells[j + 1];
                        layers->cells[count - 1] = Value::fromRef(nullptr);
                        self->cells[LM_COUNT] = Value::fromInt(count - 1);
                        return i;
                    }
                }
                return -1;
            }
            // insert(layer, index) : place `layer` à `index` (0 = dessus). Un calque
            // déjà présent est d'abord retiré (spec MIDP : il est déplacé).
            static void lm_insert(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *layer = argRef(ctx, 1);
                int idx = argInt(ctx, 2);
                if (!self || !layer || layer->kind != ObjKind::Instance)
                    return;
                lmRemoveLayer(self, layer);
                int count = self->cells[LM_COUNT].i;
                if (idx < 0) idx = 0;
                if (idx > count) idx = count;
                // On réutilise lm_append pour la (ré)allocation, puis on décale.
                lm_append(ctx);
                Obj *layers = self->cells[LM_LAYERS].o;
                if (!layers || self->cells[LM_COUNT].i != count + 1)
                    return;
                for (int j = count; j > idx; j--)
                    layers->cells[j] = layers->cells[j - 1];
                layers->cells[idx] = Value::fromRef(layer);
            }
            static void lm_remove(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *layer = argRef(ctx, 1);
                if (self && layer)
                    lmRemoveLayer(self, layer);
            }
            static void lm_getSize(NativeContext *ctx)
            {
                setInt(ctx, ctx->thisObj ? ctx->thisObj->cells[LM_COUNT].i : 0);
            }
            static void lm_getLayerAt(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                int idx = argInt(ctx, 1);
                Obj *layers = self ? self->cells[LM_LAYERS].o : nullptr;
                if (!layers || idx < 0 || idx >= self->cells[LM_COUNT].i)
                {
                    setRef(ctx, nullptr);
                    return;
                }
                setRef(ctx, layers->cells[idx].o);
            }
            void registerGameNatives()
            {
                regN("javax/microedition/lcdui/game/Layer.move:(II)V", lay_move);
                regN("javax/microedition/lcdui/game/Layer.setPosition:(II)V", lay_setPosition);
                regN("javax/microedition/lcdui/game/Layer.setVisible:(Z)V", lay_setVisible);
                regN("javax/microedition/lcdui/game/Layer.isVisible:()Z", lay_isVisible);
                regN("javax/microedition/lcdui/game/Layer.getX:()I", lay_getX);
                regN("javax/microedition/lcdui/game/Layer.getY:()I", lay_getY);
                regN("javax/microedition/lcdui/game/Layer.getWidth:()I", lay_getWidth);
                regN("javax/microedition/lcdui/game/Layer.getHeight:()I", lay_getHeight);
                regN("javax/microedition/lcdui/game/Sprite.<init>:(Ljavax/microedition/lcdui/Image;)V", spr_init);
                regN("javax/microedition/lcdui/game/Sprite.<init>:(Ljavax/microedition/lcdui/Image;II)V", spr_init);
                regN("javax/microedition/lcdui/game/Sprite.paint:(Ljavax/microedition/lcdui/Graphics;)V", spr_paint);
                regN("javax/microedition/lcdui/game/Sprite.move:(II)V", lay_move);
                regN("javax/microedition/lcdui/game/Sprite.setPosition:(II)V", lay_setPosition);
                regN("javax/microedition/lcdui/game/Sprite.setVisible:(Z)V", lay_setVisible);
                regN("javax/microedition/lcdui/game/Sprite.isVisible:()Z", lay_isVisible);
                regN("javax/microedition/lcdui/game/Sprite.getX:()I", lay_getX);
                regN("javax/microedition/lcdui/game/Sprite.getY:()I", lay_getY);
                regN("javax/microedition/lcdui/game/Sprite.getWidth:()I", lay_getWidth);
                regN("javax/microedition/lcdui/game/Sprite.getHeight:()I", lay_getHeight);
                regN("javax/microedition/lcdui/game/Sprite.setFrame:(I)V", spr_setFrame);
                regN("javax/microedition/lcdui/game/Sprite.getFrame:()I", spr_getFrame);
                regN("javax/microedition/lcdui/game/Sprite.nextFrame:()V", spr_nextFrame);
                regN("javax/microedition/lcdui/game/Sprite.prevFrame:()V", spr_prevFrame);
                regN("javax/microedition/lcdui/game/Sprite.getFrameSequenceLength:()I", spr_getFrameSequenceLength);
                regN("javax/microedition/lcdui/game/Sprite.setFrameSequence:([I)V", spr_setFrameSequence);
                regN("javax/microedition/lcdui/game/Sprite.setTransform:(I)V", spr_setTransform);
                regN("javax/microedition/lcdui/game/Sprite.defineReferencePixel:(II)V", spr_defineRefPixel);
                regN("javax/microedition/lcdui/game/Sprite.collidesWith:(Ljavax/microedition/lcdui/game/Sprite;Z)Z", spr_collides);
                regN("javax/microedition/lcdui/game/Sprite.collidesWith:(Ljavax/microedition/lcdui/Image;IZ)Z", spr_collidesImage);
                regN("javax/microedition/lcdui/game/Sprite.collidesWith:(Ljavax/microedition/lcdui/game/TiledLayer;Z)Z", spr_collidesTiled);
                regN("javax/microedition/lcdui/game/Sprite.setRefPixelPosition:(II)V", spr_setRefPixelPosition);
                regN("javax/microedition/lcdui/game/Sprite.getRefPixelX:()I", spr_getRefPixelX);
                regN("javax/microedition/lcdui/game/Sprite.getRefPixelY:()I", spr_getRefPixelY);
                regN("javax/microedition/lcdui/game/TiledLayer.<init>:(IILjavax/microedition/lcdui/Image;II)V", tl_init);
                regN("javax/microedition/lcdui/game/TiledLayer.paint:(Ljavax/microedition/lcdui/Graphics;)V", tl_paint);
                regN("javax/microedition/lcdui/game/TiledLayer.move:(II)V", lay_move);
                regN("javax/microedition/lcdui/game/TiledLayer.setPosition:(II)V", lay_setPosition);
                regN("javax/microedition/lcdui/game/TiledLayer.setVisible:(Z)V", lay_setVisible);
                regN("javax/microedition/lcdui/game/TiledLayer.isVisible:()Z", lay_isVisible);
                regN("javax/microedition/lcdui/game/TiledLayer.getX:()I", lay_getX);
                regN("javax/microedition/lcdui/game/TiledLayer.getY:()I", lay_getY);
                regN("javax/microedition/lcdui/game/TiledLayer.getWidth:()I", lay_getWidth);
                regN("javax/microedition/lcdui/game/TiledLayer.getHeight:()I", lay_getHeight);
                regN("javax/microedition/lcdui/game/TiledLayer.setCell:(III)V", tl_setCell);
                regN("javax/microedition/lcdui/game/TiledLayer.getCell:(II)I", tl_getCell);
                regN("javax/microedition/lcdui/game/TiledLayer.fillCells:(IIII)V", tl_fillCells);
                regN("javax/microedition/lcdui/game/TiledLayer.createAnimatedTile:(I)I", tl_createAnimatedTile);
                regN("javax/microedition/lcdui/game/TiledLayer.setAnimatedTile:(II)V", tl_setAnimatedTile);
                regN("javax/microedition/lcdui/game/LayerManager.<init>:()V", lm_init);
                regN("javax/microedition/lcdui/game/LayerManager.append:(Ljavax/microedition/lcdui/game/Layer;)V", lm_append);
                regN("javax/microedition/lcdui/game/LayerManager.insert:(Ljavax/microedition/lcdui/game/Layer;I)V", lm_insert);
                regN("javax/microedition/lcdui/game/LayerManager.getSize:()I", lm_getSize);
                regN("javax/microedition/lcdui/game/LayerManager.getLayerAt:(I)Ljavax/microedition/lcdui/game/Layer;", lm_getLayerAt);
                regN("javax/microedition/lcdui/game/LayerManager.remove:(Ljavax/microedition/lcdui/game/Layer;)V", lm_remove);
                regN("javax/microedition/lcdui/game/LayerManager.setViewWindow:(IIII)V", lm_setViewWindow);
                regN("javax/microedition/lcdui/game/LayerManager.paint:(Ljavax/microedition/lcdui/Graphics;II)V", lm_paint);
            }
        } // namespace detail
    } // namespace midp
} // namespace jvm
