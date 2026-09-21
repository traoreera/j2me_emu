// png.h
// Décodeur PNG minimal pour l'émulateur J2ME : RGB565/ARGB pour Image MIDP.
// Supporte PNG 8 bits, non entrelacé, les color types 0/gris, 2/RGB,
// 3/palette, 4/gris+alpha, 6/RGBA (le standard des jeux J2ME).
//
// Pas d'allocation dynamique : l'appelant fournit un buffer de travail
// (taille = idatLen + rawLen, cf. pngHeader) et la zone pixels [w*h].
// Le flux IDAT est décompressé avec jme::inflateStream (DEFLATE).

#pragma once

#include <cstdint>
#include <cstddef>

namespace jme
{

    struct PngHeader
    {
        int w = 0;
        int h = 0;
        uint8_t color = 0; // color type IHDR (0,2,3,4,6)
        size_t bpl = 0;    // octets par ligne (sans filtre)
        size_t rawLen = 0; // (1 + bpl) * h  -> scanlines non filtrées
        size_t idatLen = 0; // taille cumulée des chunks IDAT
    };

    // Valide la signature et remplit l'en-tête (dimensions + tailles).
    bool pngHeader(const uint8_t *data, size_t len, PngHeader &out);

    // Décode le corps. `work` doit faire >= out.idatLen + out.rawLen :
    //   work[0 .. idatLen) rassemble les chunks IDAT
    //   work[idatLen ..] reçoit les scanlines décompressées
    // `px` (w*h) reçoit les pixels ARGB 0xAARRGGBB.
    bool pngPixels(const uint8_t *data, size_t len, const PngHeader &hdr,
                   uint8_t *work, size_t workCap, int32_t *px);

} // namespace jme