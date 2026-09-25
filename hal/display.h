#pragma once

#include <cstdint>

namespace hal
{

    struct DisplayConfig
    {
        int width = 240;
        int height = 320;
        // RGB565 = 2 bytes/pixel
    };

    struct Framebuffer
    {
        uint16_t *pixels = nullptr;
        int width = 0;
        int height = 0;
        int stride = 0; // pixels per row (may be > width for alignment)
    };

    bool display_init(const DisplayConfig *cfg);
    void display_shutdown();
    Framebuffer *display_get_framebuffer();
    void display_present(const Framebuffer *fb);
    // Rotation d'affichage (0 ou 90 = vue tournée de 90° anti-horaire), lue de
    // JME_ROTATE. Pour les jeux 480x800 dessinés de côté (téléphone tenu en paysage).
    int display_rotation();
    // Coordonnées fenêtre -> écran logique (rotation, letterbox et échelle inclus).
    void display_window_to_logical(int wx, int wy, int &lx, int &ly);
    void display_clear(uint16_t color = 0x0000);

    // Dessine du texte 5x7 dans le framebuffer courant (clipsé aux bords).
    void display_draw_text(int x, int y, const char *text, uint16_t color = 0xFFFF);

} // namespace hal