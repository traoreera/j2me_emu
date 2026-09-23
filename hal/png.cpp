// png.cpp
// Décodeur PNG minimal (voir png.h).

#include "png.h"

#include <cstring>
#include <cstdlib>
#include "inflate.h"

namespace jme
{

namespace
{

uint32_t be32(const uint8_t *p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

int bppFor(uint8_t color)
{
    return (color == 0 || color == 3) ? 1 : (color == 4) ? 2 : (color == 6) ? 4 : 3;
}

// Les 7 passes Adam7 (spec PNG 8.2) : origine (sx,sy) et pas (dx,dy) en
// pixels de l'image finale. Chaque passe est un sous-échantillonnage de la
// grille complète ; certaines passes sont vides pour les petites images
// (ex. 1 px de large), auquel cas elles ne contribuent aucune scanline.
struct Adam7Pass { int sx, sy, dx, dy; };
const Adam7Pass kAdam7[7] = {
    {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
    {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2},
};

void adam7PassDims(int w, int h, const Adam7Pass &p, int &cols, int &rows)
{
    cols = (w > p.sx) ? (w - p.sx + p.dx - 1) / p.dx : 0;
    rows = (h > p.sy) ? (h - p.sy + p.dy - 1) / p.dy : 0;
}

// Applique le filtre PNG (spec 9.2) à une scanline déjà en place dans `row`
// (bpl octets), à l'aide de la ligne précédente `prev` (nullptr si première
// ligne de l'image/de la passe -- chaque passe Adam7 refiltre depuis zéro).
bool unfilterRow(uint8_t filter, uint8_t *row, const uint8_t *prev, size_t bpl, size_t bpp)
{
    switch (filter)
    {
    case 0: break;
    case 1:
        for (size_t x = bpp; x < bpl; x++)
            row[x] = static_cast<uint8_t>(row[x] + row[x - bpp]);
        break;
    case 2:
        for (size_t x = 0; x < bpl; x++)
            row[x] = static_cast<uint8_t>(row[x] + (prev ? prev[x] : 0));
        break;
    case 3:
        for (size_t x = 0; x < bpl; x++)
        {
            uint8_t left = (x >= bpp) ? row[x - bpp] : 0;
            uint8_t up = prev ? prev[x] : 0;
            row[x] = static_cast<uint8_t>(row[x] + ((uint16_t(left) + uint16_t(up)) >> 1));
        }
        break;
    case 4:
        for (size_t x = 0; x < bpl; x++)
        {
            uint8_t a = (x >= bpp) ? row[x - bpp] : 0;
            uint8_t b = prev ? prev[x] : 0;
            uint8_t cc = (x >= bpp && prev) ? prev[x - bpp] : 0;
            row[x] = static_cast<uint8_t>(row[x] + static_cast<uint8_t>(paeth(a, b, cc)));
        }
        break;
    default:
        return false;
    }
    return true;
}

// Convertit `cols` pixels déjà défiltrés (`row`, color type `color`) en
// ARGB, écrits dans px[destBase + i*destStride] pour i in [0,cols) --
// destStride=1/destBase=y*w pour une image non entrelacée, destStride=dx
// pour une ligne d'une passe Adam7 (pixels dispersés horizontalement).
bool convertRow(const uint8_t *row, int cols, uint8_t color, uint8_t bitDepth,
                 const uint32_t *pal, const uint32_t *palAlpha,
                 int32_t *px, int destBase, int destStride)
{
    switch (color)
    {
    case 6:
        for (int x = 0; x < cols; x++)
        {
            const uint8_t *p = row + size_t(x) * 4;
            px[destBase + x * destStride] = (uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
        }
        break;
    case 2:
        for (int x = 0; x < cols; x++)
        {
            const uint8_t *p = row + size_t(x) * 3;
            px[destBase + x * destStride] = 0xFF000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
        }
        break;
    case 3:
        for (int x = 0; x < cols; x++)
        {
            // Pixels de palette potentiellement tassés (depth 1/2/4/8), MSB d'abord.
            uint8_t bitDepth8 = bitDepth == 0 ? 8 : bitDepth;
            size_t bit = size_t(x) * bitDepth8;
            uint8_t idx8 = (row[bit >> 3] >> (8 - bitDepth8 - (bit & 7))) & uint8_t((1u << bitDepth8) - 1);
            int idx = idx8;
            px[destBase + x * destStride] = (palAlpha[idx] << 24) | (pal[idx] & 0xFFFFFF);
        }
        break;
    case 0:
        for (int x = 0; x < cols; x++)
        {
            uint8_t g = row[x];
            px[destBase + x * destStride] = 0xFF000000u | (uint32_t(g) << 16) | (uint32_t(g) << 8) | uint32_t(g);
        }
        break;
    case 4:
        for (int x = 0; x < cols; x++)
        {
            const uint8_t *p = row + size_t(x) * 2;
            uint8_t g = p[0];
            px[destBase + x * destStride] = (uint32_t(p[1]) << 24) | (uint32_t(g) << 16) | (uint32_t(g) << 8) | uint32_t(g);
        }
        break;
    default:
        return false;
    }
    return true;
}

} // namespace

bool pngHeader(const uint8_t *d, size_t n, PngHeader &out)
{
    static const uint8_t sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (n < 33)
        return false;
    if (memcmp(d, sig, 8) != 0)
        return false;
    if (be32(d + 8) != 13 || memcmp(d + 12, "IHDR", 4) != 0)
        return false;

    int w = static_cast<int>(be32(d + 16));
    int h = static_cast<int>(be32(d + 20));
    uint8_t depth = d[24], color = d[25], interlace = d[28];
    if (interlace != 0 && interlace != 1)
        return false;
    if (!(color == 0 || color == 2 || color == 3 || color == 4 || color == 6))
        return false;
    // Palettes indexées à 1/2/4/8 bits (bits tassés) ; autres color types 8 bits.
    if (color == 3)
    {
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8)
            return false;
    }
    else if (depth != 8)
    {
        return false;
    }
    if (w < 1 || h < 1 || w > 4096 || h > 4096)
        return false;

    size_t bitsPerPix = (color == 3) ? static_cast<size_t>(depth) : static_cast<size_t>(bppFor(color)) * 8;
    auto rowBytes = [&](int cols) { return (static_cast<size_t>(cols) * bitsPerPix + 7) / 8; };
    out.w = w;
    out.h = h;
    out.color = color;
    out.bitDepth = depth;
    out.interlace = interlace;
    out.bpl = rowBytes(w);
    if (interlace == 0)
    {
        out.rawLen = (out.bpl + 1) * static_cast<size_t>(h);
    }
    else
    {
        out.rawLen = 0;
        for (const Adam7Pass &p : kAdam7)
        {
            int cols, rows;
            adam7PassDims(w, h, p, cols, rows);
            if (cols > 0 && rows > 0)
                out.rawLen += (rowBytes(cols) + 1) * static_cast<size_t>(rows);
        }
    }
    out.idatLen = 0;

    size_t o = 8;
    while (o + 12 <= n)
    {
        uint32_t clen = be32(d + o);
        if (clen > n - o - 8)
            return false;
        const char *type = reinterpret_cast<const char *>(d + o + 4);
        if (memcmp(type, "IDAT", 4) == 0)
            out.idatLen += clen;
        else if (memcmp(type, "IEND", 4) == 0)
            break;
        o += 12 + static_cast<size_t>(clen);
    }
    return out.idatLen > 0;
}

bool pngPixels(const uint8_t *d, size_t n, const PngHeader &hdr,
               uint8_t *work, size_t workCap, int32_t *px)
{
    if (workCap < hdr.idatLen + hdr.rawLen)
        return false;
    if (hdr.rawLen == 0 || hdr.idatLen == 0)
        return false;

    uint32_t pal[256];
    uint32_t palAlpha[256];
    for (int i = 0; i < 256; i++)
    {
        pal[i] = 0xFF000000;
        palAlpha[i] = 255;
    }
    bool havePal = false;

    uint8_t color = hdr.color;
    size_t idatPos = 0;

    size_t o = 8;
    while (o + 12 <= n)
    {
        uint32_t clen = be32(d + o);
        if (clen > n - o - 8)
            return false;
        const char *type = reinterpret_cast<const char *>(d + o + 4);
        const uint8_t *ch = d + o + 8;
        if (memcmp(type, "PLTE", 4) == 0)
        {
            size_t k = clen / 3;
            if (k > 256) k = 256;
            for (size_t i = 0; i < k; i++)
                pal[i] = 0xFF000000u | (uint32_t(ch[i * 3 + 0]) << 16) | (uint32_t(ch[i * 3 + 1]) << 8) | uint32_t(ch[i * 3 + 2]);
            havePal = true;
        }
        else if (memcmp(type, "tRNS", 4) == 0)
        {
            size_t k = clen;
            if (k > 256) k = 256;
            for (size_t i = 0; i < k; i++)
                palAlpha[i] = ch[i];
        }
        else if (memcmp(type, "IDAT", 4) == 0 && idatPos + clen <= hdr.idatLen)
        {
            memcpy(work + idatPos, ch, clen);
            idatPos += clen;
        }
        else if (memcmp(type, "IEND", 4) == 0)
            break;
        o += 12 + static_cast<size_t>(clen);
    }

    if (color == 3 && !havePal)
        return false;

    uint8_t *raw = work + hdr.idatLen;

    // Décompresse les chunks IDAT concaténés (flux zlib : en-tête 2 octets +
    // DEFLATE + adler32 ignoré).
    size_t srcIdx = 0;
    BitReader br([&](uint8_t *buf, size_t maxLen) -> size_t {
        size_t avail = hdr.idatLen - srcIdx;
        size_t c = avail < maxLen ? avail : maxLen;
        memcpy(buf, work + srcIdx, c);
        srcIdx += c;
        return c;
    });
    br.alignToByte();
    br.readByteAligned(); // CMF
    br.readByteAligned(); // FLG
    size_t got = inflateStream(br, raw, hdr.rawLen);
    if (got != hdr.rawLen)
        return false;

    int bpp = bppFor(color);

    if (hdr.interlace == 0)
    {
        const size_t bpl = hdr.bpl;
        uint8_t *prevRow = nullptr;
        for (int y = 0; y < hdr.h; y++)
        {
            uint8_t filter = raw[static_cast<size_t>(y) * (bpl + 1)];
            uint8_t *row = raw + static_cast<size_t>(y) * (bpl + 1) + 1;
            if (!unfilterRow(filter, row, prevRow, bpl, static_cast<size_t>(bpp)))
                return false;
            if (!convertRow(row, hdr.w, color, hdr.bitDepth, pal, palAlpha, px, y * hdr.w, 1))
                return false;
            prevRow = row;
        }
        return true;
    }

    // Adam7 : le flux décompressé est la concaténation de 7 sous-images
    // indépendantes (chacune avec son propre filtrage par scanline, qui
    // repart de zéro à chaque passe), chacune couvrant un sous-ensemble
    // dispersé des pixels finaux (cf. kAdam7). On défiltre chaque passe
    // comme une image à part entière puis on disperse ses pixels dans `px`
    // aux coordonnées (sx + col*dx, sy + row*dy).
    size_t off = 0;
    for (const Adam7Pass &p : kAdam7)
    {
        int cols, rows;
        adam7PassDims(hdr.w, hdr.h, p, cols, rows);
        if (cols <= 0 || rows <= 0)
            continue;
        const size_t passBpl = (static_cast<size_t>(cols) * ((hdr.color == 3) ? static_cast<size_t>(hdr.bitDepth) : static_cast<size_t>(bppFor(hdr.color)) * 8) + 7) / 8;
        uint8_t *prevRow = nullptr;
        for (int r = 0; r < rows; r++)
        {
            uint8_t filter = raw[off];
            uint8_t *row = raw + off + 1;
            if (!unfilterRow(filter, row, prevRow, passBpl, static_cast<size_t>(bppFor(hdr.color))))
                return false;
            int destBase = (p.sy + r * p.dy) * hdr.w + p.sx;
            if (!convertRow(row, cols, hdr.color, hdr.bitDepth, pal, palAlpha, px, destBase, p.dx))
                return false;
            prevRow = row;
            off += passBpl + 1;
        }
    }
    return true;
}

} // namespace jme