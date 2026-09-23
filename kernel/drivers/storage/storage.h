#pragma once

// kernel/drivers/storage/storage.h
// Contrat stockage : accès séquentiel/aléatoire à de gros blocs (le JAR en
// ROM, les sauvegardes en flash/SD). Mêmes règles que hal/file_* : PAS de
// fichier complet en RAM, buffers fournis par l'appelant. Cible MCU : flash
// QSPI interne + SD en SPI ; le PC : stdio (hal/file.cpp).

#include "../../../kernel/kernel.h"
#include <cstddef>

namespace kernel
{
namespace storage
{

struct File
{
    uint32_t size;      // taille totale connue à l'ouverture
    uint32_t pos;       // position courante
    void *impl;         // handle opaque cible (FILE*, SD...)
};

struct Driver
{
    const char *backend; // "stdio" | "qspi" | "sdfs"
    // Ouvre une ressource par nom ; retourne != 0 si absente.
    int (*open)(File *f, const char *name);
    int (*seek)(File *f, uint32_t pos);        // absolu
    int (*read)(File *f, uint8_t *dst, uint32_t len);
    void (*close)(File *f);
};

} // namespace storage
} // namespace kernel