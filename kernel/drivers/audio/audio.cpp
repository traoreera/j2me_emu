// kernel/drivers/audio/audio.cpp
// Mélangeur mono 16-bit du noyau. Tables statiques, pas d'allocation.
// Génère voix PCM + voix tonales (sinus / séquence ToneControl) à la volée.
// Le tout est thread-neutre du point de vue du noyau : l'appelant (device)
// est seul à exécuter mix() ; les mutations de table se font côté VM.

#include "kernel/drivers/audio/audio.h"
#include <cmath>

namespace kernel
{
namespace audio
{

namespace
{
    constexpr float kPi2 = 6.28318530718f;

    enum VoiceKind : uint8_t
    {
        kNone = 0,
        kPcm,
        kTone,
        kToneSeq,
    };

    struct Voice
    {
        bool active;
        uint8_t kind;
        int8_t _pad;
        float vol;   // volume applicatif de la voix [0..1]
        int repeats; // -1 = boucle infinie ; >= 0 : nb de relances restantes

        // kPcm
        const int16_t *pcm;
        uint32_t len;  // échantillons
        uint32_t pos;  // position de lecture
        bool pcmLoop;  // loop court-circuit (utilisé pour boucle glissante)

        // kTone (sinus single) + état ToneSeq
        float phase;       // phase courante [0..2pi[
        float phaseInc;    // incr/échantillon (0 si silence)
        float amp;         // amplitude appliquée (déjà * vol)
        uint32_t remain;   // échantillons restants note courante
        uint32_t toneTotal;// durée totale d'un tone single (relance)
        float noteFreq;    // fréquence note courante (0 = silence)

        // kToneSeq
        const ToneEvt *seq;
        uint32_t seqLen;
        uint32_t seqIdx;
    };

    Voice g_voices[kMaxVoices];
    float g_master = 1.0f;
    const Device *g_device = &kStubDevice;

    // Verrou optionnel posé par le backend (SDL_AtomicLock sur PC, null sur MCU).
    void (*g_lockFn)(void) = nullptr;
    void (*g_unlockFn)(void) = nullptr;
    struct VoiceGuard
    {
        VoiceGuard() { if (g_lockFn) g_lockFn(); }
        ~VoiceGuard() { if (g_unlockFn) g_unlockFn(); }
    };

    float freqInc(float f)
    {
        return kPi2 * (f / (float)kSampleRate);
    }

    void toneReset(Voice &v, float freq)
    {
        v.noteFreq = freq;
        v.phaseInc = (freq > 1.0f) ? freqInc(freq) : 0.0f;
        v.remain = 0;
    }

    void advance(Voice &v)
    {
        if (v.kind == kPcm)
        {
            if (++v.pos >= v.len)
            {
                if (v.pcmLoop || v.repeats < 0)
                    v.pos = 0;
                else if (v.repeats > 0)
                {
                    v.repeats--;
                    v.pos = 0;
                }
                else
                    v.active = false;
            }
            return;
        }
        if (v.kind == kTone)
        {
            if (v.remain > 0)
                v.remain--;
            if (v.remain == 0)
            {
                if (v.repeats < 0)
                    v.remain = v.toneTotal;
                else if (v.repeats > 0)
                {
                    v.repeats--;
                    v.remain = v.toneTotal;
                    v.phase = 0.0f;
                }
                else
                    v.active = false;
            }
            return;
        }
        if (v.kind == kToneSeq)
        {
            // séquence : une note à la fois (déjà pré-analysée par la VM)
            if (v.remain > 0)
            {
                v.remain--;
                return;
            }
            // consommer silences/notes jusqu'à la fin
            while (v.seqIdx < v.seqLen)
            {
                const ToneEvt &e = v.seq[v.seqIdx];
                if (e.frames > 0)
                {
                    toneReset(v, e.freq);
                    v.phaseInc = (e.freq > 1.0f) ? freqInc(e.freq) : 0.0f;
                    v.amp = (e.vol > 0) ? (v.vol * (e.vol / 100.0f)) : 0.0f;
                    v.remain = e.frames;
                    v.seqIdx++;
                    return;
                }
                v.seqIdx++; // évènement 0-frame -> ignore
            }
            // fin de séquence -> relance ou arrêt
            if (v.repeats > 0)
            {
                v.repeats--;
                v.seqIdx = 0;
            }
            else if (v.repeats < 0)
            {
                v.seqIdx = 0; // boucle infinie
            }
            else
                v.active = false;
        }
    }
} // namespace

void useDevice(const Device *d)
{
    if (d)
        g_device = d;
}

const Device *activeDevice()
{
    return g_device;
}

void setAudioLock(void (*lockFn)(void), void (*unlockFn)(void))
{
    g_lockFn = lockFn;
    g_unlockFn = unlockFn;
}

int playSample(const int16_t *pcm, uint32_t lenSamples, bool loop, float vol)
{
    if (!pcm || lenSamples == 0)
        return -1;
    VoiceGuard guard;
    for (int i = 0; i < kMaxVoices; i++)
    {
        Voice &v = g_voices[i];
        if (!v.active)
        {
            v.active = true;
            v.kind = kPcm;
            v.vol = vol;
            v.repeats = loop ? -1 : 0;
            v.pcm = pcm;
            v.len = lenSamples;
            v.pos = 0;
            v.pcmLoop = loop;
            return i;
        }
    }
    return -1;
}

int playTone(float freqHz, uint32_t ms, float vol)
{
    VoiceGuard guard;
    for (int i = 0; i < kMaxVoices; i++)
    {
        Voice &v = g_voices[i];
        if (!v.active)
        {
            v.active = true;
            v.kind = kTone;
            v.vol = vol;
            v.repeats = 0;
            toneReset(v, freqHz);
            v.amp = vol;
            v.toneTotal = (uint32_t)((uint64_t)ms * kSampleRate / 1000u);
            v.remain = v.toneTotal;
            return i;
        }
    }
    return -1;
}

int playToneSeq(const ToneEvt *seq, uint32_t count, float vol)
{
    if (!seq || count == 0)
        return -1;
    VoiceGuard guard;
    for (int i = 0; i < kMaxVoices; i++)
    {
        Voice &v = g_voices[i];
        if (!v.active)
        {
            v.active = true;
            v.kind = kToneSeq;
            v.vol = vol;
            v.repeats = 0;
            v.seq = seq;
            v.seqLen = count;
            v.seqIdx = 0;
            v.remain = 0;
            return i;
        }
    }
    return -1;
}

void voiceSetLoop(int h, bool loop)
{
    VoiceGuard guard;
    if (h >= 0 && h < kMaxVoices)
        g_voices[h].repeats = loop ? -1 : g_voices[h].repeats;
}

void voiceSetRepeats(int h, int repeats)
{
    VoiceGuard guard;
    if (h >= 0 && h < kMaxVoices)
        g_voices[h].repeats = repeats;
}

void voiceSetVol(int h, float vol)
{
    VoiceGuard guard;
    if (h >= 0 && h < kMaxVoices)
        g_voices[h].vol = vol > 1.0f ? 1.0f : (vol < 0.0f ? 0.0f : vol);
}

void voiceStop(int h)
{
    VoiceGuard guard;
    if (h >= 0 && h < kMaxVoices)
        g_voices[h].active = false;
}

bool voiceActive(int h)
{
    VoiceGuard guard;
    return (h >= 0 && h < kMaxVoices) && g_voices[h].active;
}

uint32_t voicePos(int h)
{
    VoiceGuard guard;
    if (h < 0 || h >= kMaxVoices)
        return 0;
    const Voice &v = g_voices[h];
    return v.kind == kPcm ? v.pos : v.remain;
}

void stopAll()
{
    VoiceGuard guard;
    for (int i = 0; i < kMaxVoices; i++)
        g_voices[i].active = false;
}

void setMasterVolume(float vol)
{
    VoiceGuard guard;
    g_master = vol > 1.0f ? 1.0f : (vol < 0.0f ? 0.0f : vol);
}

float masterVolume()
{
    VoiceGuard guard;
    return g_master;
}

void mix(int16_t *out, uint32_t frames)
{
    if (!out || frames == 0)
        return;
    const float master = g_master;
    for (uint32_t f = 0; f < frames; f++)
    {
        int32_t acc = 0;
        for (int i = 0; i < kMaxVoices; i++)
        {
            Voice &v = g_voices[i];
            if (!v.active)
                continue;
            float s = 0.0f;
            if (v.kind == kPcm)
            {
                s = (float)v.pcm[v.pos] * v.vol;
            }
            else
            {
                // toner : silence pour note nulle/amp nulle ; amp est une
                // fraction [0..1] de l'échelle complète int16.
                if (v.phaseInc > 0.0f && v.amp > 0.0f)
                    s = (float)std::sin(v.phase) * v.amp * 32767.0f;
                v.phase += v.phaseInc;
                if (v.phase > kPi2)
                    v.phase -= kPi2;
            }
            acc += (int32_t)(s * master);
            advance(v);
        }
        if (acc > 32767)
            acc = 32767;
        else if (acc < -32768)
            acc = -32768;
        out[f] = (int16_t)acc;
    }
}

void advanceSilent(uint32_t frames)
{
    VoiceGuard guard;
    int16_t dummy[256];
    while (frames)
    {
        uint32_t n = frames > 256 ? 256 : frames;
        mix(dummy, n);
        frames -= n;
    }
}

} // namespace audio
} // namespace kernel