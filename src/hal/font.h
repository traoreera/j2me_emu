#pragma once
#include <cstdint>

namespace hal
{
    // Glyphe 5x7 de l'octet `c` (Latin-1 : ASCII + accents composés). 5 colonnes, bit 0 = rangée du haut.
    const uint8_t *font_glyph(unsigned char c);
} // namespace hal
