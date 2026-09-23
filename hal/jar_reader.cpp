#include "jar_reader.h"
#include "inflate.h"

#include <cstring>
#include <cctype>
#include <algorithm>
#include <cstdio>

#ifdef JAR_READER_INDEX_IN_RAM
#include <vector>
#endif

namespace jme
{

    // Signatures ZIP standard.
    static constexpr uint32_t SIG_LOCAL_FILE_HEADER = 0x04034b50;
    static constexpr uint32_t SIG_CENTRAL_DIR = 0x02014b50;
    static constexpr uint32_t SIG_EOCD = 0x06054b50;

    // -- petits helpers de lecture little-endian sur HAL file --------------------

    static uint16_t readU16(hal::FileHandle *f)
    {
        uint8_t b[2];
        if (hal::file_read(f, b, 2) != 2)
            return 0;
        return static_cast<uint16_t>(b[0] | (b[1] << 8));
    }

    static uint32_t readU32(hal::FileHandle *f)
    {
        uint8_t b[4];
        if (hal::file_read(f, b, 4) != 4)
            return 0;
        return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
               (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    }

    static uint16_t readU16LE(const uint8_t *p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
    static uint32_t readU32LE(const uint8_t *p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    // ---------------------------------------------------------------------
    // JarReader
    // ---------------------------------------------------------------------

    JarReader::~JarReader() { close(); }

    void JarReader::close()
    {
        if (file_.opaque)
        {
            hal::file_close(&file_);
        }
#ifdef JAR_READER_INDEX_IN_RAM
        index_.clear();
#endif
    }

    bool JarReader::open(const char *path)
    {
        close();
        if (!hal::file_open(&file_, path))
            return false;

        if (!locateEndOfCentralDirectory())
        {
            close();
            return false;
        }

#ifdef JAR_READER_INDEX_IN_RAM
        // Construit un index en RAM (utile sur PC ; à éviter sur RP2040 si le
        // jar contient beaucoup d'entrées).
        index_.reserve(entryCount_);
        hal::file_seek(&file_, centralDirOffset_, SEEK_SET);
        for (uint16_t i = 0; i < entryCount_; i++)
        {
            long entryStart = hal::file_tell(&file_);
            uint32_t sig = readU32(&file_);
            if (sig != SIG_CENTRAL_DIR)
                break;

            JarEntry e;
            hal::file_seek(&file_, entryStart + 10, SEEK_SET);
            e.compression = readU16(&file_);
            hal::file_seek(&file_, entryStart + 20, SEEK_SET);
            e.compressedSize = readU32(&file_);
            e.uncompressedSize = readU32(&file_);
            uint16_t fnLen = readU16(&file_);
            uint16_t exLen = readU16(&file_);
            uint16_t cmLen = readU16(&file_);
            hal::file_seek(&file_, entryStart + 42, SEEK_SET);
            e.localHeaderOffset = readU32(&file_);

            std::string name(fnLen, '\0');
            hal::file_seek(&file_, entryStart + 46, SEEK_SET);
            hal::file_read(&file_, reinterpret_cast<uint8_t *>(name.data()), fnLen);
            e.name = std::move(name);

            index_.push_back(std::move(e));
            hal::file_seek(&file_, entryStart + 46 + fnLen + exLen + cmLen, SEEK_SET);
        }
#endif

        return true;
    }

    bool JarReader::locateEndOfCentralDirectory()
    {
        hal::file_seek(&file_, 0, SEEK_END);
        long fileSize = hal::file_tell(&file_);
        if (fileSize < 22)
            return false;

        auto parseEocd = [this](const uint8_t *hdr)
        {
            entryCount_ = readU16LE(hdr + 10);
            centralDirSize_ = readU32LE(hdr + 12);
            centralDirOffset_ = readU32LE(hdr + 16);
        };

        // Cas rapide : pas de commentaire ZIP (quasi toujours vrai pour un .jar).
        hal::file_seek(&file_, fileSize - 22, SEEK_SET);
        uint8_t hdr[22];
        if (hal::file_read(&file_, hdr, 22) == 22 && readU32LE(hdr) == SIG_EOCD)
        {
            parseEocd(hdr);
            return true;
        }

#ifndef JAR_READER_NO_COMMENT_SCAN
        // Cas lent : présence d'un commentaire ZIP -> on scanne en arrière.
        // Sur RP2040, définir JAR_READER_NO_COMMENT_SCAN pour désactiver ce
        // chemin (évite une allocation pouvant aller jusqu'à ~64 KB) : les
        // outils de packaging J2ME n'ajoutent quasiment jamais de commentaire.
        long maxBack = std::min<long>(fileSize, 65535 + 22);
        std::vector<uint8_t> tail(static_cast<size_t>(maxBack));
        hal::file_seek(&file_, fileSize - maxBack, SEEK_SET);
        if (hal::file_read(&file_, tail.data(), maxBack) != static_cast<size_t>(maxBack))
            return false;

        for (long i = maxBack - 22; i >= 0; i--)
        {
            if (readU32LE(&tail[static_cast<size_t>(i)]) == SIG_EOCD)
            {
                parseEocd(&tail[static_cast<size_t>(i)]);
                return true;
            }
        }
#endif
        return false;
    }

    bool JarReader::findEntry(const std::string &entryPath, JarEntry &out)
    {
        auto ciEq = [](const std::string &a, const std::string &b) {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); i++)
            {
                char x = a[i], y = b[i];
                if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
                if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
                if (x != y)
                    return false;
            }
            return true;
        };
#ifdef JAR_READER_INDEX_IN_RAM
        for (const auto &e : index_)
        {
            if (e.name == entryPath || ciEq(e.name, entryPath))
            {
                out = e;
                out.name = entryPath;
                return true;
            }
        }
        return false;
#else
        hal::file_seek(&file_, centralDirOffset_, SEEK_SET);
        char nameBuf[260];

        for (uint16_t i = 0; i < entryCount_; i++)
        {
            long entryStart = hal::file_tell(&file_);
            uint32_t sig = readU32(&file_);
            if (sig != SIG_CENTRAL_DIR)
                return false;

            hal::file_seek(&file_, entryStart + 10, SEEK_SET);
            uint16_t compression = readU16(&file_);
            hal::file_seek(&file_, entryStart + 20, SEEK_SET);
            uint32_t compressedSize = readU32(&file_);
            uint32_t uncompressedSize = readU32(&file_);
            uint16_t fnLen = readU16(&file_);
            uint16_t exLen = readU16(&file_);
            uint16_t cmLen = readU16(&file_);
            hal::file_seek(&file_, entryStart + 42, SEEK_SET);
            uint32_t localHeaderOffset = readU32(&file_);

            hal::file_seek(&file_, entryStart + 46, SEEK_SET);
            size_t toRead = std::min<size_t>(fnLen, sizeof(nameBuf) - 1);
            hal::file_read(&file_, reinterpret_cast<uint8_t *>(nameBuf), toRead);
            nameBuf[toRead] = '\0';

            if (ciEq(entryPath, std::string(nameBuf, toRead)))
            {
                out.name = entryPath;
                out.compression = compression;
                out.compressedSize = compressedSize;
                out.uncompressedSize = uncompressedSize;
                out.localHeaderOffset = localHeaderOffset;
                return true;
            }

            // saut vers l'entrée suivante du central directory
            hal::file_seek(&file_, entryStart + 46 + fnLen + exLen + cmLen, SEEK_SET);
        }
        return false;
#endif
    }

    size_t JarReader::decompressEntry(const JarEntry &e, uint8_t *outBuffer, size_t maxLen)
    {
        hal::file_seek(&file_, e.localHeaderOffset, SEEK_SET);
        if (readU32(&file_) != SIG_LOCAL_FILE_HEADER)
            return 0;

        hal::file_seek(&file_, e.localHeaderOffset + 26, SEEK_SET);
        uint16_t fnLen = readU16(&file_);
        uint16_t exLen = readU16(&file_);
        uint32_t dataOffset = e.localHeaderOffset + 30 + fnLen + exLen;
        hal::file_seek(&file_, dataOffset, SEEK_SET);

        if (e.compression == 0)
        { // stored
            if (e.uncompressedSize > maxLen)
                return 0;
            return hal::file_read(&file_, outBuffer, e.uncompressedSize);
        }

        if (e.compression == 8)
        { // deflate
            hal::FileHandle *f = &file_;
            uint32_t remaining = e.compressedSize;
            BitReader br([f, remaining](uint8_t *buf, size_t maxL) mutable -> size_t
                         {
            size_t toRead = std::min<size_t>(remaining, maxL);
            if (toRead == 0) return 0;
            size_t got = hal::file_read(f, buf, toRead);
            remaining -= static_cast<uint32_t>(got);
            return got; });
            return inflateStream(br, outBuffer, maxLen);
        }

        return 0; // méthode de compression non supportée
    }

    size_t JarReader::extractEntry(const std::string &entryPath, uint8_t *outBuffer, size_t maxLen)
    {
        JarEntry e;
        if (!findEntry(entryPath, e))
            return 0;
        return decompressEntry(e, outBuffer, maxLen);
    }

    size_t JarReader::extractClass(const std::string &className, uint8_t *outBuffer, size_t maxLen)
    {
        std::string path = className;
        std::replace(path.begin(), path.end(), '.', '/');
        if (path.size() < 6 || path.compare(path.size() - 6, 6, ".class") != 0)
        {
            path += ".class";
        }
        return extractEntry(path, outBuffer, maxLen);
    }

    bool JarReader::readManifest(ManifestInfo &out)
    {
        static uint8_t scratch[4096]; // buffer statique réutilisable (pas d'alloc)
        size_t n = extractEntry("META-INF/MANIFEST.MF", scratch, sizeof(scratch));
        if (n == 0)
            return false;
        return parseManifestText(reinterpret_cast<const char *>(scratch), n, out);
    }

    // ---------------------------------------------------------------------
    // Parsing du manifeste (format texte "Clé: Valeur" par ligne).
    // Ne gère pas le "line folding" (continuation par une ligne commençant
    // par un espace) : suffisant pour les champs MIDlet-* qui tiennent
    // quasi toujours sur une seule ligne.
    // ---------------------------------------------------------------------

    static std::string trim(const std::string &s)
    {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    bool parseManifestText(const char *text, size_t len, ManifestInfo &out)
    {
        std::string content(text, len);
        size_t pos = 0;

        while (pos < content.size())
        {
            size_t eol = content.find('\n', pos);
            if (eol == std::string::npos)
                eol = content.size();
            std::string line = content.substr(pos, eol - pos);
            pos = eol + 1;

            size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;

            std::string key = trim(line.substr(0, colon));
            std::string value = trim(line.substr(colon + 1));

            if (key == "MIDlet-1")
            {
                // Format: "Nom, chemin-icone(optionnel), Classe.Principale"
                size_t lastComma = value.find_last_of(',');
                if (lastComma != std::string::npos)
                {
                    out.mainClass = trim(value.substr(lastComma + 1));
                }
                else
                {
                    out.mainClass = value; // tolérance si format simplifié
                }
            }
            else if (key == "MIDlet-Name")
            {
                out.midletName = value;
            }
            else if (key == "MIDlet-Vendor")
            {
                out.midletVendor = value;
            }
            else if (key == "MIDlet-Version")
            {
                out.midletVersion = value;
            }
        }

        out.valid = !out.mainClass.empty();
        return out.valid;
    }

} // namespace jme