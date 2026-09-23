#pragma once

// kernel/drivers/input/input.h
// Contrat saisie : tableau de touches KeyCode (masque), avec axes évencer
// press/release par frame. Le MAPPING physique->MIDP reste côté VM (cf.
// halKeyToMidp) ; le noyau ne transporte que des identifiants KeyCode.
// Cible PC : hal/input.cpp (SDL). Cible MCU : matrice + GPIO en scan 60 Hz.

#include "../../../kernel/kernel.h"

namespace kernel
{
namespace input
{

enum KeyCode : uint32_t
{
    KEY_UP = 1u << 0,
    KEY_DOWN = 1u << 1,
    KEY_LEFT = 1u << 2,
    KEY_RIGHT = 1u << 3,
    KEY_FIRE = 1u << 4,
    KEY_SOFT1 = 1u << 5,
    KEY_SOFT2 = 1u << 6,
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
    KEY_STAR = 1u << 17,
    KEY_HASH = 1u << 18,
};

struct Frame
{
    uint32_t pressed;
    uint32_t justPressed;
    uint32_t justReleased;
    bool quit; // demande d'arrêt hôte (fenêtre/F12/Ctrl+Q)
};

using PollFn = void (*)(Frame *out);

struct Driver
{
    const char *backend; // "sdl" | "matrix"
    PollFn poll;
};

} // namespace input
} // namespace kernel