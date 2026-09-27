// midp_game.cpp -- MIDP 2.0 Game API : Layer/Sprite/TiledLayer/LayerManager
// Découpé de l'ancien midp_natives.cpp ; état partagé : midp_internal.h.

#include "midp/midp_internal.h"

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

            // --- Sprite ---------------------------------------------------------------------------
            // Modèle MIDP 2.0 : (L_X,L_Y) = coin haut-gauche de la frame TRANSFORMÉE dans le repère
            // du peintre ; (SPR_RX,SPR_RY) = pixel de référence dans la frame NON transformée ;
            // sa position transformée est recalculée à chaque transformation, et setTransform garde
            // le pixel de référence immobile à l'écran (spec : la position du sprite est ajustée).
            static int sprRawCount(Obj *s)
            {
                Obj *img = s->cells[SPR_IMG].o;
                int fw = s->cells[SPR_FW].i, fh = s->cells[SPR_FH].i;
                if (!img || fw <= 0 || fh <= 0)
                    return 1;
                int n = (img->cells[IMG_W].i / fw) * (img->cells[IMG_H].i / fh);
                return n > 0 ? n : 1;
            }
            static int sprSeqLen(Obj *s)
            {
                Obj *seq = s->cells[SPR_SEQ].o;
                return (seq && seq->kind == ObjKind::IntArray && seq->arrayLen > 0) ? seq->arrayLen : sprRawCount(s);
            }
            // Point (px,py) de la frame source (fw x fh) -> position dans la frame transformée.
            static void tfmPoint(int tfm, int fw, int fh, int px, int py, int &ox, int &oy)
            {
                switch (tfm)
                {
                case 1: ox = px; oy = fh - 1 - py; break;
                case 2: ox = fw - 1 - px; oy = py; break;
                case 3: ox = fw - 1 - px; oy = fh - 1 - py; break;
                case 4: ox = py; oy = px; break;
                case 5: ox = fh - 1 - py; oy = px; break;
                case 6: ox = py; oy = fw - 1 - px; break;
                case 7: ox = fh - 1 - py; oy = fw - 1 - px; break;
                default: ox = px; oy = py; break;
                }
            }
            // Inverse : point (lx,ly) de la frame transformée -> pixel de la frame source.
            static void tfmInverse(int tfm, int fw, int fh, int lx, int ly, int &sx, int &sy)
            {
                switch (tfm)
                {
                case 1: sx = lx; sy = fh - 1 - ly; break;
                case 2: sx = fw - 1 - lx; sy = ly; break;
                case 3: sx = fw - 1 - lx; sy = fh - 1 - ly; break;
                case 4: sx = ly; sy = lx; break;
                case 5: sx = ly; sy = fh - 1 - lx; break;
                case 6: sx = fw - 1 - ly; sy = lx; break;
                case 7: sx = fw - 1 - ly; sy = fh - 1 - lx; break;
                default: sx = lx; sy = ly; break;
                }
            }
            // Rectangle (x,y,w,h) de la frame source -> rectangle dans la frame transformée.
            static void tfmRect(int tfm, int fw, int fh, int x, int y, int w, int h, int &ox, int &oy, int &ow, int &oh)
            {
                switch (tfm)
                {
                case 1: ox = x; oy = fh - y - h; ow = w; oh = h; break;
                case 2: ox = fw - x - w; oy = y; ow = w; oh = h; break;
                case 3: ox = fw - x - w; oy = fh - y - h; ow = w; oh = h; break;
                case 4: ox = y; oy = x; ow = h; oh = w; break;
                case 5: ox = fh - y - h; oy = x; ow = h; oh = w; break;
                case 6: ox = y; oy = fw - x - w; ow = h; oh = w; break;
                case 7: ox = fh - y - h; oy = fw - x - w; ow = h; oh = w; break;
                default: ox = x; oy = y; ow = w; oh = h; break;
                }
            }
            static void sprSyncSize(Obj *s)
            {
                int tfm = s->cells[SPR_TFM].i;
                bool swap = (tfm >= 4 && tfm <= 7);
                s->cells[L_W] = Value::fromInt(swap ? s->cells[SPR_FH].i : s->cells[SPR_FW].i);
                s->cells[L_H] = Value::fromInt(swap ? s->cells[SPR_FW].i : s->cells[SPR_FH].i);
            }
            static void sprRefT(Obj *s, int &rx, int &ry)
            {
                tfmPoint(s->cells[SPR_TFM].i, s->cells[SPR_FW].i, s->cells[SPR_FH].i, s->cells[SPR_RX].i, s->cells[SPR_RY].i, rx, ry);
            }
            // (image) V, (image, fw, fh) V
            static void spr_init(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                Obj *img = argRef(ctx, 1);
                int fw = argInt(ctx, 2), fh = argInt(ctx, 3);
                if (ctx->nargs <= 2 || (fw <= 0 && fh <= 0))
                {
                    fw = img ? img->cells[IMG_W].i : 0;
                    fh = img ? img->cells[IMG_H].i : 0;
                }
                else if (img)
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
                self->cells[SPR_CSET] = Value::fromInt(0);
                self->cells[L_X] = Value::fromInt(0);
                self->cells[L_Y] = Value::fromInt(0);
                self->cells[L_VIS] = Value::fromInt(1);
                sprSyncSize(self);
            }
            static void spr_initCopy(NativeContext *ctx) // Sprite(Sprite s)
            {
                Obj *self = ctx->thisObj, *o = argRef(ctx, 1);
                if (!self || !o || o->kind != ObjKind::Instance)
                    return;
                for (int i = 0; i < 18 && i < self->cellCount && i < o->cellCount; i++)
                    self->cells[i] = o->cells[i];
            }
            static void spr_setFrame(NativeContext *ctx)
            {
                Obj *s = ctx->thisObj;
                if (!s)
                    return;
                int n = sprSeqLen(s), f = argInt(ctx, 1);
                s->cells[SPR_FRAME] = Value::fromInt(((f % n) + n) % n);
            }
            static void spr_getFrame(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    setInt(ctx, ctx->thisObj->cells[SPR_FRAME].i);
            }
            static void spr_nextFrame(NativeContext *ctx)
            {
                Obj *s = ctx->thisObj;
                if (!s)
                    return;
                int n = sprSeqLen(s);
                s->cells[SPR_FRAME] = Value::fromInt((s->cells[SPR_FRAME].i + 1) % n);
            }
            static void spr_prevFrame(NativeContext *ctx)
            {
                Obj *s = ctx->thisObj;
                if (!s)
                    return;
                int n = sprSeqLen(s);
                s->cells[SPR_FRAME] = Value::fromInt((s->cells[SPR_FRAME].i + n - 1) % n);
            }
            static void spr_getFrameSequenceLength(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    setInt(ctx, sprSeqLen(ctx->thisObj));
            }
            static void spr_getRawFrameCount(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    setInt(ctx, sprRawCount(ctx->thisObj));
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
                    self->cells[SPR_SEQ] = Value::fromRef(nullptr); // séquence par défaut, frame courante conservée
            }
            static void spr_setImage(NativeContext *ctx)
            {
                Obj *s = ctx->thisObj, *img = argRef(ctx, 1);
                if (!s || !img)
                    return;
                int fw = argInt(ctx, 2), fh = argInt(ctx, 3);
                int oldFw = s->cells[SPR_FW].i, oldFh = s->cells[SPR_FH].i, oldRaw = sprRawCount(s);
                s->cells[SPR_IMG] = Value::fromRef(img);
                s->cells[SPR_FW] = Value::fromInt(fw);
                s->cells[SPR_FH] = Value::fromInt(fh);
                if (fw != oldFw || fh != oldFh)
                    s->cells[SPR_CSET] = Value::fromInt(0); // rectangle de collision remis à la frame entière
                if (sprRawCount(s) < oldRaw)
                {
                    s->cells[SPR_FRAME] = Value::fromInt(0);
                    s->cells[SPR_SEQ] = Value::fromRef(nullptr);
                }
                sprSyncSize(s);
            }
            static void spr_setTransform(NativeContext *ctx)
            {
                Obj *s = ctx->thisObj;
                if (!s)
                    return;
                int nt = argInt(ctx, 1);
                if (nt < 0 || nt > 7)
                    return;
                int orx, ory;
                sprRefT(s, orx, ory);
                int refX = s->cells[L_X].i + orx, refY = s->cells[L_Y].i + ory; // pixel de référence à l'écran
                s->cells[SPR_TFM] = Value::fromInt(nt);
                int nrx, nry;
                sprRefT(s, nrx, nry);
                s->cells[L_X] = Value::fromInt(refX - nrx);
                s->cells[L_Y] = Value::fromInt(refY - nry);
                sprSyncSize(s);
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
                int rx, ry;
                sprRefT(self, rx, ry);
                self->cells[L_X] = Value::fromInt(argInt(ctx, 1) - rx);
                self->cells[L_Y] = Value::fromInt(argInt(ctx, 2) - ry);
            }
            static void spr_getRefPixelX(NativeContext *ctx)
            {
                if (!ctx->thisObj)
                    return;
                int rx, ry;
                sprRefT(ctx->thisObj, rx, ry);
                setInt(ctx, ctx->thisObj->cells[L_X].i + rx);
            }
            static void spr_getRefPixelY(NativeContext *ctx)
            {
                if (!ctx->thisObj)
                    return;
                int rx, ry;
                sprRefT(ctx->thisObj, rx, ry);
                setInt(ctx, ctx->thisObj->cells[L_Y].i + ry);
            }
            static void spr_defineCollisionRectangle(NativeContext *ctx)
            {
                Obj *s = ctx->thisObj;
                if (!s)
                    return;
                s->cells[SPR_CX] = Value::fromInt(argInt(ctx, 1));
                s->cells[SPR_CY] = Value::fromInt(argInt(ctx, 2));
                s->cells[SPR_CW] = Value::fromInt(argInt(ctx, 3));
                s->cells[SPR_CH] = Value::fromInt(argInt(ctx, 4));
                s->cells[SPR_CSET] = Value::fromInt(1);
            }
            // Rectangle de collision dans le repère du peintre (transformation incluse).
            static void sprCollRect(Obj *s, int &x, int &y, int &w, int &h)
            {
                int fw = s->cells[SPR_FW].i, fh = s->cells[SPR_FH].i, tfm = s->cells[SPR_TFM].i;
                int cx = 0, cy = 0, cw = fw, ch = fh;
                if (s->cells[SPR_CSET].i)
                {
                    cx = s->cells[SPR_CX].i;
                    cy = s->cells[SPR_CY].i;
                    cw = s->cells[SPR_CW].i;
                    ch = s->cells[SPR_CH].i;
                }
                int ox, oy, ow, oh;
                tfmRect(tfm, fw, fh, cx, cy, cw, ch, ox, oy, ow, oh);
                x = s->cells[L_X].i + ox;
                y = s->cells[L_Y].i + oy;
                w = ow;
                h = oh;
            }
            static bool rectsOverlap(int x0, int y0, int w0, int h0, int x1, int y1, int w1, int h1)
            {
                return x0 < x1 + w1 && x0 + w0 > x1 && y0 < y1 + h1 && y0 + h0 > y1;
            }
            // Pixel non transparent du sprite `s` au point (wx,wy) du repère du peintre (frame courante).
            static bool sprOpaqueAt(Obj *s, int wx, int wy)
            {
                Obj *img = s->cells[SPR_IMG].o;
                Obj *buf = (img && img->kind == ObjKind::Instance) ? img->cells[IMG_BUF].o : nullptr;
                int fw = s->cells[SPR_FW].i, fh = s->cells[SPR_FH].i;
                if (!buf || fw <= 0 || fh <= 0)
                    return true;
                int lx = wx - s->cells[L_X].i, ly = wy - s->cells[L_Y].i;
                int tw = s->cells[L_W].i, th = s->cells[L_H].i;
                if (lx < 0 || ly < 0 || lx >= tw || ly >= th)
                    return false;
                int sx, sy;
                tfmInverse(s->cells[SPR_TFM].i, fw, fh, lx, ly, sx, sy);
                int iw = img->cells[IMG_W].i, perRow = iw / fw, raw = sprRawCount(s);
                int idx = s->cells[SPR_FRAME].i;
                Obj *seq = s->cells[SPR_SEQ].o;
                if (seq && seq->kind == ObjKind::IntArray && seq->arrayLen > 0)
                {
                    idx = ((idx % seq->arrayLen) + seq->arrayLen) % seq->arrayLen;
                    idx = seq->cells[idx].i;
                }
                idx = ((idx % raw) + raw) % raw;
                if (perRow <= 0)
                    return true;
                int px = (idx % perRow) * fw + sx, py = (idx / perRow) * fh + sy;
                size_t off = static_cast<size_t>(py) * iw + px;
                if (px < 0 || py < 0 || off >= static_cast<size_t>(buf->arrayLen))
                    return false;
                return ((buf->cells[off].u >> 24) & 0xFF) != 0;
            }
            static bool imgOpaqueAt(Obj *img, int ix, int iy, int wx, int wy)
            {
                Obj *buf = img->cells[IMG_BUF].o;
                int iw = img->cells[IMG_W].i, ih = img->cells[IMG_H].i;
                int px = wx - ix, py = wy - iy;
                if (!buf || px < 0 || py < 0 || px >= iw || py >= ih)
                    return false;
                return ((buf->cells[static_cast<size_t>(py) * iw + px].u >> 24) & 0xFF) != 0;
            }
            static void spr_collides(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *other = argRef(ctx, 1);
                bool pixel = argInt(ctx, 2) != 0;
                if (!self || !other || other->kind != ObjKind::Instance || !isSubclassOf(other, "javax/microedition/lcdui/game/Sprite") ||
                    !self->cells[L_VIS].i || !other->cells[L_VIS].i)
                {
                    setInt(ctx, 0);
                    return;
                }
                int ax, ay, aw, ah, bx, by, bw, bh;
                sprCollRect(self, ax, ay, aw, ah);
                sprCollRect(other, bx, by, bw, bh);
                if (!rectsOverlap(ax, ay, aw, ah, bx, by, bw, bh))
                {
                    setInt(ctx, 0);
                    return;
                }
                if (!pixel)
                {
                    setInt(ctx, 1);
                    return;
                }
                int x0 = std::max(ax, bx), y0 = std::max(ay, by), x1 = std::min(ax + aw, bx + bw), y1 = std::min(ay + ah, by + bh);
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++)
                        if (sprOpaqueAt(self, x, y) && sprOpaqueAt(other, x, y))
                        {
                            setInt(ctx, 1);
                            return;
                        }
                setInt(ctx, 0);
            }
            static void spr_collidesImage(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *img = argRef(ctx, 1);
                int x = argInt(ctx, 2), y = argInt(ctx, 3);
                bool pixel = argInt(ctx, 4) != 0;
                if (!self || !img || img->kind != ObjKind::Instance || !self->cells[L_VIS].i)
                {
                    setInt(ctx, 0);
                    return;
                }
                int ax, ay, aw, ah;
                sprCollRect(self, ax, ay, aw, ah);
                int iw = img->cells[IMG_W].i, ih = img->cells[IMG_H].i;
                if (!rectsOverlap(ax, ay, aw, ah, x, y, iw, ih))
                {
                    setInt(ctx, 0);
                    return;
                }
                if (!pixel)
                {
                    setInt(ctx, 1);
                    return;
                }
                int x0 = std::max(ax, x), y0 = std::max(ay, y), x1 = std::min(ax + aw, x + iw), y1 = std::min(ay + ah, y + ih);
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++)
                        if (sprOpaqueAt(self, xx, yy) && imgOpaqueAt(img, x, y, xx, yy))
                        {
                            setInt(ctx, 1);
                            return;
                        }
                setInt(ctx, 0);
            }
            // Tuile effective (1-based, 0 = vide) d'une cellule, animation résolue.
            static int tlTileAt(Obj *tl, int c, int r)
            {
                Obj *grid = tl->cells[TL_GRID].o;
                int cols = tl->cells[TL_COLS].i, rows = tl->cells[TL_ROWS].i;
                if (!grid || grid->kind != ObjKind::IntArray || c < 0 || r < 0 || c >= cols || r >= rows)
                    return 0;
                int t = grid->cells[r * cols + c].i;
                if (t < 0)
                {
                    Obj *anim = tl->cells[TL_ANIM].o;
                    int idx = -t - 1;
                    t = (anim && anim->kind == ObjKind::IntArray && idx >= 0 && idx < anim->arrayLen) ? anim->cells[idx].i : 0;
                }
                return t;
            }
            static bool tlOpaqueAt(Obj *tl, int t, int lx, int ly)
            {
                Obj *img = tl->cells[TL_IMG].o;
                Obj *buf = (img && img->kind == ObjKind::Instance) ? img->cells[IMG_BUF].o : nullptr;
                int tw = tl->cells[TL_TW].i, th = tl->cells[TL_TH].i;
                if (!buf || tw <= 0 || th <= 0)
                    return true;
                int iw = img->cells[IMG_W].i, perRow = iw / tw;
                if (perRow <= 0)
                    return true;
                int px = ((t - 1) % perRow) * tw + lx, py = ((t - 1) / perRow) * th + ly;
                size_t off = static_cast<size_t>(py) * iw + px;
                if (px < 0 || py < 0 || off >= static_cast<size_t>(buf->arrayLen))
                    return false;
                return ((buf->cells[off].u >> 24) & 0xFF) != 0;
            }
            static void spr_collidesTiled(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *tl = argRef(ctx, 1);
                bool pixel = argInt(ctx, 2) != 0;
                if (!self || !tl || tl->kind != ObjKind::Instance || !isSubclassOf(tl, "javax/microedition/lcdui/game/TiledLayer") ||
                    !self->cells[L_VIS].i || !tl->cells[L_VIS].i)
                {
                    setInt(ctx, 0);
                    return;
                }
                int ax, ay, aw, ah;
                sprCollRect(self, ax, ay, aw, ah);
                int tw = tl->cells[TL_TW].i, th = tl->cells[TL_TH].i;
                int cols = tl->cells[TL_COLS].i, rows = tl->cells[TL_ROWS].i;
                int bx = tl->cells[L_X].i, by = tl->cells[L_Y].i;
                if (tw <= 0 || th <= 0)
                {
                    setInt(ctx, 0);
                    return;
                }
                int c0 = std::max(0, (ax - bx) / tw), c1 = std::min(cols - 1, (ax + aw - 1 - bx) / tw);
                int r0 = std::max(0, (ay - by) / th), r1 = std::min(rows - 1, (ay + ah - 1 - by) / th);
                if (ax - bx < 0) c0 = 0;
                if (ay - by < 0) r0 = 0;
                for (int r = r0; r <= r1; r++)
                    for (int c = c0; c <= c1; c++)
                    {
                        int t = tlTileAt(tl, c, r);
                        if (t == 0)
                            continue;
                        int tx = bx + c * tw, ty = by + r * th;
                        if (!rectsOverlap(ax, ay, aw, ah, tx, ty, tw, th))
                            continue;
                        if (!pixel)
                        {
                            setInt(ctx, 1);
                            return;
                        }
                        int x0 = std::max(ax, tx), y0 = std::max(ay, ty), x1 = std::min(ax + aw, tx + tw), y1 = std::min(ay + ah, ty + th);
                        for (int y = y0; y < y1; y++)
                            for (int x = x0; x < x1; x++)
                                if (sprOpaqueAt(self, x, y) && tlOpaqueAt(tl, t, x - tx, y - ty))
                                {
                                    setInt(ctx, 1);
                                    return;
                                }
                    }
                setInt(ctx, 0);
            }
            static void spr_paint(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *g = argRef(ctx, 1);
                if (!self || !g || !self->cells[L_VIS].i)
                    return;
                Obj *img = self->cells[SPR_IMG].o;
                Obj *buf = (img && img->kind == ObjKind::Instance) ? img->cells[IMG_BUF].o : nullptr;
                if (!buf)
                    return;
                int iw = img->cells[IMG_W].i;
                int fw = self->cells[SPR_FW].i, fh = self->cells[SPR_FH].i;
                if (fw <= 0 || fh <= 0)
                    return;
                int perRow = iw / fw;
                int rawCount = sprRawCount(self);
                if (perRow <= 0)
                    return;
                int idx = self->cells[SPR_FRAME].i;
                Obj *seq = self->cells[SPR_SEQ].o;
                if (seq && seq->kind == ObjKind::IntArray && seq->arrayLen > 0)
                {
                    int sl = seq->arrayLen;
                    idx = ((idx % sl) + sl) % sl;
                    idx = seq->cells[idx].i;
                }
                idx = ((idx % rawCount) + rawCount) % rawCount;
                int xs = (idx % perRow) * fw, ys = (idx / perRow) * fh;
                Pix p(g);
                drawRegionRaw(p, buf, iw, xs, ys, fw, fh, self->cells[SPR_TFM].i,
                              self->cells[L_X].i, self->cells[L_Y].i);
            }

            // --- TiledLayer ---
            // ctor(int cols, int rows, Image, int tileW, int tileH). Indices de tuile 1-based (0 = vide,
            // négatif = tuile animée) : la tuile t est le sous-rectangle (t-1) de l'image (spec MIDP).
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
                Obj *anim = g_rt ? g_rt->heap().newArray(ObjKind::IntArray, 64) : nullptr;
                self->cells[TL_ANIM] = Value::fromRef(anim);
            }
            static void tl_setCell(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                int col = argInt(ctx, 1), row = argInt(ctx, 2), t = argInt(ctx, 3);
                Obj *grid = self->cells[TL_GRID].o;
                int cols = self->cells[TL_COLS].i, rows = self->cells[TL_ROWS].i;
                if (!grid || grid->kind != ObjKind::IntArray || col < 0 || row < 0 || col >= cols || row >= rows)
                    return;
                grid->cells[row * cols + col] = Value::fromInt(t);
            }
            static void tl_getCell(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                int col = argInt(ctx, 1), row = argInt(ctx, 2);
                Obj *grid = self ? self->cells[TL_GRID].o : nullptr;
                int cols = self ? self->cells[TL_COLS].i : 0, rows = self ? self->cells[TL_ROWS].i : 0;
                if (!grid || grid->kind != ObjKind::IntArray || col < 0 || row < 0 || col >= cols || row >= rows)
                {
                    setInt(ctx, 0);
                    return;
                }
                setInt(ctx, grid->cells[row * cols + col].i);
            }
            // fillCells(int col, int row, int numCols, int numRows, int tileIndex)
            static void tl_fillCells(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                if (!self)
                    return;
                int col = argInt(ctx, 1), row = argInt(ctx, 2);
                int nc = argInt(ctx, 3), nr = argInt(ctx, 4), t = argInt(ctx, 5);
                Obj *grid = self->cells[TL_GRID].o;
                int cols = self->cells[TL_COLS].i, rows = self->cells[TL_ROWS].i;
                if (!grid || grid->kind != ObjKind::IntArray)
                    return;
                for (int r = row; r < row + nr && r < rows; r++)
                    for (int c = col; c < col + nc && c < cols; c++)
                        if (r >= 0 && c >= 0)
                            grid->cells[r * cols + c] = Value::fromInt(t);
            }
            static void tl_createAnimatedTile(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *anim = self ? self->cells[TL_ANIM].o : nullptr;
                int t = argInt(ctx, 1);
                if (anim && anim->kind == ObjKind::IntArray)
                {
                    // Chaque tuile animée occupe un emplacement ; 0 = libre, l'index renvoyé est -(i+1).
                    for (int i = 0; i < anim->arrayLen; i++)
                        if (anim->cells[i].i == 0)
                        {
                            anim->cells[i] = Value::fromInt(t > 0 ? t : 1);
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
                int idx = -argInt(ctx, 1) - 1;
                Obj *anim = self->cells[TL_ANIM].o;
                if (anim && anim->kind == ObjKind::IntArray && idx >= 0 && idx < anim->arrayLen)
                    anim->cells[idx] = Value::fromInt(argInt(ctx, 2));
            }
            static void tl_getAnimatedTile(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *anim = self ? self->cells[TL_ANIM].o : nullptr;
                int idx = -argInt(ctx, 1) - 1;
                setInt(ctx, (anim && anim->kind == ObjKind::IntArray && idx >= 0 && idx < anim->arrayLen) ? anim->cells[idx].i : 0);
            }
            static void tl_getCellWidth(NativeContext *ctx) { if (ctx->thisObj) setInt(ctx, ctx->thisObj->cells[TL_TW].i); }
            static void tl_getCellHeight(NativeContext *ctx) { if (ctx->thisObj) setInt(ctx, ctx->thisObj->cells[TL_TH].i); }
            static void tl_getColumns(NativeContext *ctx) { if (ctx->thisObj) setInt(ctx, ctx->thisObj->cells[TL_COLS].i); }
            static void tl_getRows(NativeContext *ctx) { if (ctx->thisObj) setInt(ctx, ctx->thisObj->cells[TL_ROWS].i); }
            // setStaticTileSet(Image, tileW, tileH) : nouveau jeu de tuiles ; si le nombre de tuiles
            // diminue, la grille est vidée (spec), la taille de la couche est recalculée.
            static void tl_setStaticTileSet(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj, *img = argRef(ctx, 1);
                if (!self || !img)
                    return;
                int tw = argInt(ctx, 2), th = argInt(ctx, 3);
                int oldTiles = 0;
                {
                    Obj *oi = self->cells[TL_IMG].o;
                    int otw = self->cells[TL_TW].i, oth = self->cells[TL_TH].i;
                    if (oi && otw > 0 && oth > 0)
                        oldTiles = (oi->cells[IMG_W].i / otw) * (oi->cells[IMG_H].i / oth);
                }
                int newTiles = (tw > 0 && th > 0) ? (img->cells[IMG_W].i / tw) * (img->cells[IMG_H].i / th) : 0;
                self->cells[TL_IMG] = Value::fromRef(img);
                self->cells[TL_TW] = Value::fromInt(tw);
                self->cells[TL_TH] = Value::fromInt(th);
                self->cells[L_W] = Value::fromInt(self->cells[TL_COLS].i * tw);
                self->cells[L_H] = Value::fromInt(self->cells[TL_ROWS].i * th);
                if (newTiles < oldTiles)
                {
                    Obj *grid = self->cells[TL_GRID].o;
                    if (grid && grid->kind == ObjKind::IntArray)
                        for (int i = 0; i < grid->arrayLen; i++)
                            grid->cells[i] = Value::fromInt(0);
                }
            }
            static void tl_paint(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *g = argRef(ctx, 1);
                if (!self || !g || !self->cells[L_VIS].i)
                    return;
                Obj *img = self->cells[TL_IMG].o;
                Obj *buf = (img && img->kind == ObjKind::Instance) ? img->cells[IMG_BUF].o : nullptr;
                Obj *grid = self->cells[TL_GRID].o;
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
                // On ne parcourt que les cellules intersectant la zone de clip courante.
                int c0 = 0, c1 = cols - 1, r0 = 0, r1 = rows - 1;
                {
                    int cxl = p.clipX - p.tx - bx, cyt = p.clipY - p.ty - by;
                    int cxr = cxl + p.clipW - 1, cyb = cyt + p.clipH - 1;
                    c0 = std::max(0, cxl / tw);
                    r0 = std::max(0, cyt / th);
                    c1 = std::min(cols - 1, cxr < 0 ? -1 : cxr / tw);
                    r1 = std::min(rows - 1, cyb < 0 ? -1 : cyb / th);
                    if (cxl < 0) c0 = 0;
                    if (cyt < 0) r0 = 0;
                }
                for (int r = r0; r <= r1; r++)
                    for (int c = c0; c <= c1; c++)
                    {
                        int t = tlTileAt(self, c, r);
                        if (t <= 0)
                            continue;
                        int tt = t - 1;
                        int xs = (tt % perRow) * tw, ys = (tt / perRow) * th;
                        if (ys + th > img->cells[IMG_H].i)
                            continue;
                        drawRegionRaw(p, buf, iw, xs, ys, tw, th, 0, bx + c * tw, by + r * th);
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
                regN("javax/microedition/lcdui/game/Sprite.<init>:(Ljavax/microedition/lcdui/game/Sprite;)V", spr_initCopy);
                regN("javax/microedition/lcdui/game/Sprite.getRawFrameCount:()I", spr_getRawFrameCount);
                regN("javax/microedition/lcdui/game/Sprite.setImage:(Ljavax/microedition/lcdui/Image;II)V", spr_setImage);
                regN("javax/microedition/lcdui/game/Sprite.defineCollisionRectangle:(IIII)V", spr_defineCollisionRectangle);
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
                regN("javax/microedition/lcdui/game/TiledLayer.fillCells:(IIIII)V", tl_fillCells);
                regN("javax/microedition/lcdui/game/TiledLayer.getAnimatedTile:(I)I", tl_getAnimatedTile);
                regN("javax/microedition/lcdui/game/TiledLayer.getCellWidth:()I", tl_getCellWidth);
                regN("javax/microedition/lcdui/game/TiledLayer.getCellHeight:()I", tl_getCellHeight);
                regN("javax/microedition/lcdui/game/TiledLayer.getColumns:()I", tl_getColumns);
                regN("javax/microedition/lcdui/game/TiledLayer.getRows:()I", tl_getRows);
                regN("javax/microedition/lcdui/game/TiledLayer.setStaticTileSet:(Ljavax/microedition/lcdui/Image;II)V", tl_setStaticTileSet);
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

            // GC : Sprite/TiledLayer/LayerManager gardent TOUT leur état dans les cellules des objets Java
            // eux-mêmes (SPR_IMG, TL_*...) -- déjà scannées via les statics/frames qui les référencent. Aucun
            // Obj* natif persistant à marquer ici.
            void gcMarkGameRoots(Heap::Marker &) {}
        } // namespace detail
    } // namespace midp
} // namespace jvm
