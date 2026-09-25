// Launcher : menu de sélection des jeux (bitmap font 5x7 agrandie, sans dépendance).
#include "app/launcher.h"
#include "hal/display.h"
#include "hal/input.h"
#include "hal/jar_reader.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <dirent.h>

namespace
{
    struct Entry
    {
        std::string path, title, sub;
    };

    constexpr int kW = 480, kH = 320, kRowH = 30, kTop = 44, kBottom = 22, kScale = 2;

    uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

    // Lit "JME_WIDTH/HEIGHT" du .conf voisin pour afficher le profil de résolution.
    std::string profileOf(const std::string &jar)
    {
        std::string c = jar.substr(0, jar.size() - 4) + ".conf";
        FILE *f = fopen(c.c_str(), "r");
        if (!f)
            return "";
        int w = 0, h = 0;
        bool rot = false;
        char line[256];
        while (fgets(line, sizeof line, f))
        {
            sscanf(line, "JME_WIDTH=%d", &w);
            sscanf(line, "JME_HEIGHT=%d", &h);
            if (strncmp(line, "JME_ROTATE=90", 13) == 0)
                rot = true;
        }
        fclose(f);
        if (w <= 0 || h <= 0)
            return "";
        char b[48];
        snprintf(b, sizeof b, "%dx%d%s", w, h, rot ? " tourne" : "");
        return b;
    }

    std::vector<Entry> scan(const std::string &dir)
    {
        std::vector<Entry> out;
        DIR *d = opendir(dir.c_str());
        if (!d)
            return out;
        while (dirent *e = readdir(d))
        {
            std::string n = e->d_name;
            if (n.size() < 5 || n.compare(n.size() - 4, 4, ".jar") != 0)
                continue;
            Entry en;
            en.path = dir + "/" + n;
            en.title = n.substr(0, n.size() - 4);
            jme::JarReader jr;
            jme::ManifestInfo mi;
            if (jr.open(en.path.c_str()) && jr.readManifest(mi) && !mi.midletName.empty())
            {
                en.title = mi.midletName;
                en.sub = mi.midletVendor;
                if (!mi.midletVersion.empty())
                    en.sub += (en.sub.empty() ? "v" : "  v") + mi.midletVersion;
            }
            std::string prof = profileOf(en.path);
            if (!prof.empty())
                en.sub += (en.sub.empty() ? "" : "  ") + std::string("[") + prof + "]";
            out.push_back(en);
        }
        closedir(d);
        std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
            std::string x = a.title, y = b.title;
            std::transform(x.begin(), x.end(), x.begin(), ::tolower);
            std::transform(y.begin(), y.end(), y.begin(), ::tolower);
            return x < y;
        });
        return out;
    }

    void clip(std::string &s, size_t maxChars)
    {
        if (s.size() > maxChars)
            s = s.substr(0, maxChars - 2) + "..";
    }
} // namespace

namespace launcher
{
    std::string run(const std::string &gamesDir, const std::string &preselect)
    {
        std::vector<Entry> games = scan(gamesDir);

        hal::DisplayConfig cfg{kW, kH};
        if (!hal::display_init(&cfg))
        {
            fprintf(stderr, "launcher: echec init display\n");
            return "";
        }
        hal::input_init();

        int sel = 0, top = 0;
        for (size_t i = 0; i < games.size(); i++)
            if (games[i].path == preselect)
                sel = (int)i;
        const int rows = (kH - kTop - kBottom) / kRowH;
        const uint16_t bg = rgb(16, 18, 28), hi = rgb(200, 40, 40), fg = rgb(235, 235, 240),
                       dim = rgb(140, 145, 165), bar = rgb(30, 34, 52);
        // JME_LAUNCHER_AUTO=n : lance le jeu n° n à la 3e trame (tests/CI sans clavier).
        const int autoPick = getenv("JME_LAUNCHER_AUTO") ? atoi(getenv("JME_LAUNCHER_AUTO")) : -1;
        int maxFrames = getenv("JME_MAXFRAMES") ? atoi(getenv("JME_MAXFRAMES")) : -1;
        std::string chosen;
        int prevDownY = -1;

        for (int frame = 0;; frame++)
        {
            if (maxFrames >= 0 && frame >= maxFrames)
                break;
            hal::InputState in;
            hal::input_poll(&in);
            if (in.quit)
                break;
            const int n = (int)games.size();
            if (autoPick >= 0 && autoPick < n && frame == 3)
            {
                chosen = games[autoPick].path;
                break;
            }
            if (n > 0)
            {
                if (in.justPressed & hal::KEY_DOWN) sel = std::min(n - 1, sel + 1);
                if (in.justPressed & hal::KEY_UP) sel = std::max(0, sel - 1);
                if (in.justPressed & hal::KEY_RIGHT) sel = std::min(n - 1, sel + rows);
                if (in.justPressed & hal::KEY_LEFT) sel = std::max(0, sel - rows);
                sel = std::max(0, std::min(n - 1, sel - in.wheel));
                if (in.justPressed & (hal::KEY_FIRE | hal::KEY_SOFT1))
                {
                    chosen = games[sel].path;
                    break;
                }
                for (int i = 0; i < in.pointerCount; i++)
                {
                    const hal::PointerEvent &pe = in.pointer[i];
                    if (pe.kind == hal::PointerEvent::PRESS && pe.y >= kTop && pe.y < kTop + rows * kRowH)
                    {
                        int idx = top + (pe.y - kTop) / kRowH;
                        if (idx < n)
                        {
                            if (idx == sel && prevDownY == idx)
                                chosen = games[idx].path; // 2e clic sur la même ligne = lancer
                            sel = idx;
                            prevDownY = idx;
                        }
                    }
                }
                if (!chosen.empty())
                    break;
                if (sel < top) top = sel;
                if (sel >= top + rows) top = sel - rows + 1;
            }

            hal::display_fill_rect(0, 0, kW, kH, bg);
            hal::display_fill_rect(0, 0, kW, 32, bar);
            hal::display_draw_text_scaled(10, 9, "J2ME EMU - JEUX", fg, kScale);
            char cnt[32];
            snprintf(cnt, sizeof cnt, "%d", (int)games.size());
            hal::display_draw_text_scaled(kW - 10 - 12 * (int)strlen(cnt), 9, cnt, dim, kScale);

            if (games.empty())
            {
                hal::display_draw_text_scaled(10, 60, "Aucun .jar dans :", fg, kScale);
                std::string d = gamesDir;
                clip(d, 36);
                hal::display_draw_text_scaled(10, 80, d.c_str(), dim, kScale);
            }
            for (int r = 0; r < rows && top + r < n; r++)
            {
                const Entry &e = games[top + r];
                int y = kTop + r * kRowH;
                bool on = (top + r == sel);
                if (on)
                    hal::display_fill_rect(6, y - 2, kW - 12, kRowH - 2, hi);
                std::string t = e.title;
                clip(t, 38);
                hal::display_draw_text_scaled(14, y + 1, t.c_str(), fg, kScale);
                std::string s = e.sub;
                clip(s, 80);
                hal::display_draw_text_scaled(14, y + 18, s.c_str(), on ? fg : dim, 1);
            }
            if (n > rows)
            {
                int th = std::max(12, (kH - kTop - kBottom) * rows / n);
                int ty = kTop + (kH - kTop - kBottom - th) * (n > 1 ? sel : 0) / std::max(1, n - 1);
                hal::display_fill_rect(kW - 5, ty, 3, th, dim);
            }
            hal::display_fill_rect(0, kH - kBottom, kW, kBottom, bar);
            hal::display_draw_text_scaled(10, kH - kBottom + 7,
                                          "HAUT/BAS choisir  ENTREE jouer  F12 menu  CTRL+Q quitter", dim, 1);
            hal::display_present(hal::display_get_framebuffer());
            SDL_Delay(16);
        }
        if (const char *dump = getenv("JME_DUMP"))
            hal::display_dump_ppm(dump);
        hal::input_shutdown();
        hal::display_shutdown();
        return chosen;
    }
} // namespace launcher
