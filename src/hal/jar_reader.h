// jar_reader.h
// Lecteur JAR/ZIP minimal pour un émulateur J2ME.
//
// Contraintes de conception (pensées RP2040) :
//  - Aucune lecture intégrale du fichier .jar en RAM : tout se fait par
//    seek/read ciblés via HAL file abstraction (pas de FILE*).
//  - Aucun index complet des entrées gardé en mémoire par défaut : la
//    recherche d'une entrée (classe, manifeste...) se fait par scan
//    séquentiel du "central directory" directement depuis le disque.
//    Compilez avec JAR_READER_INDEX_IN_RAM pour activer un cache RAM
//    (utile sur PC pour accélérer les lectures répétées).
//  - Toutes les fonctions d'extraction écrivent dans un buffer fourni par
//    l'appelant (jamais d'allocation interne) : c'est à l'appelant de
//    dimensionner son buffer (ex : classe .class rarement > 8-16 KB en
//    J2ME MIDP).

#pragma once

#include <cstdint>
#include <string>
#include "hal/file.h"

#ifdef JAR_READER_INDEX_IN_RAM
#include <vector>
#endif

namespace jme
{

    struct JarEntry
    {
        std::string name;         // ex: "com/foo/Game.class"
        uint16_t compression = 0; // 0 = stored, 8 = deflate
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint32_t localHeaderOffset = 0;
    };

    struct ManifestInfo
    {
        std::string mainClass; // forme avec points, ex: "com.foo.Game"
        std::string midletName;
        std::string midletVendor;
        std::string midletVersion;
        bool valid = false;
    };

    class JarReader
    {
    public:
        JarReader() = default;
        ~JarReader();

        JarReader(const JarReader &) = delete;
        JarReader &operator=(const JarReader &) = delete;

        // Ouvre le .jar et localise le central directory (EOCD).
        bool open(const char *path);
        void close();
        bool isOpen() const { return file_.opaque != nullptr; }

        // Lit META-INF/MANIFEST.MF et en extrait les champs MIDlet-1 / Name /
        // Vendor / Version.
        bool readManifest(ManifestInfo &out);

        // Extrait une classe par son nom qualifié (points ou slashs acceptés,
        // le suffixe ".class" est ajouté automatiquement s'il est absent).
        // Retourne la taille décompressée écrite dans outBuffer, ou 0 en échec.
        size_t extractClass(const std::string &className, uint8_t *outBuffer, size_t maxLen);

        // Extraction générique par chemin exact dans l'archive.
        size_t extractEntry(const std::string &entryPath, uint8_t *outBuffer, size_t maxLen);

        // Recherche une entrée sans l'extraire (utile pour connaître sa taille
        // avant d'allouer / réserver le buffer de destination).
        bool findEntry(const std::string &entryPath, JarEntry &out);

#ifdef JAR_READER_INDEX_IN_RAM
        const std::vector<JarEntry> &entries() const { return index_; }
#endif

    private:
        hal::FileHandle file_{};
        uint32_t centralDirOffset_ = 0;
        uint32_t centralDirSize_ = 0;
        uint16_t entryCount_ = 0;

#ifdef JAR_READER_INDEX_IN_RAM
        std::vector<JarEntry> index_;
#endif

        bool locateEndOfCentralDirectory();
        size_t decompressEntry(const JarEntry &e, uint8_t *outBuffer, size_t maxLen);
    };

    // Parseur du texte brut d'un MANIFEST.MF (exposé séparément pour pouvoir
    // être testé unitairement sans dépendre du ZIP).
    bool parseManifestText(const char *text, size_t len, ManifestInfo &out);

} // namespace jme

// ---------------------------------------------------------------------
// Note de portage RP2040 :
// Le header utilise hal::FileHandle (opaque). L'implémentation
// hal/file.cpp côté PC utilise FILE* ; côté RP2040, remplacer par
// vos primitives hal_file_* sur flash externe / carte SD.
// ---------------------------------------------------------------------