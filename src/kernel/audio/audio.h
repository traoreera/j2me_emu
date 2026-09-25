#pragma once

// kernel/drivers/audio/audio.h
// Pilote audio du noyau. Contrat valable pour TOUTES les cibles :
//   - PC/SDL2 : sdl_audio.cpp (wavette + callback SDL)
//   - MCU     : stub_audio.cpp (modèle à adapter à un codec I2S / sortie PWM)
// Le noyau possède un mélangeur mono 16-bit à taux fixe (kSampleRate), et un
// ensemble FIXE de voix (pas d'allocation, pas de GC : requis MCU).
//
// Deux familles de voix :
//   * voix PCM  : joue un buffer int16 mono fourni par l'appelant (kasée).
//   * voix Tone : synthèse sinusoïdale (Manager.playTone) ou séquence
//                 d'événements (ToneControl) calculée à la volée -- aucun
//                 buffer, idéal MCU.
//
// Chaque voix expose boucle ("repeats": -1 = ∞) et volume [0..1].
// Le callback du device (ex. SDL) appelle kernel::audio::mix() pour remplir
// son buffer de sortie.

#include "kernel/kernel.h"

namespace kernel
{
namespace audio
{

constexpr uint32_t kSampleRate = 22050; // mono, int16, CCITT/MIDP courant
constexpr int kMaxVoices = 8;

// ---------------------------------------------------------------------------
// Événement d'une séquence tonale (parsing ToneControl côté VM).
// frames = durée en échantillons ; freq = 0 -> silence.
// ---------------------------------------------------------------------------
struct ToneEvt
{
    uint32_t frames;
    float freq;
    uint8_t vol; // 0..100
};

// ---------------------------------------------------------------------------
// Device : le trait d'union vers la cible matérielle/logicielle.
// ---------------------------------------------------------------------------
struct Device
{
    const char *name; // "sdl" | "stub"
    int (*open)(const Device *d);   // -> 0 OK (mais raté n'est pas fatal)
    void (*close)(const Device *d);
};

extern const Device kSdlDevice;  // PC (défini dans sdl_audio.cpp)
extern const Device kStubDevice; // MCU/headless (stub_audio.cpp)

void useDevice(const Device *d);
const Device *activeDevice();

// ---------------------------------------------------------------------------
// Verrouillage optionnel du tableau de voix.
// PC : la callback SDL (fil audio) lit g_voices pendant que le thread VM les
// modifie -> on verrouille les mutations/getters côté VM. mix() (côté
// callback) n'est JAMAIS verrouillé par le noyau : le backend le verrouille
// lui-même s'il en a besoin (PAS via SDL_LockAudioDevice, qui ne doit pas
// être appelé depuis la callback). MCU mono-thread : laisser à null.
// ---------------------------------------------------------------------------
void setAudioLock(void (*lockFn)(void), void (*unlockFn)(void));

// ---------------------------------------------------------------------------
// API voix
// ---------------------------------------------------------------------------
// Retourne un handle >= 0, ou -1 si table de voix pleine.
int playSample(const int16_t *pcm, uint32_t lenSamples, bool loop, float vol);
int playTone(float freqHz, uint32_t ms, float vol);                  // sin mono
int playToneSeq(const ToneEvt *seq, uint32_t count, float vol);      // séquence
void voiceSetLoop(int h, bool loop);       // loope à l'infini (repeats = -1)
void voiceSetRepeats(int h, int repeats);  // -1 = ∞, >= 0 = nb relances
void voiceSetVol(int h, float vol);
void voiceStop(int h);
bool voiceActive(int h);
uint32_t voicePos(int h); // échantillons consommés (0 si inconnu)
void stopAll();

// ---------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------
void setMasterVolume(float vol);
float masterVolume();
// Fin du mix global : garde-fou anti-saturation (voix multiples).
// mix() remplit 'frames' échantillons mono int16.
void mix(int16_t *out, uint32_t frames);
// Avance les voix sans produire de son (backend stub / CI headless) : conserve
// le timing FIFO (fins de piste, boucles) sans callback matériel.
void advanceSilent(uint32_t frames);

} // namespace audio
} // namespace kernel