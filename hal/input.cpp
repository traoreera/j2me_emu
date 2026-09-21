#include "hal/input.h"
#include <SDL2/SDL.h>
#include <unordered_map>

namespace hal
{

static std::unordered_map<SDL_Keycode, KeyCode> g_keyMap = {
    {SDLK_UP, KEY_UP},
    {SDLK_DOWN, KEY_DOWN},
    {SDLK_LEFT, KEY_LEFT},
    {SDLK_RIGHT, KEY_RIGHT},
    {SDLK_RETURN, KEY_FIRE},
    {SDLK_SPACE, KEY_FIRE},
    {SDLK_ESCAPE, KEY_SOFT2},
    {SDLK_F1, KEY_SOFT1},
    {SDLK_F2, KEY_SOFT2},
    {SDLK_KP_0, KEY_0}, {SDLK_0, KEY_0},
    {SDLK_KP_1, KEY_1}, {SDLK_1, KEY_1},
    {SDLK_KP_2, KEY_2}, {SDLK_2, KEY_2},
    {SDLK_KP_3, KEY_3}, {SDLK_3, KEY_3},
    {SDLK_KP_4, KEY_4}, {SDLK_4, KEY_4},
    {SDLK_KP_5, KEY_5}, {SDLK_5, KEY_5},
    {SDLK_KP_6, KEY_6}, {SDLK_6, KEY_6},
    {SDLK_KP_7, KEY_7}, {SDLK_7, KEY_7},
    {SDLK_KP_8, KEY_8}, {SDLK_8, KEY_8},
    {SDLK_KP_9, KEY_9}, {SDLK_9, KEY_9},
    {SDLK_KP_MULTIPLY, KEY_STAR},
    {SDLK_ASTERISK, KEY_STAR},
    {SDLK_HASH, KEY_HASH},
};

static KeyMask g_pressed = 0;
static KeyMask g_justPressed = 0;
static KeyMask g_justReleased = 0;

bool input_init()
{
    return true;
}

void input_shutdown()
{
}

void input_poll(InputState *out)
{
    g_justPressed = 0;
    g_justReleased = 0;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            out->pressed = 0;
            continue;
        }
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            auto it = g_keyMap.find(e.key.keysym.sym);
            if (it != g_keyMap.end()) {
                KeyCode kc = it->second;
                bool down = (e.type == SDL_KEYDOWN);
                bool wasDown = (g_pressed & kc) != 0;
                if (down && !wasDown) {
                    g_pressed |= kc;
                    g_justPressed |= kc;
                } else if (!down && wasDown) {
                    g_pressed &= ~kc;
                    g_justReleased |= kc;
                }
            }
        }
    }

    out->pressed = g_pressed;
    out->justPressed = g_justPressed;
    out->justReleased = g_justReleased;
}

const char *key_name(KeyCode kc)
{
    switch (kc) {
        case KEY_UP: return "UP";
        case KEY_DOWN: return "DOWN";
        case KEY_LEFT: return "LEFT";
        case KEY_RIGHT: return "RIGHT";
        case KEY_FIRE: return "FIRE";
        case KEY_SOFT1: return "SOFT1";
        case KEY_SOFT2: return "SOFT2";
        case KEY_0: return "0";
        case KEY_1: return "1";
        case KEY_2: return "2";
        case KEY_3: return "3";
        case KEY_4: return "4";
        case KEY_5: return "5";
        case KEY_6: return "6";
        case KEY_7: return "7";
        case KEY_8: return "8";
        case KEY_9: return "9";
        case KEY_STAR: return "*";
        case KEY_HASH: return "#";
        default: return "?";
    }
}

} // namespace hal