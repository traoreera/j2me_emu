#include "hal/inflate.h"
#include <cstring>

namespace jme
{

    // ---------------------------------------------------------------------
    // BitReader
    // ---------------------------------------------------------------------

    uint8_t BitReader::nextByte()
    {
        if (bufPos_ >= bufLen_)
        {
            bufLen_ = refill_(buffer_, sizeof(buffer_));
            bufPos_ = 0;
            if (bufLen_ == 0)
            {
                eof_ = true;
                return 0;
            }
        }
        return buffer_[bufPos_++];
    }

    int BitReader::getBit()
    {
        if (bitCount_ == 0)
        {
            bitBuf_ = nextByte();
            bitCount_ = 8;
        }
        int bit = bitBuf_ & 1;
        bitBuf_ >>= 1;
        bitCount_--;
        return bit;
    }

    uint32_t BitReader::getBits(int n)
    {
        uint32_t value = 0;
        for (int i = 0; i < n; i++)
        {
            value |= static_cast<uint32_t>(getBit()) << i;
        }
        return value;
    }

    void BitReader::alignToByte()
    {
        bitBuf_ = 0;
        bitCount_ = 0;
    }

    uint8_t BitReader::readByteAligned()
    {
        return nextByte();
    }

    // ---------------------------------------------------------------------
    // Tables Huffman canoniques (algorithme façon puff.c : construction à
    // partir des longueurs de code, décodage bit-à-bit).
    // ---------------------------------------------------------------------

    struct HuffTable
    {
        uint16_t counts[16] = {0};   // nb de codes par longueur (1..15)
        uint16_t symbols[288] = {0}; // symboles triés par (longueur, valeur)
    };

    static void buildHuffman(HuffTable &t, const uint8_t *lengths, int n)
    {
        std::memset(t.counts, 0, sizeof(t.counts));
        for (int i = 0; i < n; i++)
            t.counts[lengths[i]]++;
        t.counts[0] = 0; // les longueurs 0 = symbole absent

        uint16_t offs[16] = {0};
        for (int len = 1; len < 15; len++)
            offs[len + 1] = offs[len] + t.counts[len];

        for (int i = 0; i < n; i++)
        {
            if (lengths[i] != 0)
            {
                t.symbols[offs[lengths[i]]++] = static_cast<uint16_t>(i);
            }
        }
    }

    static int decodeSymbol(BitReader &br, const HuffTable &t)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; len++)
        {
            code |= br.getBit();
            int count = t.counts[len];
            if (code - first < count)
            {
                return t.symbols[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (br.eof())
                return -1;
        }
        return -1;
    }

    // Tables de longueur/distance imposées par RFC 1951 §3.2.5.
    static const uint16_t LENGTH_BASE[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t LENGTH_EXTRA[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t DIST_BASE[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t DIST_EXTRA[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    static const uint8_t CLEN_ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    static void buildFixedTables(HuffTable &lit, HuffTable &dist)
    {
        uint8_t litLengths[288];
        int i = 0;
        for (; i < 144; i++)
            litLengths[i] = 8;
        for (; i < 256; i++)
            litLengths[i] = 9;
        for (; i < 280; i++)
            litLengths[i] = 7;
        for (; i < 288; i++)
            litLengths[i] = 8;
        buildHuffman(lit, litLengths, 288);

        uint8_t distLengths[30];
        for (int d = 0; d < 30; d++)
            distLengths[d] = 5;
        buildHuffman(dist, distLengths, 30);
    }

    static bool buildDynamicTables(BitReader &br, HuffTable &lit, HuffTable &dist)
    {
        int hlit = static_cast<int>(br.getBits(5)) + 257;
        int hdist = static_cast<int>(br.getBits(5)) + 1;
        int hclen = static_cast<int>(br.getBits(4)) + 4;

        uint8_t clenLengths[19] = {0};
        for (int i = 0; i < hclen; i++)
        {
            clenLengths[CLEN_ORDER[i]] = static_cast<uint8_t>(br.getBits(3));
        }
        HuffTable clenTable;
        buildHuffman(clenTable, clenLengths, 19);

        uint8_t allLengths[288 + 32] = {0};
        int total = hlit + hdist;
        int n = 0;
        while (n < total)
        {
            int sym = decodeSymbol(br, clenTable);
            if (sym < 0)
                return false;
            if (sym < 16)
            {
                allLengths[n++] = static_cast<uint8_t>(sym);
            }
            else if (sym == 16)
            {
                if (n == 0)
                    return false;
                int repeat = static_cast<int>(br.getBits(2)) + 3;
                uint8_t prev = allLengths[n - 1];
                while (repeat-- > 0 && n < total)
                    allLengths[n++] = prev;
            }
            else if (sym == 17)
            {
                int repeat = static_cast<int>(br.getBits(3)) + 3;
                while (repeat-- > 0 && n < total)
                    allLengths[n++] = 0;
            }
            else
            { // sym == 18
                int repeat = static_cast<int>(br.getBits(7)) + 11;
                while (repeat-- > 0 && n < total)
                    allLengths[n++] = 0;
            }
        }

        buildHuffman(lit, allLengths, hlit);
        buildHuffman(dist, allLengths + hlit, hdist);
        return true;
    }

    // ---------------------------------------------------------------------
    // Boucle principale d'inflation
    // ---------------------------------------------------------------------

    size_t inflateStream(BitReader &br, uint8_t *out, size_t outCap)
    {
        size_t outPos = 0;
        bool final = false;

        while (!final)
        {
            final = br.getBits(1) != 0;
            uint32_t btype = br.getBits(2);

            if (btype == 0)
            { // stored (non compressé)
                br.alignToByte();
                uint16_t len = static_cast<uint16_t>(br.readByteAligned());
                len |= static_cast<uint16_t>(br.readByteAligned()) << 8;
                br.readByteAligned(); // NLEN (ignoré)
                br.readByteAligned();
                for (uint16_t i = 0; i < len; i++)
                {
                    if (outPos >= outCap)
                        return 0; // dépassement buffer
                    out[outPos++] = br.readByteAligned();
                }
            }
            else if (btype == 1 || btype == 2)
            {
                HuffTable litTable, distTable;
                if (btype == 1)
                {
                    buildFixedTables(litTable, distTable);
                }
                else
                {
                    if (!buildDynamicTables(br, litTable, distTable))
                        return 0;
                }

                for (;;)
                {
                    int sym = decodeSymbol(br, litTable);
                    if (sym < 0)
                        return 0;

                    if (sym < 256)
                    {
                        if (outPos >= outCap)
                            return 0;
                        out[outPos++] = static_cast<uint8_t>(sym);
                    }
                    else if (sym == 256)
                    {
                        break; // fin de bloc
                    }
                    else
                    {
                        int lenIdx = sym - 257;
                        if (lenIdx >= 29)
                            return 0;
                        uint32_t length = LENGTH_BASE[lenIdx] + br.getBits(LENGTH_EXTRA[lenIdx]);

                        int distSym = decodeSymbol(br, distTable);
                        if (distSym < 0 || distSym >= 30)
                            return 0;
                        uint32_t distance = DIST_BASE[distSym] + br.getBits(DIST_EXTRA[distSym]);

                        if (distance == 0 || distance > outPos)
                            return 0;
                        if (outPos + length > outCap)
                            return 0;

                        size_t srcPos = outPos - distance;
                        for (uint32_t i = 0; i < length; i++)
                        {
                            out[outPos] = out[srcPos];
                            outPos++;
                            srcPos++;
                        }
                    }
                    if (br.eof())
                        return 0;
                }
            }
            else
            {
                return 0; // btype == 3 : invalide
            }
        }

        return outPos;
    }

} // namespace jme