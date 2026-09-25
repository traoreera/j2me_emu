#pragma once
// Interne : état et helpers partagés entre les modules midp_*.cpp
// (généré lors du découpage de l'ancien midp_natives.cpp monolithique).
#include "debug.h"
#include "midp.h"
#include "native.h"
#include "interpreter.h"
#include "../hal/display.h"
#include "../hal/input.h"
#include "../hal/jar_reader.h"
#include "../hal/png.h"
#include "class_file.h"
#include "../kernel/kernel.h"
#include "../kernel/drivers/audio/audio.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <algorithm>

namespace jvm
{

    const std::vector<Obj *> &jme_threads();
    void jme_threadForget(Obj *r);
    bool jme_threadResume(Obj *r, Interpreter *interp, ClassInfo *cls);


    namespace midp
    {
        extern int g_fbWrites, g_pixdbg, g_screenPix, g_canvasPix, g_flushCalls;
        namespace detail
        {


            // --- javax.microedition.media : implémentation réelle via kernel audio ---
            // L'InputStream est drainé vers un buffer compact côté hôte (comme pour
            // decodePng : chemin PC/memory ; sur MCU, on pointera un buffer ROM).
            // "audio/x-wav" : parse RIFF/WAV (PCM 8/16-bit, mono/stéréo, resample
            // vers 22050 Hz). "audio/x-tone-seq" : séquence ToneControl (sous-ensemble).
            // Le kernel joue du PCM via une voix PCM, ou une ToneSeq via une voix
            // tone-seq. Durées ToneControl : 1 unité = 1/64 s, vol 0..100.
            namespace audio = kernel::audio;

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
                G_COLOR = 0,
                G_FONT = 1,
                G_TX = 2,
                G_TY = 3,
                G_CLIPX = 4,
                G_CLIPY = 5,
                G_CLIPW = 6,
                G_CLIPH = 7,
                G_MODE = 8,
                G_TW = 9,
                G_TH = 10,
                G_STRIDE = 11,
                G_BUF = 12,
            };

            // DirectGraphicsWrapper (com.nokia.mid.ui.DirectGraphics implémenté
            // nativement) : retient le Graphics MIDP sous-jacent.
            enum
            {
                DG_GFX = 0
            };

            enum
            {
                GC_FULLSCREEN = 0,
                GC_GFX = 1
            };
            // Champs d'instance de javax.microedition.lcdui.game.Layer : tous les
            // objets de la hiérarchie game* se partagent les slots 0..4.
            enum
            {
                L_X = 0,
                L_Y = 1,
                L_W = 2,
                L_H = 3,
                L_VIS = 4
            };
            // Sprite (hérite de Layer) : slots 5..12.
            enum
            {
                SPR_IMG = 5,
                SPR_FW = 6,
                SPR_FH = 7,
                SPR_SEQ = 8,
                SPR_FRAME = 9,
                SPR_TFM = 10,
                SPR_RX = 11,
                SPR_RY = 12
            };
            // TiledLayer (hérite de Layer) : slots 5..11.
            enum
            {
                TL_IMG = 5,
                TL_TW = 6,
                TL_TH = 7,
                TL_COLS = 8,
                TL_ROWS = 9,
                TL_GRID = 10,
                TL_ANIM = 11
            };
            // LayerManager (hérite d'Object) : slots 0..6.
            enum
            {
                LM_LAYERS = 0,
                LM_COUNT = 1,
                LM_CAP = 2,
                LM_VX = 3,
                LM_VY = 4,
                LM_VW = 5,
                LM_VH = 6
            };
            enum
            {
                IMG_W = 0,
                IMG_H = 1,
                IMG_MUT = 2,
                IMG_BUF = 3,
                IMG_GFX = 4
            };
            enum
            {
                F_FACE = 0,
                F_STYLE = 1,
                F_SIZE = 2
            };
            constexpr int kNumSound = 16;            class Pix;
            struct JmeUi;
            extern Runtime *g_rt;
            extern Interpreter *g_interp;
            extern Obj *g_midlet;
            extern Obj *g_display;
            extern Obj *g_current;
            extern bool g_paintRequested;
            extern bool g_destroyed;
            extern std::vector<std::pair<std::string, std::string>> g_appProps;
            extern Obj *g_screenGfx;
            extern Obj *g_fontCache[4][3];
            extern uint16_t *g_canvas565;
            extern uint32_t g_keyStates;
            extern int g_tickN;
            extern Obj *g_listSelectCommand;


            inline int screenW()
            {
                auto *fb = hal::display_get_framebuffer();
                return fb ? fb->width : 240;
            }
            inline int screenH()
            {
                auto *fb = hal::display_get_framebuffer();
                return fb ? fb->height : 320;
            }            int jme_tickCount();
            Obj *argRef(NativeContext *ctx, int i);
            int32_t argInt(NativeContext *ctx, int i);
            void setInt(NativeContext *ctx, int32_t v);
            void setLong(NativeContext *ctx, int64_t v);
            void setRef(NativeContext *ctx, Obj *o);
            std::string sbCurrentStr(Obj *sb);
            Obj *makeInstance(const char *internalName);
            bool isSubclassOf(Obj *o, const char *internalName);
            uint16_t argb565(uint32_t c);
            Obj *screenGraphics();
            void drawRegionRaw(Pix &p, Obj *src, int iw, int xs, int ys, int w, int h, int tfm, int dx, int dy);
            JmeUi *uiFind(Obj *disp);
            JmeUi *uiFor(Obj *disp);
            void uiCmdSetLabel(Obj *cmd, Obj *label);
            void uiDispatchCommand(Obj *cmd, Obj *disp);
            void uiRenderScreen();
            int64_t argLongL(NativeContext *ctx, int i);
            int32_t streamFill(Obj *s, Obj *dst, int off, int len);
            void fireTimers();
            void mediaFlush();
            void regClass(Runtime *rt, const char *name, const char *super,
                          std::initializer_list<std::pair<const char *, const char *>> methods,
                          std::initializer_list<std::pair<const char *, const char *>> fields);
            void regN(const char *key, NativeFn fn);


            // Gwrape le target RGB565 pour écrire un pixel en tenant compte du clip.
            class Pix
            {
            public:
                Pix(Obj *g)
                {
                    color = static_cast<uint32_t>(g->cells[G_COLOR].u) | 0xFF000000u;
                    tx = g->cells[G_TX].i;
                    ty = g->cells[G_TY].i;
                    clipX = g->cells[G_CLIPX].i;
                    clipY = g->cells[G_CLIPY].i;
                    clipW = g->cells[G_CLIPW].i;
                    clipH = g->cells[G_CLIPH].i;
                    tw = g->cells[G_TW].i;
                    th = g->cells[G_TH].i;
                    stride = g->cells[G_STRIDE].i;
                    mode = g->cells[G_MODE].i;
                    buf = g->cells[G_BUF].o;
                    u565 = nullptr;
                    if (mode == GM_CANVAS_565)
                    {
                        u565 = g_canvas565;
                        g_canvasPix++;
                    }
                    if (mode == GM_SCREEN_565)
                        g_screenPix++;
                    if (jvm::pixDbg() && g_pixdbg < 8)
                    {
                        g_pixdbg++;
                        fprintf(stderr, "[pix] cible mode=%u %dx%d at(%d,%d) clip=%d,%d %dx%d g=%p\n",
                                mode, tw, th, tx, ty, clipX, clipY, clipW, clipH, (void *)g);
                    }
                }

                void put(int x, int y, uint32_t argb)
                {
                    int rx = x - tx, ry = y - ty;
                    if (rx < clipX || ry < clipY)
                        return;
                    if (rx >= clipX + clipW || ry >= clipY + clipH)
                        return;
                    if (rx < 0 || ry < 0 || rx >= tw || ry >= th)
                        return;

                    uint32_t srcA = (argb >> 24) & 0xFF;
                    if (mode == GM_IMAGE_ARGB)
                    {
                        if (!buf)
                            return;
                        int idx = ry * stride + rx;
                        if (srcA == 0xFF || !(buf->cells[idx].u & 0xFF000000))
                        {
                            buf->cells[idx].u = argb;
                            return;
                        }
                        uint32_t dst = buf->cells[idx].u;
                        uint32_t da = (dst >> 24) & 0xFF;
                        uint32_t a = srcA + (da * (255 - srcA)) / 255;
                        if (a == 0)
                        {
                            buf->cells[idx].u = 0;
                            return;
                        }
                        uint32_t sa = (srcA * 255) / a;
                        uint32_t sr = ((argb >> 16) & 0xFF) * sa, sg = ((argb >> 8) & 0xFF) * sa, sb = (argb & 0xFF) * sa;
                        uint32_t dr = ((dst >> 16) & 0xFF) * (255 - sa), dg = ((dst >> 8) & 0xFF) * (255 - sa), db = (dst & 0xFF) * (255 - sa);
                        buf->cells[idx].u = (a << 24) | (((sr + dr) / 255) << 16) | (((sg + dg) / 255) << 8) | ((sb + db) / 255);
                        return;
                    }

                    uint16_t *dst = (mode == GM_SCREEN_565) ? (hal::display_get_framebuffer() ? hal::display_get_framebuffer()->pixels : nullptr) : u565;
                    if (!dst)
                        return;
                    int idx = ry * stride + rx;
                    if (srcA == 0)
                        return;
                    if (srcA == 0xFF)
                    {
                        dst[idx] = argb565(argb);
                        if (mode == GM_SCREEN_565)
                            g_fbWrites++;
                        return;
                    }
                    uint16_t cur = dst[idx];
                    uint32_t r = ((cur >> 11) & 0x1F) * 255 / 31;
                    uint32_t gg = ((cur >> 5) & 0x3F) * 255 / 63;
                    uint32_t b = (cur & 0x1F) * 255 / 31;
                    dst[idx] = static_cast<uint16_t>((((((argb >> 16) & 0xFF) * srcA + r * (255 - srcA)) / 255) >> 3) << 11) |
                               static_cast<uint16_t>((((((argb >> 8) & 0xFF) * srcA + gg * (255 - srcA)) / 255) >> 2) << 5) |
                               static_cast<uint16_t>((((argb & 0xFF) * srcA + b * (255 - srcA)) / 255) >> 3);
                    if (mode == GM_SCREEN_565)
                        g_fbWrites++;
                }

                uint32_t color;
                int tx, ty, clipX, clipY, clipW, clipH;
                int tw, th, stride;
                uint32_t mode;
                Obj *buf;
                uint16_t *u565;
            };


            // ---------------------------------------------------------------------
            // UI réelle : List / Form / Command / Displayable. Bounce (obfusquée :
            // classe `a` = CommandListener, champs c (List menu), F (List niveaux),
            // j (Form scores)) ne peut être jouable sans un minimum de support de
            // ces classes : les List publient des items, gardent une sélection, et
            // un FIRE softkey dispatch commandAction(Command, Displayable) sur le
            // CommandListener enregistré (comparaison par identité d'objet avec
            // List.SELECT_COMMAND). L'état est stocké dans un registre C++ côté
            // émulateur indexé par Obj* (heap bump-only, pas de GC : les pointeurs
            // restent stables) -- on n'ajoute AUCUN champ d'instance à Displayable,
            // car Canvas réserve cells[0]/cells[1] (GC_FULLSCREEN/GC_GFX) sur lui.
            // ---------------------------------------------------------------------

            struct JmeUi
            {
                Obj *disp = nullptr;     // Displayable (List ou Form)
                Obj *title = nullptr;    // String
                Obj *listener = nullptr; // CommandListener
                std::vector<Obj *> commands;
                bool isList = false;
                std::vector<Obj *> items; // String pour List/Form
                int sel = 0;
                Obj *selectCmd = nullptr; // setSelectCommand (défaut = SELECT_COMMAND)
                int type = 0;
            };
            void registerCoreNatives();
            void registerGraphicsNatives();
            void registerGameNatives();
            void registerUiNatives();
            void registerIoNatives();
            void registerMediaNatives();
        } // namespace detail
    } // namespace midp
} // namespace jvm
