// kernel/drivers/audio/sdl_audio.cpp
// Backend audio PC (SDL2). Ce fichier est EXCLUSIVEMENT PC : sa cible de
// portage MCU est stub_audio.cpp (sans SDL). Conditionné à la présence d'un
// SDL2 dans l'inclusion, garde le build monocible `/dev/null`.

#if defined(__has_include)
#if __has_include(<SDL2/SDL.h>)
#define KERNEL_AUDIO_HAS_SDL 1
#endif
#endif

#ifdef KERNEL_AUDIO_HAS_SDL

#include "kernel/audio/audio.h"

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>

namespace kernel
{
namespace audio
{

namespace
{
    SDL_AudioDeviceID g_dev = 0;
    SDL_AudioSpec g_spec{};
    SDL_SpinLock g_lock = 0;
    FILE *g_capture = nullptr; // JME_AUDIO_CAPTURE=fichier.raw : PCM s16 mono 22050 Hz (diagnostic)

    void sdlLockFn() { SDL_AtomicLock(&g_lock); }
    void sdlUnlockFn() { SDL_AtomicUnlock(&g_lock); }

    void sdlCallback(void *userdata, Uint8 *stream, int len)
    {
        (void)userdata;
        int32_t frames = len / (int32_t)sizeof(int16_t);
        // La callback est le SEUL producteur de son : verrou interne du noyau
        // pour ne pas lire une table en cours de mutation par un autre thread.
        sdlLockFn();
        mix(reinterpret_cast<int16_t *>(stream), (uint32_t)frames);
        sdlUnlockFn();
        if (g_capture)
            fwrite(stream, 1, (size_t)len, g_capture);
    }
} // namespace

int sdlOpen(const Device *d)
{
    (void)d;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        fprintf(stderr, "[audio] SDL audio indisponible: %s\n", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec want{};
    want.freq = (int)kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = sdlCallback;
    want.userdata = nullptr;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &g_spec, 0);
    if (g_dev == 0)
    {
        fprintf(stderr, "[audio] Ouvreur SDL échouée (headless/CI ?): %s\n", SDL_GetError());
        return -1;
    }
    fprintf(stderr, "[audio] device SDL (%s): %d Hz, %d canaux, %d samples\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", g_spec.freq, g_spec.channels, g_spec.samples);
    if (const char *cap = getenv("JME_AUDIO_CAPTURE"))
        g_capture = fopen(cap, "wb");
    setAudioLock(sdlLockFn, sdlUnlockFn);
    SDL_PauseAudioDevice(g_dev, 0);
    return 0;
}

void sdlClose(const Device *d)
{
    (void)d;
    if (g_dev)
    {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    if (g_capture)
    {
        fclose(g_capture);
        g_capture = nullptr;
    }
}

const Device kSdlDevice = {"sdl", sdlOpen, sdlClose};

} // namespace audio
} // namespace kernel

#endif // KERNEL_AUDIO_HAS_SDL