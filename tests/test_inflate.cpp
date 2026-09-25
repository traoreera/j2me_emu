// test_inflate.cpp
// Tests du décodeur DEFLATE (hal/inflate.*). Les flux compressés ci-dessous
// (blocs "fixed"/"dynamic" Huffman) ont été générés hors-ligne avec le zlib
// de Python (deflate brut, wbits=-15, donc directement compatible avec
// jme::inflateStream qui ne gère pas l'en-tête/trailer zlib) puis vérifiés
// par un round-trip zlib avant d'être figés ici.

#include "framework.h"
#include "hal/inflate.h"

#include <cstring>
#include <memory>
#include <string>

using namespace jme;

namespace
{
    // Construit un BitReader qui sert `len` octets de `data` via le refill callback.
    BitReader makeReader(const uint8_t *data, size_t len, std::shared_ptr<size_t> pos)
    {
        return BitReader([data, len, pos](uint8_t *buf, size_t maxLen) -> size_t {
            size_t avail = len - *pos;
            size_t c = avail < maxLen ? avail : maxLen;
            std::memcpy(buf, data + *pos, c);
            *pos += c;
            return c;
        });
    }
}

TEST(inflate_stored_block_roundtrip)
{
    // BFINAL=1, BTYPE=00 (stored), LEN=3, NLEN=~3, données "Hi!"
    static const uint8_t comp[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 0x48, 0x69, 0x21};
    auto pos = std::make_shared<size_t>(0);
    BitReader br = makeReader(comp, sizeof(comp), pos);

    uint8_t out[16] = {0};
    size_t n = inflateStream(br, out, sizeof(out));

    ASSERT_EQ(n, (size_t)3);
    ASSERT_EQ(std::string(reinterpret_cast<char *>(out), n), std::string("Hi!"));
}

TEST(inflate_stored_block_output_overflow_fails)
{
    static const uint8_t comp[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 0x48, 0x69, 0x21};
    auto pos = std::make_shared<size_t>(0);
    BitReader br = makeReader(comp, sizeof(comp), pos);

    uint8_t out[2] = {0}; // trop petit pour les 3 octets du bloc
    size_t n = inflateStream(br, out, sizeof(out));
    ASSERT_EQ(n, (size_t)0);
}

TEST(inflate_fixed_huffman_block_roundtrip)
{
    // zlib.compressobj(9, DEFLATED, -15, 8, Z_FIXED) sur ce texte -> force
    // BTYPE=1 (Huffman fixe), pas de table dynamique -- exerce
    // buildFixedTables()/decodeSymbol() avec des back-references réelles.
    static const uint8_t comp[] = {
        0x2b, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c, 0xce, 0x56, 0x48, 0x2a,
        0xca, 0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50, 0xc8, 0x2a, 0xcd, 0x2d,
        0x28, 0x56, 0xc8, 0x2f, 0x4b, 0x2d, 0x52, 0x28, 0x01, 0x4a, 0xe7, 0x24,
        0x56, 0x55, 0x2a, 0xa4, 0xe4, 0xa7, 0xeb, 0x81, 0x79, 0xd8, 0x15, 0x27,
        0xa6, 0x27, 0x66, 0xe6, 0x29, 0x02, 0x00};
    static const char *expected = "the quick brown fox jumps over the lazy dog. the quick brown fox jumps again!";

    auto pos = std::make_shared<size_t>(0);
    BitReader br = makeReader(comp, sizeof(comp), pos);

    uint8_t out[128] = {0};
    size_t n = inflateStream(br, out, sizeof(out));

    ASSERT_EQ(n, std::strlen(expected));
    ASSERT_EQ(std::string(reinterpret_cast<char *>(out), n), std::string(expected));
}

TEST(inflate_dynamic_huffman_block_roundtrip)
{
    // zlib.compressobj(9, DEFLATED, -15) par défaut sur un texte assez varié
    // pour forcer BTYPE=2 (tables Huffman dynamiques) -- exerce
    // buildDynamicTables() (HLIT/HDIST/HCLEN + répétitions 16/17/18).
    static const uint8_t comp[] = {
        0x25, 0x8c, 0xc1, 0x0d, 0xc3, 0x30, 0x0c, 0x03, 0x57, 0xe1, 0x00, 0x45,
        0x26, 0xe9, 0xb7, 0x03, 0xa8, 0x96, 0x10, 0x10, 0xb0, 0x64, 0x27, 0x96,
        0x3a, 0x7f, 0x5d, 0xf4, 0xc7, 0x03, 0x71, 0xf7, 0x1c, 0xb7, 0x39, 0x38,
        0x57, 0x39, 0x74, 0xf4, 0x71, 0x63, 0x31, 0x21, 0x6e, 0xf9, 0x40, 0x1b,
        0xb1, 0xac, 0xa5, 0x65, 0xdd, 0x10, 0xe5, 0xe4, 0x6a, 0x8c, 0x13, 0xd6,
        0xb9, 0xcf, 0x65, 0xba, 0x05, 0x18, 0x6b, 0xf9, 0x50, 0xa4, 0xf9, 0xdc,
        0x32, 0xa3, 0x51, 0xa9, 0x15, 0x89, 0x4a, 0x74, 0x79, 0xef, 0x3c, 0x2c,
        0xff, 0x69, 0x83, 0xcb, 0x19, 0x02, 0xe9, 0xbc, 0x4a, 0x0e, 0xbc, 0x12,
        0x16, 0xf4, 0xdd, 0x86, 0xf3, 0x37, 0x3e, 0x1b, 0xc5, 0x8f, 0x2f};
    static const char *expected =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor "
        "incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam.";

    auto pos = std::make_shared<size_t>(0);
    BitReader br = makeReader(comp, sizeof(comp), pos);

    uint8_t out[256] = {0};
    size_t n = inflateStream(br, out, sizeof(out));

    ASSERT_EQ(n, std::strlen(expected));
    ASSERT_EQ(std::string(reinterpret_cast<char *>(out), n), std::string(expected));
}

TEST(inflate_invalid_block_type_fails)
{
    // BFINAL=1, BTYPE=11 (reserve, invalide) : premier octet = 0b00000111.
    static const uint8_t comp[] = {0x07};
    auto pos = std::make_shared<size_t>(0);
    BitReader br = makeReader(comp, sizeof(comp), pos);

    uint8_t out[8] = {0};
    ASSERT_EQ(inflateStream(br, out, sizeof(out)), (size_t)0);
}
