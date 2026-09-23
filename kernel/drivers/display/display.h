#pragma once

// kernel/drivers/display/display.h
// Contrat écran : framebuffer RGB565 (format natif de la cible MCU ; le PC
// le convertit en surface SDL). Implémentation cible : hal/display.cpp (PC),
// écran SPI + DMADMA (RP2040). Interface volontairement minimale.

#include "../../../kernel/kernel.h"

namespace kernel
{
namespace display
{

struct Framebuffer
{
    uint16_t *pixels;
    uint16_t stride; // largeur en pixels (octets de ligne)
    uint16_t width;
    uint16_t height;
};

using PresentFn = void (*)(const Framebuffer *fb);
using ClearFn = void (*)(uint16_t color);

// Pilote : foncteurs fournis par la cible.
struct Driver
{
    const char *backend;   // "sdl" | "st7789" | ...
    PresentFn present;     // pousse le framebuffer vers l'écran
    ClearFn clear;         // remplit le framebuffer
    Framebuffer *fb;       // framebuffer courant (lu/écrit par le rendu)
};

} // namespace display
} // namespace kernel