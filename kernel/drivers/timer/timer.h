#pragma once

// kernel/drivers/timer/timer.h
// Contrat temporisation : permet de demander un callback au tick suivant.
// Cible MCU : timer période (ex. alimente le scan d'entrées et le mix audio
// stub).

#include "../../../kernel/kernel.h"

namespace kernel
{
namespace timer
{

struct Driver
{
    const char *backend; // "sdl" | "systick"
    // Planifie un callback à appeler dans 'delayMs' (au mieux). Renvoie un
    // handle >= 0 pour annuler, -1 si indisponible.
    int (*schedule)(void (*cb)(void *), void *arg, uint32_t delayMs);
    void (*cancel)(int handle);
};

} // namespace timer
} // namespace kernel