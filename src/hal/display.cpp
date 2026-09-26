#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include "hal/display.h"
#include "hal/font.h"
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
        // Fenêtre : JME_WINDOW_WIDTH/HEIGHT sinon le PLUS GRAND facteur entier qui tient dans
        // ~90 % de la zone utile du bureau (24" : 240x320 -> x3, 480x320 -> x2...).
        // Redimensionnable ensuite (image proportionnelle + bandes noires), F11 = plein écran.
        int winW = 0, winH = 0;
        if (const char *v = getenv("JME_WINDOW_WIDTH")) winW = atoi(v);
        if (const char *v = getenv("JME_WINDOW_HEIGHT")) winH = atoi(v);
        if (winW <= 0 || winH <= 0)
        {
            int mul = 0;
            SDL_Rect ub{};
            if (SDL_GetDisplayUsableBounds(0, &ub) == 0 && ub.w > 0 && ub.h > 0)
                mul = std::min((int)(ub.w * 0.90) / fbW, (int)(ub.h * 0.90) / fbH);
            if (mul <= 0) // bureau inconnu (headless) ou écran plus petit que l'image
                mul = (fbW * 2 > 1200 || fbH * 2 > 1000) ? 1 : 2;
            mul = std::max(1, std::min(mul, 12));
            winW = fbW * mul;
            winH = fbH * mul;
        }
        fprintf(stderr, "[display] fenetre %dx%d pour une image %dx%d\n", winW, winH, fbW, fbH);
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

    void display_toggle_fullscreen()
    {
        if (!g_window)
            return;
        bool fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
        SDL_SetWindowFullscreen(g_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

    void display_set_title(const char *title)
    {
        if (g_window && title)
            SDL_SetWindowTitle(g_window, title);
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

    // display_present() ne fait que marquer l'image comme « à afficher » : les MIDlets appellent flushGraphics()/
    // repaint() plusieurs fois par trame (Stalker : des dizaines), et chaque SDL_RenderPresent attend le vsync.
    // display_flip() (une fois par trame, boucle principale / launcher) envoie réellement l'image à l'écran.
    static bool g_dirty = false;
    void display_present(const Framebuffer *fb)
    {
        (void)fb;
        g_dirty = true;
    }

    void display_flip()
    {
        const Framebuffer *fb = &g_fb;
        g_dirty = false;
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
            const uint8_t *glyph = font_glyph(static_cast<unsigned char>(*text));
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
            const uint8_t *glyph = font_glyph(static_cast<unsigned char>(*text));
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