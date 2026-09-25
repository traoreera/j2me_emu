// test_png.cpp
// Tests du décodeur PNG (hal/png.*), non-entrelacé et Adam7. Les fixtures
// PNG ci-dessous ont été générées hors-ligne (Python, zlib + chunks IHDR/
// IDAT/IEND avec CRC32 corrects) pour des images RGB 8 bits minimales dont
// le contenu pixel exact est connu, afin de vérifier le pipeline complet
// (chunk scan -> inflate -> unfilter -> conversion ARGB) plutôt que juste le
// "ça ne plante pas".

#include "framework.h"
#include "hal/png.h"

#include <cstdint>
#include <vector>

using namespace jme;

namespace
{
    // 2x2 RGB non-entrelacé : rouge, vert, bleu, jaune (ordre ligne par ligne).
    const uint8_t kPng2x2[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00,
        0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xc0,
        0x00, 0xc2, 0x0c, 0xff, 0xff, 0xff, 0x67, 0x00, 0x00, 0x1e, 0xef, 0x04,
        0xfc, 0x73, 0x1c, 0x53, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e,
        0x44, 0xae, 0x42, 0x60, 0x82};

    // 4x4 RGB entrelacé Adam7 : pixel(x,y) = (60x, 60y, 30(x+y)).
    const uint8_t kPng4x4Interlaced[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04,
        0x08, 0x02, 0x00, 0x00, 0x01, 0x51, 0x94, 0x39, 0xbf, 0x00, 0x00, 0x00,
        0x31, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x15, 0xc6, 0x41, 0x11, 0x00,
        0x20, 0x10, 0xc3, 0xc0, 0x2a, 0x41, 0x49, 0x94, 0x9c, 0x92, 0x2a, 0x41,
        0x49, 0x04, 0x72, 0xe4, 0xb1, 0x93, 0x64, 0x6b, 0x58, 0x68, 0xff, 0x1c,
        0x33, 0xa1, 0x63, 0x6f, 0xc2, 0x01, 0xca, 0x48, 0x13, 0x07, 0x5b, 0xaf,
        0xfa, 0x00, 0x64, 0xc2, 0x10, 0xe1, 0x14, 0x18, 0xa2, 0xa7, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

    uint32_t argb(uint8_t r, uint8_t g, uint8_t b) { return 0xFF000000u | (r << 16) | (g << 8) | b; }
}

TEST(png_header_parses_2x2)
{
    PngHeader hdr;
    ASSERT_TRUE(pngHeader(kPng2x2, sizeof(kPng2x2), hdr));
    ASSERT_EQ(hdr.w, 2);
    ASSERT_EQ(hdr.h, 2);
    ASSERT_EQ((int)hdr.color, 2); // RGB
    ASSERT_EQ((int)hdr.interlace, 0);
}

TEST(png_decodes_2x2_pixels_exactly)
{
    PngHeader hdr;
    ASSERT_TRUE(pngHeader(kPng2x2, sizeof(kPng2x2), hdr));

    std::vector<uint8_t> work(hdr.idatLen + hdr.rawLen);
    std::vector<int32_t> px(static_cast<size_t>(hdr.w) * hdr.h);
    ASSERT_TRUE(pngPixels(kPng2x2, sizeof(kPng2x2), hdr, work.data(), work.size(), px.data()));

    ASSERT_EQ((uint32_t)px[0], argb(255, 0, 0));   // (0,0) rouge
    ASSERT_EQ((uint32_t)px[1], argb(0, 255, 0));   // (1,0) vert
    ASSERT_EQ((uint32_t)px[2], argb(0, 0, 255));   // (0,1) bleu
    ASSERT_EQ((uint32_t)px[3], argb(255, 255, 0)); // (1,1) jaune
}

TEST(png_decodes_adam7_interlaced_pixels_exactly)
{
    PngHeader hdr;
    ASSERT_TRUE(pngHeader(kPng4x4Interlaced, sizeof(kPng4x4Interlaced), hdr));
    ASSERT_EQ(hdr.w, 4);
    ASSERT_EQ(hdr.h, 4);
    ASSERT_EQ((int)hdr.interlace, 1);

    std::vector<uint8_t> work(hdr.idatLen + hdr.rawLen);
    std::vector<int32_t> px(static_cast<size_t>(hdr.w) * hdr.h);
    ASSERT_TRUE(pngPixels(kPng4x4Interlaced, sizeof(kPng4x4Interlaced), hdr, work.data(), work.size(), px.data()));

    for (int y = 0; y < 4; y++)
    {
        for (int x = 0; x < 4; x++)
        {
            uint32_t expected = argb(static_cast<uint8_t>(60 * x), static_cast<uint8_t>(60 * y),
                                      static_cast<uint8_t>(30 * (x + y)));
            ASSERT_EQ((uint32_t)px[y * 4 + x], expected);
        }
    }
}

TEST(png_header_rejects_bad_signature)
{
    uint8_t garbage[40] = {0};
    PngHeader hdr;
    ASSERT_FALSE(pngHeader(garbage, sizeof(garbage), hdr));
}

TEST(png_pixels_rejects_undersized_work_buffer)
{
    PngHeader hdr;
    ASSERT_TRUE(pngHeader(kPng2x2, sizeof(kPng2x2), hdr));

    std::vector<uint8_t> tooSmall(hdr.idatLen); // manque rawLen
    std::vector<int32_t> px(static_cast<size_t>(hdr.w) * hdr.h);
    ASSERT_FALSE(pngPixels(kPng2x2, sizeof(kPng2x2), hdr, tooSmall.data(), tooSmall.size(), px.data()));
}
