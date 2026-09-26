#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include "hal/display.h"
#include <SDL2/SDL.h>

namespace hal
{

    static SDL_Window *g_window = nullptr;
    static SDL_Renderer *g_renderer = nullptr;
    static SDL_Texture *g_texture = nullptr;
    static Framebuffer g_fb = {};
    static int g_rotation = 0;
    static bool g_integerScale = false; // JME_SCALE=integer : facteur entier + letterbox

    int display_rotation() { return g_rotation; }

    bool display_init(const DisplayConfig *cfg)
    {
        if (SDL_Init(SDL_INIT_VIDEO) != 0)
            return false;

        if (const char *r = getenv("JME_ROTATE"))
            g_rotation = (atoi(r) == 90) ? 90 : 0;
        if (const char *s = getenv("JME_SCALE"))
            g_integerScale = (strcmp(s, "integer") == 0);
        const int fbW = g_rotation ? cfg->height : cfg->width; // taille affichée (après rotation)
        const int fbH = g_rotation ? cfg->width : cfg->height;
        // Fenêtre : JME_WINDOW_WIDTH/HEIGHT sinon taille affichée x2 (x1 si trop grande).
        int winW = 0, winH = 0;
        if (const char *v = getenv("JME_WINDOW_WIDTH")) winW = atoi(v);
        if (const char *v = getenv("JME_WINDOW_HEIGHT")) winH = atoi(v);
        if (winW <= 0 || winH <= 0)
        {
            int mul = (fbW * 2 > 1200 || fbH * 2 > 1000) ? 1 : 2;
            if (fbW < 200) // petits écrans (Nokia 96x65...) : agrandir pour rester lisible
                mul = std::max(2, std::min(8, std::min(1200 / fbW, 960 / fbH)));
            winW = fbW * mul;
            winH = fbH * mul;
        }
        Uint32 wflags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
        const char *fs = getenv("JME_FULLSCREEN");
        if (fs && atoi(fs) != 0)
            wflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        g_window = SDL_CreateWindow("J2ME Emu",
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    winW, winH, wflags);
        if (!g_window)
            return false;

        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | (getenv("JME_VSYNC") && atoi(getenv("JME_VSYNC")) == 0 ? 0 : SDL_RENDERER_PRESENTVSYNC));
        if (!g_renderer)
            return false;

        g_texture = SDL_CreateTexture(g_renderer,
                                      SDL_PIXELFORMAT_RGB565,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      cfg->width, cfg->height);
        if (!g_texture)
            return false;

        g_fb.pixels = new uint16_t[cfg->width * cfg->height];
        g_fb.width = cfg->width;
        g_fb.height = cfg->height;
        g_fb.stride = cfg->width;

        return true;
    }

    void display_shutdown()
    {
        delete[] g_fb.pixels;
        g_fb.pixels = nullptr;
        if (g_texture)
            SDL_DestroyTexture(g_texture);
        if (g_renderer)
            SDL_DestroyRenderer(g_renderer);
        if (g_window)
            SDL_DestroyWindow(g_window);
        SDL_Quit();
    }

    Framebuffer *display_get_framebuffer()
    {
        return &g_fb;
    }

    // Cadrage de l'image dans la fenêtre : facteur (entier si JME_SCALE=integer,
    // sinon "fit" proportionnel), centré, bandes noires (letterbox) ailleurs.
    // Pas de filtrage : SDL est en nearest par défaut. Partagé avec le mapping
    // souris/tactile pour que clic et rendu restent cohérents.
    struct View { float s; int x0, y0, vw, vh; };
    static View computeView(int ww, int wh)
    {
        const int sw = g_rotation ? g_fb.height : g_fb.width;
        const int sh = g_rotation ? g_fb.width : g_fb.height;
        View v{1.f, 0, 0, sw, sh};
        if (sw <= 0 || sh <= 0 || ww <= 0 || wh <= 0)
            return v;
        float s = std::min(ww / (float)sw, wh / (float)sh);
        if (g_integerScale)
            s = std::max(1.f, std::floor(s));
        v.s = s;
        v.vw = (int)(sw * s);
        v.vh = (int)(sh * s);
        v.x0 = (ww - v.vw) / 2;
        v.y0 = (wh - v.vh) / 2;
        return v;
    }

    void display_window_to_logical(int wx, int wy, int &lx, int &ly)
    {
        int ww = 0, wh = 0;
        if (g_window)
            SDL_GetWindowSize(g_window, &ww, &wh);
        lx = wx;
        ly = wy;
        if (!g_fb.pixels || ww <= 0 || wh <= 0)
            return;
        View v = computeView(ww, wh);
        int px = (int)((wx - v.x0) / v.s);
        int py = (int)((wy - v.y0) / v.s);
        const int sw = g_rotation ? g_fb.height : g_fb.width;
        const int sh = g_rotation ? g_fb.width : g_fb.height;
        px = std::max(0, std::min(sw - 1, px));
        py = std::max(0, std::min(sh - 1, py));
        if (g_rotation == 90) // vue tournée de 90° anti-horaire
        {
            lx = sh - 1 - py;
            ly = px;
        }
        else
        {
            lx = px;
            ly = py;
        }
    }

    void display_present(const Framebuffer *fb)
    {
        SDL_UpdateTexture(g_texture, nullptr, fb->pixels, fb->stride * sizeof(uint16_t));
        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_renderer);
        int ww = 0, wh = 0;
        SDL_GetWindowSize(g_window, &ww, &wh);
        View v = computeView(ww, wh);
        if (g_rotation == 90)
        {
            const int cx = v.x0 + v.vw / 2, cy = v.y0 + v.vh / 2;
            SDL_Rect dst = {cx - v.vh / 2, cy - v.vw / 2, v.vh, v.vw};
            SDL_RenderCopyEx(g_renderer, g_texture, nullptr, &dst, -90.0, nullptr, SDL_FLIP_NONE);
        }
        else
        {
            SDL_Rect dst = {v.x0, v.y0, v.vw, v.vh};
            SDL_RenderCopy(g_renderer, g_texture, nullptr, &dst);
        }
        SDL_RenderPresent(g_renderer);
    }

    void display_clear(uint16_t color)
    {
        if (g_fb.pixels)
        {
            for (int i = 0; i < g_fb.width * g_fb.height; ++i)
            {
                g_fb.pixels[i] = color;
            }
        }
    }

    // Font 5x7 pour les caractères ASCII 0x20..0x7F.
    static const uint8_t kFont5x7[96][5] = {
        {0x00, 0x00, 0x00, 0x00, 0x00}, // space
        {0x00, 0x00, 0x4F, 0x00, 0x00}, // !
        {0x00, 0x07, 0x00, 0x07, 0x00}, // "
        {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
        {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
        {0x23, 0x13, 0x08, 0x64, 0x62}, // %
        {0x36, 0x49, 0x55, 0x22, 0x50}, // &
        {0x00, 0x05, 0x03, 0x00, 0x00}, // '
        {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
        {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
        {0x14, 0x08, 0x3E, 0x08, 0x14}, // *
        {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
        {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
        {0x08, 0x08, 0x08, 0x08, 0x08}, // -
        {0x00, 0x60, 0x60, 0x00, 0x00}, // .
        {0x20, 0x10, 0x08, 0x04, 0x02}, // /
        {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
        {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
        {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
        {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
        {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
        {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
        {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
        {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
        {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
        {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
        {0x00, 0x14, 0x14, 0x00, 0x00}, // :
        {0x00, 0x40, 0x34, 0x00, 0x00}, // ;
        {0x00, 0x08, 0x14, 0x22, 0x41}, // <
        {0x14, 0x14, 0x14, 0x14, 0x14}, // =
        {0x41, 0x22, 0x14, 0x08, 0x00}, // >
        {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
        {0x32, 0x49, 0x79, 0x41, 0x3E}, // @
        {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
        {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
        {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
        {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
        {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
        {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
        {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
        {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
        {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
        {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
        {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
        {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
        {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
        {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
        {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
        {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
        {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
        {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
        {0x46, 0x49, 0x49, 0x49, 0x31}, // S
        {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
        {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
        {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
        {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
        {0x63, 0x14, 0x08, 0x14, 0x63}, // X
        {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
        {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
        {0x00, 0x7F, 0x41, 0x41, 0x00}, // [
        {0x02, 0x04, 0x08, 0x10, 0x20}, // backslash
        {0x00, 0x41, 0x41, 0x7F, 0x00}, // ]
        {0x04, 0x02, 0x01, 0x02, 0x04}, // ^
        {0x40, 0x40, 0x40, 0x40, 0x40}, // _
        {0x00, 0x01, 0x02, 0x04, 0x00}, // `
        {0x20, 0x54, 0x54, 0x54, 0x78}, // a
        {0x7F, 0x48, 0x44, 0x44, 0x38}, // b
        {0x38, 0x44, 0x44, 0x44, 0x20}, // c
        {0x38, 0x44, 0x44, 0x48, 0x7F}, // d
        {0x38, 0x54, 0x54, 0x54, 0x18}, // e
        {0x08, 0x7E, 0x09, 0x01, 0x02}, // f
        {0x0C, 0x52, 0x52, 0x52, 0x3E}, // g
        {0x7F, 0x08, 0x04, 0x04, 0x78}, // h
        {0x00, 0x44, 0x7D, 0x40, 0x00}, // i
        {0x20, 0x40, 0x44, 0x3D, 0x00}, // j
        {0x7F, 0x10, 0x28, 0x44, 0x00}, // k
        {0x00, 0x41, 0x7F, 0x40, 0x00}, // l
        {0x7C, 0x04, 0x18, 0x04, 0x78}, // m
        {0x7C, 0x08, 0x04, 0x04, 0x78}, // n
        {0x38, 0x44, 0x44, 0x44, 0x38}, // o
        {0x7C, 0x14, 0x14, 0x14, 0x08}, // p
        {0x08, 0x14, 0x14, 0x18, 0x7C}, // q
        {0x7C, 0x08, 0x04, 0x04, 0x08}, // r
        {0x48, 0x54, 0x54, 0x54, 0x20}, // s
        {0x04, 0x3F, 0x44, 0x40, 0x20}, // t
        {0x3C, 0x40, 0x40, 0x20, 0x7C}, // u
        {0x1C, 0x20, 0x40, 0x20, 0x1C}, // v
        {0x3C, 0x40, 0x30, 0x40, 0x3C}, // w
        {0x44, 0x28, 0x10, 0x28, 0x44}, // x
        {0x0C, 0x50, 0x50, 0x50, 0x3C}, // y
        {0x44, 0x64, 0x54, 0x4C, 0x44}, // z
        {0x00, 0x08, 0x36, 0x41, 0x00}, // {
        {0x00, 0x00, 0x7F, 0x00, 0x00}, // |
        {0x00, 0x41, 0x36, 0x08, 0x00}, // }
        {0x08, 0x04, 0x08, 0x10, 0x08}, // ~
    };

    bool display_dump_ppm(const char *path)
    {
        FILE *f = g_fb.pixels ? fopen(path, "wb") : nullptr;
        if (!f)
            return false;
        fprintf(f, "P6\n%d %d\n255\n", g_fb.width, g_fb.height);
        for (int y = 0; y < g_fb.height; y++)
            for (int x = 0; x < g_fb.width; x++)
            {
                uint16_t p = g_fb.pixels[y * g_fb.stride + x];
                fputc((p >> 11) << 3, f);
                fputc(((p >> 5) & 0x3F) << 2, f);
                fputc((p & 0x1F) << 3, f);
            }
        fclose(f);
        return true;
    }

    void display_fill_rect(int x, int y, int w, int h, uint16_t color)
    {
        if (!g_fb.pixels)
            return;
        int x1 = std::min(x + w, g_fb.width), y1 = std::min(y + h, g_fb.height);
        for (int yy = std::max(y, 0); yy < y1; yy++)
            for (int xx = std::max(x, 0); xx < x1; xx++)
                g_fb.pixels[yy * g_fb.stride + xx] = color;
    }

    void display_draw_text_scaled(int x, int y, const char *text, uint16_t color, int scale)
    {
        if (!g_fb.pixels || !text || scale < 1)
            return;
        for (; *text; text++, x += 6 * scale)
        {
            unsigned char c = static_cast<unsigned char>(*text);
            if (c < 0x20 || c > 0x7F)
                c = '?';
            const uint8_t *glyph = kFont5x7[c - 0x20];
            for (int col = 0; col < 5; col++)
                for (int row = 0; row < 7; row++)
                    if (glyph[col] & (1 << row))
                        display_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }

    void display_draw_text(int x, int y, const char *text, uint16_t color)
    {
        if (!g_fb.pixels || !text)
            return;
        while (*text)
        {
            unsigned char c = static_cast<unsigned char>(*text);
            if (c < 0x20 || c > 0x7F)
                c = '?';
            const uint8_t *glyph = kFont5x7[c - 0x20];
            for (int col = 0; col < 5; col++)
            {
                int px = x + col;
                if (px < 0 || px >= g_fb.width)
                    continue;
                for (int row = 0; row < 7; row++)
                {
                    int py = y + row;
                    if (py < 0 || py >= g_fb.height)
                        continue;
                    if (glyph[col] & (1 << row))
                        g_fb.pixels[py * g_fb.stride + px] = color;
                }
            }
            x += 6;
            text++;
        }
    }

} // namespace hal