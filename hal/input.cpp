#include "hal/input.h"
#include <SDL2/SDL.h>
#include <unordered_map>
#include <string>
#include <cctype>

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

static KeyCode keyCodeForToken(const std::string &token)
{
    std::string up(token);
    for (auto &c : up)
        c = (char)std::toupper((unsigned char)c);
    if (up == "UP") return KEY_UP;
    if (up == "DOWN") return KEY_DOWN;
    if (up == "LEFT") return KEY_LEFT;
    if (up == "RIGHT") return KEY_RIGHT;
    if (up == "FIRE") return KEY_FIRE;
    if (up == "SOFT1") return KEY_SOFT1;
    if (up == "SOFT2") return KEY_SOFT2;
    if (up == "STAR") return KEY_STAR;
    if (up == "HASH") return KEY_HASH;
    if (up.size() == 1 && up[0] >= '0' && up[0] <= '9')
        return (KeyCode)(KEY_0 << (up[0] - '0'));
    if (up == "NONE") return KEY_NONE;
    return KEY_NONE;
}

static KeyMask g_pressed = 0;
static KeyMask g_justPressed = 0;
static KeyMask g_justReleased = 0;
static bool g_quitRequested = false;

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
            g_quitRequested = true;
        }
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            bool down = (e.type == SDL_KEYDOWN);
            if (down && e.key.keysym.sym == SDLK_F12)
                g_quitRequested = true;
            if (down && e.key.keysym.sym == SDLK_q && (e.key.keysym.mod & KMOD_CTRL))
                g_quitRequested = true;
            auto it = g_keyMap.find(e.key.keysym.sym);
            if (it != g_keyMap.end()) {
                KeyCode kc = it->second;
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
    out->quit = g_quitRequested;
}

bool input_applyKeyMap(const char *spec)
{
    if (!spec || !*spec)
        return false;
    bool any = false;
    std::string s(spec);
    size_t pos = 0;
    while (pos < s.size())
    {
        size_t next = s.find_first_of(",\n;", pos);
        std::string item = s.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = (next == std::string::npos) ? s.size() : next + 1;
        size_t eq = item.find('=');
        if (eq == std::string::npos)
            continue;
        std::string sdlName = item.substr(0, eq);
        std::string token = item.substr(eq + 1);
        while (!sdlName.empty() && (sdlName.front() == ' ' || sdlName.front() == '\t' || sdlName.front() == '\r'))
            sdlName.erase(sdlName.begin());
        while (!sdlName.empty() && (sdlName.back() == ' ' || sdlName.back() == '\t' || sdlName.back() == '\r'))
            sdlName.pop_back();
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t' || token.front() == '\r'))
            token.erase(token.begin());
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t' || token.back() == '\r'))
            token.pop_back();
        if (sdlName.empty() || token.empty())
            continue;

        KeyCode kc = keyCodeForToken(token);
        bool wantUnbind = kc == KEY_NONE;

        SDL_Keycode sym = SDL_GetKeyFromName(sdlName.c_str());
        if (sym == SDLK_UNKNOWN)
        {
            fprintf(stderr, "[keys] touche SDL inconnue: \"%s\"\n", sdlName.c_str());
            continue;
        }

        // Une KeyCode donnée ne vit que sur UNE touche physique : on retire d'abord
        // les autres liaisons de la même KeyCode pour éviter les doubles envois.
        for (auto it = g_keyMap.begin(); it != g_keyMap.end();)
        {
            if (it->first == sym)
                it = g_keyMap.erase(it);
            else if (!wantUnbind && it->second == kc)
            {
                g_keyMap.erase(it++);
            }
            else
                ++it;
        }
        if (!wantUnbind)
            g_keyMap[sym] = kc;
        fprintf(stderr, "[keys] %s => %s\n", sdlName.c_str(), token.c_str());
        any = true;
    }
    return any;
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