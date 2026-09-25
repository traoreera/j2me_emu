#pragma once

#include <cstdint>

namespace hal
{

    enum KeyCode : uint32_t
    {
        KEY_NONE = 0,
        KEY_UP = 1u << 0,
        KEY_DOWN = 1u << 1,
        KEY_LEFT = 1u << 2,
        KEY_RIGHT = 1u << 3,
        KEY_FIRE = 1u << 4,  // central D-pad
        KEY_SOFT1 = 1u << 5, // left softkey
        KEY_SOFT2 = 1u << 6, // right softkey
        KEY_0 = 1u << 7,
        KEY_1 = 1u << 8,
        KEY_2 = 1u << 9,
        KEY_3 = 1u << 10,
        KEY_4 = 1u << 11,
        KEY_5 = 1u << 12,
        KEY_6 = 1u << 13,
        KEY_7 = 1u << 14,
        KEY_8 = 1u << 15,
        KEY_9 = 1u << 16,
        KEY_STAR = 1u << 17, // *
        KEY_HASH = 1u << 18, // #
    };

    using KeyMask = uint32_t;

    // Évènement souris/tactile de la trame, en coordonnées de l'écran LOGIQUE
    // (déjà ramenées de la taille de la fenêtre à celle du framebuffer).
    struct PointerEvent
    {
        enum Kind : int { PRESS = 0, RELEASE = 1, DRAG = 2 };
        int kind = PRESS;
        int x = 0, y = 0;
    };
    constexpr int kMaxPointerEvents = 16;

    struct InputState
    {
        PointerEvent pointer[kMaxPointerEvents]; // évènements de la trame (clic gauche)
        int pointerCount = 0;
        KeyMask pressed = 0;      // currently held
        KeyMask justPressed = 0;  // pressed this frame
        KeyMask justReleased = 0; // released this frame
        bool quit = false;        // quitter demandé (F12 / Ctrl+Q / fermeture fenêtre)
    };

    bool input_init();
    void input_shutdown();
    void input_poll(InputState *out);
    const char *key_name(KeyCode kc);

    // Remappe des touches physiques -> KeyCode. spec = liste "NOMSDL=TOKEN" séparée
    // par des virgules/points-virgules/newlines. TOKEN ∈ {UP,DOWN,LEFT,RIGHT,FIRE,
    // SOFT1,SOFT2,STAR,HASH,0..9} ou NONE pour délier. Appliqué PAR-DESSUS la table
    // par défaut. Retourne false si aucune entrée valide n'a été analysée.
    bool input_applyKeyMap(const char *spec);

} // namespace hal