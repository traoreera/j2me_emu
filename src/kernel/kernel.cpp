// kernel/kernel.cpp
// Implémentation du noyau : registre des pilotes + horloge monotone.
// Portable MCU : pas d'allocation, seulement des tableaux statiques.

#include "kernel/kernel.h"

namespace kernel
{

namespace
{
    Driver g_drivers[kMaxDrivers];
    int g_count = 0;
    bool g_booted = false;
    MillisFn g_millis = nullptr;
} // namespace

void setMillisProvider(MillisFn fn)
{
    g_millis = fn;
}

Ms millis()
{
    return g_millis ? g_millis() : 0;
}

int driverRegister(Driver *d)
{
    if (!d || !d->name)
        return -1;
    if (g_count >= kMaxDrivers)
        return -1;
    g_drivers[g_count] = *d;
    return g_count++;
}

Driver *driverByName(const char *name)
{
    for (int i = 0; i < g_count; i++)
        if (strEq(g_drivers[i].name, name))
            return &g_drivers[i];
    return nullptr;
}

void forEachDriver(void (*cb)(Driver *d))
{
    if (!cb)
        return;
    for (int i = 0; i < g_count; i++)
        cb(&g_drivers[i]);
}

int kernelBoot(Ms now)
{
    int ok = 0;
    for (int i = 0; i < g_count; i++)
    {
        if (g_drivers[i].init && g_drivers[i].init(&g_drivers[i]) == 0)
            ok++;
    }
    g_booted = true;
    (void)now;
    return ok;
}

void kernelShutdown(Ms now)
{
    for (int i = 0; i < g_count; i++)
        if (g_drivers[i].shutdown)
            g_drivers[i].shutdown(&g_drivers[i]);
    g_booted = false;
    (void)now;
}

} // namespace kernel