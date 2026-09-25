// test_jar_reader.cpp
// Tests du lecteur JAR/ZIP (hal/jar_reader.*) : parseManifestText() (fonction
// pure, exposee separement pour ca dans jar_reader.h) et un round-trip
// complet -- un .zip minimal (stored + deflate) est construit a la main,
// ecrit dans un fichier temporaire, puis relu via JarReader pour exercer la
// localisation de l'EOCD, le scan du central directory et la decompression
// des deux methodes (0=stored, 8=deflate).

#include "framework.h"
#include "hal/jar_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace jme;

// ---------------------------------------------------------------------
// parseManifestText (pur, sans I/O)
// ---------------------------------------------------------------------

TEST(parse_manifest_extracts_midlet1_main_class)
{
    const char *text =
        "MIDlet-Name: Demo\n"
        "MIDlet-Vendor: Acme\n"
        "MIDlet-Version: 1.0\n"
        "MIDlet-1: Demo, icon.png, com.acme.Demo\n";
    ManifestInfo info;
    ASSERT_TRUE(parseManifestText(text, std::strlen(text), info));
    ASSERT_TRUE(info.valid);
    ASSERT_EQ(info.mainClass, std::string("com.acme.Demo"));
    ASSERT_EQ(info.midletName, std::string("Demo"));
    ASSERT_EQ(info.midletVendor, std::string("Acme"));
    ASSERT_EQ(info.midletVersion, std::string("1.0"));
}

TEST(parse_manifest_midlet1_without_icon_field)
{
    // Format simplifie "Nom, Classe" (pas d'icone) : le dernier champ apres
    // la derniere virgule doit rester la classe principale.
    const char *text = "MIDlet-1: Demo, com.acme.Demo\n";
    ManifestInfo info;
    ASSERT_TRUE(parseManifestText(text, std::strlen(text), info));
    ASSERT_EQ(info.mainClass, std::string("com.acme.Demo"));
}

TEST(parse_manifest_missing_midlet1_is_invalid)
{
    const char *text = "MIDlet-Name: Demo\n";
    ManifestInfo info;
    ASSERT_FALSE(parseManifestText(text, std::strlen(text), info));
    ASSERT_FALSE(info.valid);
}

// ---------------------------------------------------------------------
// Round-trip ZIP complet (construit a la main, ecrit sur disque, relu).
// ---------------------------------------------------------------------

namespace
{
    void putU16(std::vector<uint8_t> &v, uint16_t x)
    {
        v.push_back(static_cast<uint8_t>(x));
        v.push_back(static_cast<uint8_t>(x >> 8));
    }
    void putU32(std::vector<uint8_t> &v, uint32_t x)
    {
        v.push_back(static_cast<uint8_t>(x));
        v.push_back(static_cast<uint8_t>(x >> 8));
        v.push_back(static_cast<uint8_t>(x >> 16));
        v.push_back(static_cast<uint8_t>(x >> 24));
    }
    void putBytes(std::vector<uint8_t> &v, const uint8_t *p, size_t n)
    {
        v.insert(v.end(), p, p + n);
    }
    void putStr(std::vector<uint8_t> &v, const std::string &s)
    {
        putBytes(v, reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }

    struct ZipEntrySpec
    {
        std::string name;
        uint16_t method; // 0 = stored, 8 = deflate
        std::vector<uint8_t> data;       // contenu (compresse si method==8)
        uint32_t uncompressedSize;
    };

    // Construit un .zip standard (local headers + central directory + EOCD).
    // CRC32 laisse a 0 : hal/jar_reader.cpp ne le verifie pas (cf. lecture du
    // code : decompressEntry n'y touche jamais).
    std::vector<uint8_t> buildZip(const std::vector<ZipEntrySpec> &entries)
    {
        std::vector<uint8_t> out;
        std::vector<uint32_t> localOffsets;

        for (const auto &e : entries)
        {
            localOffsets.push_back(static_cast<uint32_t>(out.size()));
            putU32(out, 0x04034b50); // local file header signature
            putU16(out, 20);         // version needed
            putU16(out, 0);          // flags
            putU16(out, e.method);   // compression method
            putU16(out, 0);          // mod time
            putU16(out, 0);          // mod date
            putU32(out, 0);          // crc32 (non verifie par le lecteur)
            putU32(out, static_cast<uint32_t>(e.data.size())); // compressed size
            putU32(out, e.uncompressedSize);
            putU16(out, static_cast<uint16_t>(e.name.size()));
            putU16(out, 0); // extra length
            putStr(out, e.name);
            putBytes(out, e.data.data(), e.data.size());
        }

        uint32_t cdStart = static_cast<uint32_t>(out.size());
        for (size_t i = 0; i < entries.size(); i++)
        {
            const auto &e = entries[i];
            putU32(out, 0x02014b50); // central directory signature
            putU16(out, 20);         // version made by
            putU16(out, 20);         // version needed
            putU16(out, 0);          // flags
            putU16(out, e.method);
            putU16(out, 0); // mod time
            putU16(out, 0); // mod date
            putU32(out, 0); // crc32
            putU32(out, static_cast<uint32_t>(e.data.size()));
            putU32(out, e.uncompressedSize);
            putU16(out, static_cast<uint16_t>(e.name.size()));
            putU16(out, 0); // extra length
            putU16(out, 0); // comment length
            putU16(out, 0); // disk number start
            putU16(out, 0); // internal attrs
            putU32(out, 0); // external attrs
            putU32(out, localOffsets[i]);
            putStr(out, e.name);
        }
        uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdStart;

        putU32(out, 0x06054b50); // EOCD signature
        putU16(out, 0);          // disk number
        putU16(out, 0);          // disk with cd
        putU16(out, static_cast<uint16_t>(entries.size()));
        putU16(out, static_cast<uint16_t>(entries.size()));
        putU32(out, cdSize);
        putU32(out, cdStart);
        putU16(out, 0); // comment length

        return out;
    }

    // RAII : ecrit les octets dans un fichier temporaire et le supprime a la
    // destruction.
    struct TempFile
    {
        std::string path;
        explicit TempFile(const std::vector<uint8_t> &bytes)
        {
            char tmpl[] = "/tmp/j2me_test_zip_XXXXXX";
            int fd = mkstemp(tmpl);
            path = tmpl;
            FILE *f = fdopen(fd, "wb");
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
        ~TempFile() { std::remove(path.c_str()); }
    };
} // namespace

TEST(jar_reader_roundtrip_stored_and_deflate_entries)
{
    const std::string helloContent = "Hello, J2ME!\n";

    // "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCAAAAAAAAAABBBBBBBBBBCCCCCCCCCC" (60
    // octets) compresse en DEFLATE brut (zlib, wbits=-15), precalcule hors
    // ligne -- exerce a la fois les back-references LZ77 et l'integration
    // jar_reader -> inflateStream.
    static const uint8_t dataBinCompressed[] = {
        0x73, 0x74, 0x84, 0x01, 0x27, 0x38, 0x70, 0x86, 0x03, 0x47, 0xbc, 0xb2, 0x00};
    const std::string dataBinExpected =
        "AAAAAAAAAABBBBBBBBBBCCCCCCCCCCAAAAAAAAAABBBBBBBBBBCCCCCCCCCC";

    std::vector<ZipEntrySpec> entries;
    entries.push_back({"hello.txt", 0,
                        std::vector<uint8_t>(helloContent.begin(), helloContent.end()),
                        static_cast<uint32_t>(helloContent.size())});
    entries.push_back({"data.bin", 8,
                        std::vector<uint8_t>(dataBinCompressed, dataBinCompressed + sizeof(dataBinCompressed)),
                        static_cast<uint32_t>(dataBinExpected.size())});

    TempFile tf(buildZip(entries));

    JarReader jar;
    ASSERT_TRUE(jar.open(tf.path.c_str()));

    JarEntry found;
    ASSERT_TRUE(jar.findEntry("hello.txt", found));
    ASSERT_EQ(found.compression, (uint16_t)0);
    ASSERT_EQ(found.uncompressedSize, (uint32_t)helloContent.size());

    uint8_t buf[128] = {0};
    size_t n = jar.extractEntry("hello.txt", buf, sizeof(buf));
    ASSERT_EQ(n, helloContent.size());
    ASSERT_EQ(std::string(reinterpret_cast<char *>(buf), n), helloContent);

    // Recherche insensible a la casse (documentee dans jar_reader.cpp : ciEq).
    n = jar.extractEntry("HELLO.TXT", buf, sizeof(buf));
    ASSERT_EQ(n, helloContent.size());

    uint8_t buf2[128] = {0};
    n = jar.extractEntry("data.bin", buf2, sizeof(buf2));
    ASSERT_EQ(n, dataBinExpected.size());
    ASSERT_EQ(std::string(reinterpret_cast<char *>(buf2), n), dataBinExpected);
}

TEST(jar_reader_missing_entry_returns_zero)
{
    std::vector<ZipEntrySpec> entries;
    entries.push_back({"only.txt", 0, {'x'}, 1});
    TempFile tf(buildZip(entries));

    JarReader jar;
    ASSERT_TRUE(jar.open(tf.path.c_str()));

    uint8_t buf[16];
    ASSERT_EQ(jar.extractEntry("does_not_exist.txt", buf, sizeof(buf)), (size_t)0);

    JarEntry e;
    ASSERT_FALSE(jar.findEntry("does_not_exist.txt", e));
}

TEST(jar_reader_read_manifest_end_to_end)
{
    const std::string manifest =
        "MIDlet-Name: Demo\n"
        "MIDlet-Vendor: Acme\n"
        "MIDlet-Version: 2.1\n"
        "MIDlet-1: Demo, , com.acme.Demo\n";
    std::vector<ZipEntrySpec> entries;
    entries.push_back({"META-INF/MANIFEST.MF", 0,
                        std::vector<uint8_t>(manifest.begin(), manifest.end()),
                        static_cast<uint32_t>(manifest.size())});
    TempFile tf(buildZip(entries));

    JarReader jar;
    ASSERT_TRUE(jar.open(tf.path.c_str()));

    ManifestInfo info;
    ASSERT_TRUE(jar.readManifest(info));
    ASSERT_EQ(info.mainClass, std::string("com.acme.Demo"));
    ASSERT_EQ(info.midletVersion, std::string("2.1"));
}

TEST(jar_reader_open_nonexistent_file_fails)
{
    JarReader jar;
    ASSERT_FALSE(jar.open("/nonexistent/path/does_not_exist.jar"));
}
