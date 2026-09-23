#pragma once

// kernel/kernel.h
// Cœur léger de l'émulateur, pensé pour être porté tel quel sur un MCU
// (ex. RP2040) : aucune dépendance libc/SDL/STL ici -- uniquement des
// entiers et des tables fixes. Les pilotes (voir kernel/drivers/*) sont des
// bundles dédiés à chaque cible ; le noyau ne fait que les initier, les
// arrêter et fournir l'horloge monotone utilisée par le reste de la VM.

#include <cstdint>
#include <cstddef>

namespace kernel
{

#ifdef KERNEL_USE_STL
#include <string>
inline bool strEq(const std::string &a, const char *b) { return a == b; }
#else
inline bool strEq(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *b && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}
#endif

// ---------------------------------------------------------------------------
// Horloge monosourcée : la plateforme hôte fournit un tick montant. PC :
// SDL_GetTicks(). MCU : compteur temps réel / timer.
// ---------------------------------------------------------------------------
using Ms = uint32_t;
using MillisFn = Ms (*)(void);

void setMillisProvider(MillisFn fn);
Ms millis();

// ---------------------------------------------------------------------------
// Pilote générique : un bundle de fonctions associé à une cible. Le noyau
// garde une table fixe (pas d'allocation) ; chaque pilote peut avoir son
// propre ctx opaque C.
// ---------------------------------------------------------------------------
constexpr int kMaxDrivers = 8;

struct Driver
{
    const char *name;    // "audio" | "display" | "input" | ...
    const char *backend; // sous-implantation (ex. "sdl"), pour diagnostic
    void *ctx;           // état privé du pilote (0 si inutile)
    int (*init)(Driver *d);     // -> 0 OK
    void (*shutdown)(Driver *d);
    void (*tick)(Driver *d, Ms now); // appelé à chaque frame; peut être 0
};

int driverRegister(Driver *d); // -> indice >= 0, -1 si table pleine
Driver *driverByName(const char *name);
void forEachDriver(void (*cb)(Driver *d));

// Boot/arrêt : initialise chaque pilote enregistré (ordre de déclaration).
// kernelBoot() -> nombre de pilotes initialisés (peut être partiel : un
// pilote qui échoue est simplement ignoré, cf. retour de son init).
int kernelBoot(Ms now);
void kernelShutdown(Ms now);

} // namespace kernel