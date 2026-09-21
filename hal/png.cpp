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
    if (depth != 8 || interlace != 0)
        return false;
    if (!(color == 0 || color == 2 || color == 3 || color == 4 || color == 6))
        return false;
    if (w < 1 || h < 1 || w > 4096 || h > 4096)
        return false;

    int bpp = (color == 0 || color == 3) ? 1 : (color == 4) ? 2 : (color == 6) ? 4 : 3;
    out.w = w;
    out.h = h;
    out.color = color;
    out.bpl = static_cast<size_t>(w) * static_cast<size_t>(bpp);
    out.rawLen = (out.bpl + 1) * static_cast<size_t>(h);
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

    const size_t bpl = hdr.bpl;
    int bpp = (color == 0 || color == 3) ? 1 : (color == 4) ? 2 : (color == 6) ? 4 : 3;
    const size_t ub = static_cast<size_t>(bpp);
    for (int y = 0; y < hdr.h; y++)
    {
        uint8_t filter = raw[static_cast<size_t>(y) * (bpl + 1)];
        uint8_t *row = raw + static_cast<size_t>(y) * (bpl + 1) + 1;
        const uint8_t *prev = y > 0 ? raw + static_cast<size_t>(y - 1) * (bpl + 1) + 1 : nullptr;

        switch (filter)
        {
        case 0: break;
        case 1: // Sub : le 1er pixel est brut, les suivants sont relatifs
            for (size_t x = ub; x < bpl; x++)
                row[x] = static_cast<uint8_t>(row[x] + row[x - ub]);
            break;
        case 2: // Up
            for (size_t x = 0; x < bpl; x++)
                row[x] = static_cast<uint8_t>(row[x] + (prev ? prev[x] : 0));
            break;
        case 3: // Average
            for (size_t x = 0; x < bpl; x++)
            {
                uint8_t left = (x >= ub) ? row[x - ub] : 0;
                uint8_t up = prev ? prev[x] : 0;
                row[x] = static_cast<uint8_t>(row[x] + ((uint16_t(left) + uint16_t(up)) >> 1));
            }
            break;
        case 4: // Paeth
            for (size_t x = 0; x < bpl; x++)
            {
                uint8_t a = (x >= ub) ? row[x - ub] : 0;
                uint8_t b = prev ? prev[x] : 0;
                uint8_t cc = (x >= ub && prev) ? prev[x - ub] : 0;
                row[x] = static_cast<uint8_t>(row[x] + static_cast<uint8_t>(paeth(a, b, cc)));
            }
            break;
        default:
            return false;
        }

        switch (color)
        {
        case 6:
            for (int x = 0; x < hdr.w; x++)
            {
                uint8_t *p = row + size_t(x) * 4;
                px[y * hdr.w + x] = (uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
            }
            break;
        case 2:
            for (int x = 0; x < hdr.w; x++)
            {
                uint8_t *p = row + size_t(x) * 3;
                px[y * hdr.w + x] = 0xFF000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
            }
            break;
        case 3:
            for (int x = 0; x < hdr.w; x++)
            {
                uint8_t idx = row[x];
                px[y * hdr.w + x] = (palAlpha[idx] << 24) | (pal[idx] & 0xFFFFFF);
            }
            break;
        case 0:
            for (int x = 0; x < hdr.w; x++)
            {
                uint8_t g = row[x];
                px[y * hdr.w + x] = 0xFF000000u | (uint32_t(g) << 16) | (uint32_t(g) << 8) | uint32_t(g);
            }
            break;
        case 4:
            for (int x = 0; x < hdr.w; x++)
            {
                uint8_t *p = row + size_t(x) * 2;
                uint8_t g = p[0];
                px[y * hdr.w + x] = (uint32_t(p[1]) << 24) | (uint32_t(g) << 16) | (uint32_t(g) << 8) | uint32_t(g);
            }
            break;
        default:
            return false;
        }
    }
    return true;
}

} // namespace jme