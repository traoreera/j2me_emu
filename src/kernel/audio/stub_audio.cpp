// kernel/drivers/audio/stub_audio.cpp
// Backend audio de référence pour cible MCU / sans sortie audio (CI headless).
// Le device "stub" échantillonne le mélangeur à période fixe (timer) au lieu
// d'un callback matériel. Sur RP2040 ce point devient : DMA/I2S -> mix().
//
// TODO MCU : remplacer kernel::millis() polling par un timer matériel, et
// mixer la sortie vers un codec I2S (PCM5102/MAX98357) ou un canal PWM.

#include "kernel/audio/audio.h"

namespace kernel
{
namespace audio
{

namespace
{
    Ms g_last = 0;
} // namespace

int stubOpen(const Device *d)
{
    (void)d;
    g_last = kernel::millis();
    return 0;
}

void stubClose(const Device *d)
{
    (void)d;
}

const Device kStubDevice = {"stub", stubOpen, stubClose};

} // namespace audio
} // namespace kernel