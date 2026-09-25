// midp_media.cpp -- MMAPI : Player/Manager/VolumeControl, moteur audio (WAV/MIDI/ToneSeq)
// Découpé de l'ancien midp_natives.cpp ; état partagé : midp_internal.h.

#include "midp/midp_internal.h"

namespace jvm
{
    namespace midp
    {
        namespace detail
        {

            static void n_sound_play(NativeContext *ctx)
            {
                // Nokia Sound.play(int) - no-op minimal implementation
                (void)ctx;
            }

            static void n_sound_init_intlong(NativeContext *ctx)
            {
                // Nokia Sound.<init>(int, long) - no-op
                (void)ctx;
            }

            static void n_sound_init_bytesint(NativeContext *ctx)
            {
                // Nokia Sound.<init>(byte[], int) - no-op
                (void)ctx;
            }
            // Dans votre fichier de liaisons natives J2ME (ex: vm/midp.cpp)
            static jvm::Value native_Player_getControl(jvm::Runtime *rt, jvm::Interpreter *interp, jvm::Value *args, jvm::Value &res)
            {
                jvm::ClassInfo *volCtrlCls = rt->loadFromJar("javax/microedition/media/control/VolumeControl");
                if (!volCtrlCls)
                {
                    // Si la classe n'existe pas en bytecode, créer un objet générique Object/Stub
                    volCtrlCls = rt->loadFromJar("java/lang/Object");
                }

                jvm::Obj *controlObj = rt->heap().newInstance(volCtrlCls);
                return jvm::Value::fromRef(controlObj);
            }
            static jvm::Value native_VolumeControl_setLevel(jvm::Runtime *rt, jvm::Value *args, int argCount)
            {
                int level = args[1].i;
                // Enregistrer le volume si nécessaire...
                return jvm::Value::fromInt(level); // Retourne le niveau appliqué
            }

            // --- javax.microedition.media : file d'événements Player ---
            struct MediaEvt
            {
                Obj *listener;
                Obj *player;
                const char *what;
                int frame;
            };
            static std::vector<MediaEvt> &mediaQueueRef()
            {
                static std::vector<MediaEvt> q;
                return q;
            }
            static int &mediaTimerBase()
            {
                static int f = 0;
                return f;
            }
            void midpMediaPollEnded();
            void mediaFlush()
            {
                if (!g_interp)
                    return;
                midpMediaPollEnded();
                int &base = mediaTimerBase();
                base++;
                std::vector<MediaEvt> cur;
                cur.swap(mediaQueueRef());
                for (MediaEvt &e : cur)
                {
                    if (e.frame > base)
                    {
                        mediaQueueRef().push_back(e);
                        continue;
                    }
                    if (!e.listener || !e.player)
                        continue;
                    Obj *whatStr = g_rt->heap().newString(e.what);
                    if (jvm::jmeDebug())
                        fprintf(stderr, "MEDIA %s -> %s\n", e.what,
                                e.listener->cls ? e.listener->cls->name.c_str() : "?");
                    Value args[4];
                    args[0] = Value::fromRef(e.listener);
                    args[1] = Value::fromRef(e.player);
                    args[2] = Value::fromRef(whatStr);
                    args[3] = Value::fromRef(nullptr);
                    Value res;
                    g_interp->invokeVirtual(e.listener->cls, "playerUpdate",
                                            "(Ljavax/microedition/media/Player;Ljava/lang/String;Ljava/lang/Object;)V",
                                            e.listener, args, 4, res);
                }
            }
            static void mediaQueue(Obj *listener, Obj *player, const char *what, int delayFrames)
            {
                if (!listener || !player || !g_interp)
                    return;
                MediaEvt e;
                e.listener = listener;
                e.player = player;
                e.what = what;
                e.frame = mediaTimerBase() + delayFrames;
                mediaQueueRef().push_back(e);
            }
            struct SoundPlayer
            {
                Obj *player = nullptr;
                bool loaded = false;
                int kind = 0;                    // 0 rien, 1 pcm, 2 seq
                std::vector<uint8_t> raw;        // données brutes compactes
                std::vector<int16_t> pcm;        // PCM mono 22050 Hz
                std::vector<audio::ToneEvt> seq; // séquence tonale
                int voice = -1;                  // handle kernel (ou -1)
                int loop = 0;                    // MIDP setLoopCount
                bool started = false;
                bool eofQueued = false;
            };
            static SoundPlayer g_snd[kNumSound];

            static SoundPlayer *sndSlotFor(Obj *p)
            {
                if (!p || !p->cells || p->cellCount < 4)
                    return nullptr;
                int idx = p->cells[3].i;
                if (idx < 0 || idx >= kNumSound)
                    return nullptr;
                SoundPlayer &s = g_snd[idx];
                if (s.player != p)
                    return nullptr;
                return &s;
            }
            static int sndAllocSlot(Obj *p)
            {
                for (int i = 0; i < kNumSound; i++)
                    if (!g_snd[i].player)
                    {
                        g_snd[i] = SoundPlayer{};
                        g_snd[i].player = p;
                        g_snd[i].voice = -1;
                        return i;
                    }
                return -1;
            }
            static void sndFreeSlot(int idx)
            {
                if (idx >= 0 && idx < kNumSound)
                {
                    if (g_snd[idx].voice >= 0)
                        audio::voiceStop(g_snd[idx].voice);
                    g_snd[idx] = SoundPlayer{};
                }
            }
            // Ramasse tout l'InputStream (cells = data/pos/lim ; streamFill lit).
            static std::vector<uint8_t> sndDrainStream(Obj *stream, int cap)
            {
                std::vector<uint8_t> out;
                Obj *tmp = g_rt->heap().newArray(ObjKind::ByteArray, 2048);
                while (true)
                {
                    if (cap > 0 && (int)out.size() > cap)
                        break;
                    int n = streamFill(stream, tmp, 0, 2048);
                    if (n <= 0)
                        break;
                    for (int i = 0; i < n; i++)
                        out.push_back((uint8_t)tmp->cells[i].u);
                }
                return out;
            }
            static void sndResample(std::vector<int16_t> &pcm, uint32_t srcRate)
            {
                if (srcRate == 0 || srcRate == audio::kSampleRate || pcm.size() < 2)
                    return;
                uint64_t ratio = ((uint64_t)srcRate << 16) / audio::kSampleRate;
                size_t outLen = (size_t)(((uint64_t)pcm.size() << 16) / ratio) + 1;
                std::vector<int16_t> o;
                o.reserve(outLen);
                uint32_t phase = 0;
                for (size_t k = 0; k < outLen; k++)
                {
                    uint32_t idx = phase >> 16;
                    if (idx >= pcm.size())
                        idx = (uint32_t)pcm.size() - 1;
                    o.push_back(pcm[idx]);
                    phase += (uint32_t)ratio;
                }
                pcm.swap(o);
            }
            // RIFF/WAV → PCM mono 16-bit. Retourne false si ce n'est pas du PCM jouable.
            static bool sndParseWav(const std::vector<uint8_t> &raw, std::vector<int16_t> &pcm)
            {
                size_t len = raw.size();
                uint32_t dataOff = 0, dataLen = 0, rate = 0;
                uint16_t ch = 1, bits = 16, fmt = 0;
                bool riff = len >= 12 && raw[0] == 'R' && raw[1] == 'I' && raw[2] == 'F' &&
                            raw[3] == 'F' && raw[8] == 'W' && raw[9] == 'A' && raw[10] == 'V' &&
                            raw[11] == 'E';
                if (riff)
                {
                    size_t off = 12;
                    while (off + 8 <= len)
                    {
                        uint32_t cid = (uint32_t)raw[off] | ((uint32_t)raw[off + 1] << 8) |
                                       ((uint32_t)raw[off + 2] << 16) | ((uint32_t)raw[off + 3] << 24);
                        uint32_t sz = (uint32_t)raw[off + 4] | ((uint32_t)raw[off + 5] << 8) |
                                      ((uint32_t)raw[off + 6] << 16) | ((uint32_t)raw[off + 7] << 24);
                        if (cid == 0x20746D66 && sz >= 16) // 'fmt '
                        {
                            fmt = (uint16_t)(raw[off + 8] | (raw[off + 9] << 8));
                            ch = (uint16_t)(raw[off + 10] | (raw[off + 11] << 8));
                            rate = (uint32_t)raw[off + 12] | ((uint32_t)raw[off + 13] << 8) |
                                   ((uint32_t)raw[off + 14] << 16) | ((uint32_t)raw[off + 15] << 24);
                            // blockAlign à off+20, bits par échantillon à off+22
                            bits = (uint16_t)(raw[off + 22] | (raw[off + 23] << 8));
                        }
                        else if (cid == 0x61746164) // 'data'
                        {
                            dataOff = (uint32_t)off + 8;
                            dataLen = sz;
                        }
                        off += 8 + sz + (sz & 1);
                    }
                    if (fmt != 1 || ch == 0 || (bits != 8 && bits != 16) || dataLen == 0)
                        return false;
                }
                else
                {
                    // Non-RIFF : GAMELOFT livre parfois du PCM16 brut sous type "wav".
                    if (len % 2 != 0 || len < 16)
                        return false;
                    dataOff = 0;
                    dataLen = (uint32_t)len;
                    fmt = 1;
                    ch = 1;
                    bits = 16;
                    rate = audio::kSampleRate;
                }
                size_t avail = dataOff + dataLen <= len ? dataLen : len - dataOff;
                size_t i = dataOff;
                size_t end = dataOff + avail;
                pcm.clear();
                if (bits == 16)
                {
                    if (ch == 1)
                    {
                        while (i + 1 < end)
                        {
                            int16_t s = (int16_t)((uint16_t)raw[i] | ((uint16_t)raw[i + 1] << 8));
                            pcm.push_back(s);
                            i += 2;
                        }
                    }
                    else
                    {
                        while (i + 2 * ch <= end)
                        {
                            int32_t acc = 0;
                            for (uint16_t c = 0; c < ch; c++)
                                acc += (int16_t)((uint16_t)raw[i + c * 2] | ((uint16_t)raw[i + c * 2 + 1] << 8));
                            pcm.push_back((int16_t)(acc / (int)ch));
                            i += 2 * ch;
                        }
                    }
                }
                else
                {
                    if (ch == 1)
                    {
                        while (i < end)
                        {
                            pcm.push_back((int16_t)(((int32_t)raw[i] - 128) << 8));
                            i++;
                        }
                    }
                    else
                    {
                        while (i + ch <= end)
                        {
                            int32_t acc = 0;
                            for (uint16_t c = 0; c < ch; c++)
                                acc += (int32_t)raw[i + c] - 128;
                            pcm.push_back((int16_t)((acc / (int)ch) << 8));
                            i += ch;
                        }
                    }
                }
                sndResample(pcm, rate);
                return !pcm.empty();
            }
            // MIDI (audio/midi) : parser SMF minimal — MThd/MTrk, VLQ, running
            // status, meta tempo + end-of-track, synthèse polyphonique sinusoïdale
            // vers PCM mono 22050 Hz (le kernel ne sait jouer que du PCM).
            namespace
            {
                struct MidiEvt
                {
                    uint32_t tick;
                    uint8_t type; // 0 = tempo, 1 = noteOn, 2 = noteOff
                    uint8_t note;
                    uint8_t vel;
                    uint32_t tempo; // µs par noire (pour type 0)
                };
                struct MidiSeg
                {
                    int32_t start;
                    int32_t end;
                    double freq;
                    float vol;
                };
                uint32_t midiVLQ(const std::vector<uint8_t> &raw, size_t &i)
                {
                    uint32_t v = 0;
                    while (i < raw.size())
                    {
                        uint8_t b = raw[i++];
                        v = (v << 7) | (b & 0x7F);
                        if (!(b & 0x80))
                            break;
                    }
                    return v;
                }
                double midiNoteFreq(uint8_t note)
                {
                    return 440.0 * pow(2.0, ((double)note - 69.0) / 12.0);
                }
            } // namespace
            static bool sndParseMidi(const std::vector<uint8_t> &raw, std::vector<int16_t> &pcm)
            {
                if (raw.size() < 14 || raw[0] != 'M' || raw[1] != 'T' || raw[2] != 'h' || raw[3] != 'd')
                    return false;
                auto rd16 = [&](size_t i, int d = 0) -> int
                {
                    return ((int)raw[i] << 8 | (int)raw[i + 1]) + d;
                };
                auto rd32 = [&](size_t i) -> uint32_t
                {
                    return ((uint32_t)raw[i] << 24) | ((uint32_t)raw[i + 1] << 16) |
                           ((uint32_t)raw[i + 2] << 8) | (uint32_t)raw[i + 3];
                };
                int div = rd16(12);
                bool smpte = (div & 0x8000) != 0;
                int ppqn = smpte ? 1 : (div & 0x7FFF);
                if (ppqn < 1)
                    ppqn = 1;
                std::vector<MidiEvt> evs;
                // Parcours des chunks MTrk indépendamment.
                size_t i = 14;
                while (i + 8 <= raw.size())
                {
                    char id0 = (char)raw[i], id1 = (char)raw[i + 1];
                    char id2 = (char)raw[i + 2], id3 = (char)raw[i + 3];
                    size_t len = rd32(i + 4);
                    i += 8;
                    if (i + len > raw.size())
                        len = raw.size() - i;
                    if (id0 == 'M' && id1 == 'T' && id2 == 'r' && id3 == 'k')
                    {
                        size_t end = i + len;
                        size_t p = i;
                        uint8_t running = 0;
                        uint32_t absTick = 0;
                        while (p < end)
                        {
                            uint32_t delta = midiVLQ(raw, p);
                            absTick += delta;
                            if (p >= end)
                                break;
                            uint8_t b = raw[p];
                            if (b == 0xFF)
                            {
                                p++;
                                uint8_t type = p < end ? raw[p++] : 0;
                                size_t elen = midiVLQ(raw, p);
                                size_t estart = p;
                                if (type == 0x51 && elen >= 3 && estart + 3 <= end)
                                {
                                    uint32_t tempo = ((uint32_t)raw[estart] << 16) | ((uint32_t)raw[estart + 1] << 8) | raw[estart + 2];
                                    if (tempo < 1000)
                                        tempo = 1000;
                                    MidiEvt e;
                                    e.tick = absTick;
                                    e.type = 0;
                                    e.tempo = tempo;
                                    evs.push_back(e);
                                }
                                running = 0;
                                p = estart + elen;
                                continue;
                            }
                            if (b == 0xF0 || b == 0xF7)
                            {
                                p++;
                                size_t slen = midiVLQ(raw, p);
                                p += slen;
                                running = 0;
                                continue;
                            }
                            uint8_t status = b;
                            bool hasData1 = false;
                            uint8_t d1 = 0, d2 = 0;
                            if (status >= 0x80)
                            {
                                p++;
                                running = (status >= 0x80 && status < 0xF0) ? status : 0;
                            }
                            else
                            {
                                status = running;
                                d1 = b;
                                hasData1 = true;
                                if (status == 0)
                                    break; // running status invalide
                            }
                            uint8_t hi = status & 0xF0;
                            uint8_t ch = status & 0x0F;
                            if (hi == 0xC0 || hi == 0xD0)
                            {
                                if (!hasData1 && p < end)
                                {
                                    d1 = raw[p++];
                                    hasData1 = true;
                                }
                            }
                            else
                            {
                                if (!hasData1 && p < end)
                                    d1 = raw[p++];
                                if (p < end)
                                    d2 = raw[p++];
                            }
                            if (hi == 0x90 && d2 > 0)
                            {
                                MidiEvt e;
                                e.tick = absTick;
                                e.type = 1;
                                e.note = d1;
                                e.vel = d2;
                                evs.push_back(e);
                            }
                            else if (hi == 0x80 || (hi == 0x90 && d2 == 0))
                            {
                                MidiEvt e;
                                e.tick = absTick;
                                e.type = 2;
                                e.note = d1;
                                e.vel = 0;
                                evs.push_back(e);
                            }
                        }
                    }
                    i += len;
                }
                if (evs.empty())
                    return false;
                std::sort(evs.begin(), evs.end(),
                          [](const MidiEvt &a, const MidiEvt &b)
                          { return a.tick < b.tick; });
                // Temps en samples : breaks de tempo (µs/noire) → sps (samples/tick).
                struct Break
                {
                    uint32_t tick;
                    double sps;
                };
                std::vector<Break> breaks;
                breaks.push_back({0u, (500000.0 / 1e6) * (double)audio::kSampleRate / (double)ppqn});
                uint32_t lastNoteTick = 0;
                for (const MidiEvt &e : evs)
                {
                    if (e.type == 0)
                    {
                        double sps = (e.tempo / 1e6) * (double)audio::kSampleRate / (double)ppqn;
                        breaks.push_back({e.tick, sps});
                    }
                    else
                        lastNoteTick = std::max(lastNoteTick, e.tick);
                }
                std::sort(breaks.begin(), breaks.end(),
                          [](const Break &a, const Break &b)
                          { return a.tick < b.tick; });
                auto tickToSamples = [&](uint32_t tick) -> int32_t
                {
                    double sps = breaks[0].sps;
                    uint32_t prev = 0;
                    double acc = 0.0;
                    for (const Break &b : breaks)
                    {
                        if (b.tick > tick)
                            break;
                        acc += (double)(b.tick - prev) * sps;
                        sps = b.sps;
                        prev = b.tick;
                    }
                    acc += (double)(tick - prev) * sps;
                    return (int32_t)llround(acc);
                };
                const int32_t kMaxSamples = (int32_t)(30.0 * (double)audio::kSampleRate);
                std::vector<MidiSeg> segs;
                int32_t lastSamples = 0;
                int32_t activeStart[128];
                uint8_t activeVel[128];
                for (int k = 0; k < 128; k++)
                    activeStart[k] = -1;
                for (const MidiEvt &e : evs)
                {
                    if (e.type != 1)
                        continue;
                    int32_t nowS = tickToSamples(e.tick);
                    if (nowS > kMaxSamples)
                        break;
                    lastSamples = nowS;
                    if (e.vel == 0 || activeStart[e.note] >= 0)
                    {
                        if (activeStart[e.note] >= 0)
                        {
                            MidiSeg s;
                            s.start = activeStart[e.note];
                            s.end = nowS;
                            s.freq = midiNoteFreq(e.note);
                            s.vol = (float)activeVel[e.note] / 127.0f;
                            if (s.end > s.start)
                                segs.push_back(s);
                            activeStart[e.note] = -1;
                        }
                        continue;
                    }
                    activeStart[e.note] = nowS;
                    activeVel[e.note] = e.vel;
                }
                for (const MidiEvt &e : evs)
                {
                    if (e.type != 2)
                        continue;
                    int32_t nowS = tickToSamples(e.tick);
                    if (nowS > kMaxSamples)
                        break;
                    lastSamples = std::max(lastSamples, nowS);
                    if (activeStart[e.note] >= 0)
                    {
                        MidiSeg s;
                        s.start = activeStart[e.note];
                        s.end = nowS;
                        s.freq = midiNoteFreq(e.note);
                        s.vol = (float)activeVel[e.note] / 127.0f;
                        if (s.end > s.start)
                            segs.push_back(s);
                        activeStart[e.note] = -1;
                    }
                }
                for (int k = 0; k < 128; k++)
                {
                    if (activeStart[k] >= 0)
                    {
                        MidiSeg s;
                        s.start = activeStart[k];
                        s.end = std::min(lastSamples + (int32_t)(audio::kSampleRate / 16), kMaxSamples);
                        s.freq = midiNoteFreq((uint8_t)k);
                        s.vol = (float)activeVel[k] / 127.0f;
                        if (s.end > s.start)
                            segs.push_back(s);
                    }
                }
                if (segs.empty())
                    return false;
                int32_t total = (int32_t)tickToSamples(lastNoteTick) + (int32_t)(audio::kSampleRate / 8);
                if (total < 1)
                    total = 1;
                if (total > kMaxSamples)
                    total = kMaxSamples;
                std::vector<int32_t> buf((size_t)total, 0);
                const double twoPi = 6.283185307179586;
                for (const MidiSeg &sg : segs)
                {
                    if (sg.start >= total)
                        continue;
                    if (sg.vol <= 0.0f || sg.freq <= 0.0)
                        continue;
                    float vol = sg.vol > 0.8f ? 0.8f : (sg.vol < 0.05f ? 0.05f : sg.vol);
                    int32_t e0 = sg.end > total ? total : sg.end;
                    int32_t fade = (int32_t)(audio::kSampleRate / 500); // 2 ms anti-clic
                    for (int32_t s = sg.start; s < e0; s++)
                    {
                        float env = 1.0f;
                        int32_t segLen = e0 - sg.start;
                        if (fade < segLen)
                        {
                            if (s - sg.start < fade)
                                env = (float)(s - sg.start) / (float)fade;
                            else if (e0 - s < fade)
                                env = (float)(e0 - s) / (float)fade;
                        }
                        else
                            env = (float)(s - sg.start) / (float)segLen;
                        double amp = (double)(vol * env * 32767.0);
                        int32_t v = (int32_t)(amp * sin(twoPi * sg.freq * (double)s / (double)audio::kSampleRate));
                        buf[(size_t)s] += v;
                    }
                }
                pcm.clear();
                pcm.reserve(buf.size());
                for (int32_t v : buf)
                {
                    if (v > 32767)
                        v = 32767;
                    if (v < -32768)
                        v = -32768;
                    pcm.push_back((int16_t)v);
                }
                return (int32_t)pcm.size() > (int32_t)audio::kSampleRate / 8;
            }
            // Séquence ToneControl (bytes compacts, PAS un WAV). Constantes JSR-135 :
            //   VERSION(1) en tête, puis jetons :
            //     PLAY_TONE(-2) note dur vol | SILENCE(-1) dur | SET_VOLUME(-6) vol
            //     SEQUENTIAL(-3) / PARALLEL(-4) : bornes de groupe
            //     REPEAT(-5) count : répète le groupe d'événements suivant count fois.
            // Durées en unités de 1/64 s, volume 0..100. Parser récursif (REPEAT
            // imbriqués, profondeur bornée) qui linéarise tout en évents séquentiels.
            namespace
            {
                bool toneParseGroup(const std::vector<uint8_t> &raw, size_t &i, size_t end,
                                    std::vector<audio::ToneEvt> &out, int &curVol, int depth)
                {
                    const uint32_t unit = audio::kSampleRate / 64u;
                    audio::ToneEvt ev;
                    auto pushSilence = [&](uint32_t frames)
                    {
                        if (!frames)
                            return;
                        audio::ToneEvt s;
                        s.frames = frames;
                        s.freq = 0.0f;
                        s.vol = (uint8_t)curVol;
                        out.push_back(s);
                    };
                    auto pushTone = [&](int8_t note, uint32_t frames, int8_t vol)
                    {
                        if (!frames)
                            return;
                        ev.frames = frames;
                        ev.freq = (note > 0) ? 440.0f * powf(2.0f, (note - 69) / 12.0f) : 0.0f;
                        ev.vol = (uint8_t)(vol > 0 ? vol : curVol);
                        if (ev.freq > 0.0f || ev.vol > 0)
                            out.push_back(ev);
                    };
                    while (i < end)
                    {
                        int8_t t = (int8_t)raw[i];
                        if (t <= 0)
                        {
                            if (t == 0)
                                return true; // terminaison explicite
                            if (t == -2)     // PLAY_TONE note dur vol
                            {
                                if (i + 3 >= end)
                                    return true;
                                int8_t note = (int8_t)raw[i + 1];
                                int8_t dur = (int8_t)raw[i + 2];
                                int8_t vol = (int8_t)raw[i + 3];
                                if (dur < 1)
                                    dur = 1;
                                pushTone(note, (uint32_t)dur * unit, vol);
                                i += 4;
                                continue;
                            }
                            if (t == -1) // SILENCE dur
                            {
                                int8_t dur = (i + 1 < end) ? (int8_t)raw[i + 1] : 1;
                                pushSilence((dur > 0) ? (uint32_t)dur * unit : unit);
                                i += (i + 1 < end) ? 2 : 1;
                                continue;
                            }
                            if (t == -6) // SET_VOLUME v
                            {
                                if (i + 1 < end)
                                {
                                    uint8_t v = raw[i + 1];
                                    if (v > 100)
                                        v = 100;
                                    curVol = v;
                                }
                                i += 2;
                                continue;
                            }
                            if (t == -5) // REPEAT count
                            {
                                if (i + 1 >= end)
                                    return true;
                                int repeatCount = (int8_t)raw[i + 1];
                                if (repeatCount < 0)
                                    repeatCount = 1;
                                if (repeatCount > 32)
                                    repeatCount = 32;
                                i += 2;
                                // Groupe suivant : jusqu'au prochain jeton de borne
                                // (SEQUENTIAL/PARALLEL/REPEAT) ou fin de séquence.
                                std::vector<audio::ToneEvt> group;
                                if (depth < 4)
                                    toneParseGroup(raw, i, end, group, curVol, depth + 1);
                                for (int c = 0; c < repeatCount; c++)
                                    out.insert(out.end(), group.begin(), group.end());
                                continue;
                            }
                            // -3 SEQUENTIAL / -4 PARALLEL / inconnu : borne de groupe.
                            i++;
                            return true;
                        }
                        // Note nue : NOTE DURATION [VOLUME si 1..100 et suivie d'un octet].
                        {
                            int8_t note = (int8_t)raw[i];
                            if (i + 1 >= end)
                                return true;
                            int8_t dur = (int8_t)raw[i + 1];
                            int8_t vol = 0;
                            size_t consumed = 2;
                            if (i + 2 < end)
                            {
                                int8_t v = (int8_t)raw[i + 2];
                                if (v > 0 && v <= 100 && i + 3 < end)
                                {
                                    vol = v;
                                    consumed = 3;
                                }
                            }
                            if (dur < 1)
                                dur = 1;
                            pushTone(note, (uint32_t)dur * unit, vol);
                            i += consumed;
                        }
                    }
                    return true;
                }
            } // namespace
            static bool sndParseToneSeq(const std::vector<uint8_t> &raw, std::vector<audio::ToneEvt> &seq)
            {
                if (raw.size() < 2)
                    return false;
                if ((int8_t)raw[1] == 0)
                    return false;
                seq.clear();
                size_t i = 1; // byte[0] = VERSION
                int curVol = 100;
                toneParseGroup(raw, i, raw.size(), seq, curVol, 0);
                return !seq.empty();
            }
            // Appelée à chaque frame : une voix terminée → endOfMedia (une fois) + PREFETCHED.
            void midpMediaPollEnded()
            {
                const audio::Device *ad = audio::activeDevice();
                if (ad && ad->name && strcmp(ad->name, "stub") == 0)
                {
                    static kernel::Ms lastPoll = 0;
                    kernel::Ms now = kernel::millis();
                    uint32_t dt = (uint32_t)(now - lastPoll);
                    lastPoll = now;
                    if (dt > 500)
                        dt = 500; // premier appel / long spatule : évite une boucle
                    if (dt)
                        audio::advanceSilent((uint32_t)((uint64_t)dt * audio::kSampleRate / 1000u));
                }
                for (int i = 0; i < kNumSound; i++)
                {
                    SoundPlayer &s = g_snd[i];
                    if (s.player && s.started && !s.eofQueued && s.voice >= 0 &&
                        !audio::voiceActive(s.voice))
                    {
                        s.eofQueued = true;
                        s.started = false;
                        if (s.player->cells)
                            s.player->cells[0].i = 300; // PREFETCHED
                        Obj *ln = s.player->cells ? s.player->cells[1].o : nullptr;
                        mediaQueue(ln, s.player, "endOfMedia", 0);
                    }
                }
            }

            static int mediaStateOf(Obj *p) { return (p && p->cells) ? p->cells[0].i : 0; }

            static void mp_createPlayerIS(NativeContext *ctx)
            {
                Obj *stream = argRef(ctx, 0);
                Obj *type = argRef(ctx, 1);
                Obj *p = makeInstance("javax/microedition/media/Player");
                if (!p || !p->cells)
                {
                    setRef(ctx, nullptr);
                    return;
                }
                p->cells[0].i = 100; // UNREALIZED
                p->cells[1] = Value::fromRef(nullptr);
                p->cells[2] = Value::fromRef(type); // contentType
                int idx = sndAllocSlot(p);
                p->cells[3] = Value::fromInt(idx);
                if (stream && idx >= 0)
                {
                    SoundPlayer &s = g_snd[idx];
                    s.raw = sndDrainStream(stream, 4 * 1024 * 1024);
                }
                if (jvm::jmeDebug())
                    fprintf(stderr, "[midp] Manager.createPlayer(InputStream, %s) -> %p (slot %d, %zu octets, hdr %02x %02x %02x %02x %02x %02x %02x %02x)\n",
                            (type && type->kind == ObjKind::String) ? type->str.c_str() : "", (void *)p, idx,
                            idx >= 0 ? g_snd[idx].raw.size() : 0,
                            idx >= 0 && g_snd[idx].raw.size() > 0 ? g_snd[idx].raw[0] : 0,
                            idx >= 0 && g_snd[idx].raw.size() > 1 ? g_snd[idx].raw[1] : 0,
                            idx >= 0 && g_snd[idx].raw.size() > 2 ? g_snd[idx].raw[2] : 0,
                            idx >= 0 && g_snd[idx].raw.size() > 3 ? g_snd[idx].raw[3] : 0,
                            idx >= 0 && g_snd[idx].raw.size() > 7 ? g_snd[idx].raw[7] : 0,
                            idx >= 0 && g_snd[idx].raw.size() > 11 ? g_snd[idx].raw[11] : 0,
                            idx >= 0 && g_snd[idx].raw.size() > 12 ? g_snd[idx].raw[12] : 0,
                            idx >= 0 && g_snd[idx].raw.size() > 20 ? g_snd[idx].raw[20] : 0,
                            idx >= 0 && g_snd[idx].raw.size() > 22 ? g_snd[idx].raw[22] : 0);
                setRef(ctx, p);
                if (const char *wd = getenv("JME_WAVDUMP"))
                {
                    if (idx >= 0 && !g_snd[idx].raw.empty())
                    {
                        char fn[96];
                        snprintf(fn, sizeof fn, "%s_%d.wav", wd, idx);
                        FILE *fd = fopen(fn, "wb");
                        if (fd)
                        {
                            fwrite(g_snd[idx].raw.data(), 1, g_snd[idx].raw.size(), fd);
                            fclose(fd);
                        }
                    }
                }
                if (jvm::jmeDebug() && stream && idx >= 0)
                {
                    // Sonde de décodage : confirme le RIFF sans dépendre d'un start().
                    std::vector<int16_t> probe;
                    const char *pt = (type && type->kind == ObjKind::String) ? type->str.c_str() : "";
                    bool ok = strstr(pt, "wav")    ? sndParseWav(g_snd[idx].raw, probe)
                              : strstr(pt, "midi") ? sndParseMidi(g_snd[idx].raw, probe)
                                                   : false;
                    if (ok)
                        fprintf(stderr, "[midp] %s decode OK: %zu echantillons PCM mono\n",
                                strstr(pt, "midi") ? "midi" : "wav", probe.size());
                    else if (strstr(pt, "wav"))
                    {
                        const auto &r = g_snd[idx].raw;
                        fprintf(stderr, "[midp] wav decode FAIL (len=%zu): %c%c%c%c %c%c%c%c\n",
                                r.size(), r.size() > 0 ? r[0] : ' ', r.size() > 1 ? r[1] : ' ',
                                r.size() > 2 ? r[2] : ' ', r.size() > 3 ? r[3] : ' ',
                                r.size() > 8 ? r[8] : ' ', r.size() > 9 ? r[9] : ' ',
                                r.size() > 10 ? r[10] : ' ', r.size() > 11 ? r[11] : ' ');
                    }
                }
            }

            static void mp_createPlayerStr(NativeContext *ctx)
            {
                Obj *p = makeInstance("javax/microedition/media/Player");
                if (!p || !p->cells)
                {
                    setRef(ctx, nullptr);
                    return;
                }
                p->cells[0].i = 100;
                p->cells[1] = Value::fromRef(nullptr);
                p->cells[2] = Value::fromRef(argRef(ctx, 0));
                p->cells[3] = Value::fromInt(-1);
                if (jvm::jmeDebug())
                    fprintf(stderr, "[midp] Manager.createPlayer(locator=%s) -> %p (sans audio)\n",
                            (argRef(ctx, 0) && argRef(ctx, 0)->kind == ObjKind::String) ? argRef(ctx, 0)->str.c_str() : "",
                            (void *)p);
                setRef(ctx, p);
            }
            static void mp_realize(NativeContext *ctx)
            {
                Obj *p = ctx->thisObj;
                if (p && p->cells && mediaStateOf(p) == 100)
                    p->cells[0].i = 200; // REALIZED
            }
            static void mp_prefetch(NativeContext *ctx)
            {
                Obj *p = ctx->thisObj;
                if (p && p->cells && mediaStateOf(p) == 200)
                    p->cells[0].i = 300; // PREFETCHED
            }
            static void mp_start(NativeContext *ctx)
            {
                Obj *p = ctx->thisObj;
                if (!p || !p->cells)
                    return;
                Obj *ln = p->cells[1].o;
                SoundPlayer *s = sndSlotFor(p);
                if (s)
                {
                    if (!s->loaded)
                    {
                        const char *ct = (p->cells[2].o && p->cells[2].o->kind == ObjKind::String)
                                             ? p->cells[2].o->str.c_str()
                                             : "";
                        if (strstr(ct, "wav") || strstr(ct, "WAV"))
                        {
                            if (sndParseWav(s->raw, s->pcm))
                                s->kind = 1;
                        }
                        else if (strstr(ct, "tone"))
                        {
                            if (sndParseToneSeq(s->raw, s->seq))
                                s->kind = 2;
                        }
                        else if (strstr(ct, "midi"))
                        {
                            if (sndParseMidi(s->raw, s->pcm))
                                s->kind = 1;
                        }
                        s->loaded = true;
                    }
                    if (s->voice >= 0 && !audio::voiceActive(s->voice))
                        s->voice = -1; // relance après fin naturelle
                    if (s->voice < 0)
                    {
                        if (s->kind == 1 && s->pcm.size() >= 2)
                            s->voice = audio::playSample(s->pcm.data(), (int)s->pcm.size(), false, 0.6f);
                        else if (s->kind == 2)
                            s->voice = audio::playToneSeq(s->seq.data(), (int)s->seq.size(), 0.6f);
                        if (s->voice >= 0)
                        {
                            int rep = s->loop < 0 ? -1 : (s->loop > 0 ? s->loop - 1 : 0);
                            audio::voiceSetRepeats(s->voice, rep);
                            if (jvm::jmeDebug())
                                fprintf(stderr, "[midp] Player.start -> voice %d (kind %d, loop %d)\n",
                                        s->voice, s->kind, s->loop);
                        }
                    }
                    s->started = true;
                    s->eofQueued = false;
                    p->cells[0].i = 400; // STARTED
                }
                else
                {
                    p->cells[0].i = 400;
                }
                mediaQueue(ln, p, "started", 0);
            }
            static void mp_stop(NativeContext *ctx)
            {
                Obj *p = ctx->thisObj;
                if (!p || !p->cells)
                    return;
                SoundPlayer *s = sndSlotFor(p);
                if (s)
                {
                    if (s->voice >= 0)
                        audio::voiceStop(s->voice);
                    s->voice = -1;
                    s->started = false;
                    s->eofQueued = false;
                    p->cells[0].i = 300;
                }
                mediaQueue(p->cells[1].o, p, "stopped", 0);
            }
            static void mp_deallocate(NativeContext *ctx)
            {
                Obj *p = ctx->thisObj;
                if (!p || !p->cells || mediaStateOf(p) == 0)
                    return;
                SoundPlayer *s = sndSlotFor(p);
                if (s)
                {
                    if (s->voice >= 0)
                        audio::voiceStop(s->voice);
                    s->voice = -1;
                    s->started = false;
                    s->eofQueued = false;
                }
                p->cells[0].i = 200;
            }
            static void mp_close(NativeContext *ctx)
            {
                Obj *p = ctx->thisObj;
                if (p && p->cells)
                {
                    SoundPlayer *s = sndSlotFor(p);
                    if (s)
                        sndFreeSlot((int)p->cells[3].i);
                    p->cells[3] = Value::fromInt(-1);
                    p->cells[0].i = 0; // CLOSED
                }
            }
            static void mp_noop(NativeContext *ctx) { (void)ctx; }
            static void mp_addListener(NativeContext *ctx)
            {
                if (ctx->thisObj && ctx->thisObj->cells)
                    ctx->thisObj->cells[1] = Value::fromRef(argRef(ctx, 1));
            }
            static void mp_setLoopCount(NativeContext *ctx)
            {
                Obj *p = ctx->thisObj;
                SoundPlayer *s = sndSlotFor(p);
                int v = argInt(ctx, 1);
                if (s)
                    s->loop = v;
                if (jvm::jmeDebug())
                    fprintf(stderr, "[midp] Player.setLoopCount(%d)\n", v);
            }
            static void mp_getInt(NativeContext *ctx)
            {
                Obj *p = ctx->thisObj;
                setInt(ctx, (p && p->cells) ? p->cells[0].i : 400);
            }
            static void mp_getControl(NativeContext *ctx)
            {
                Obj *ctl = argRef(ctx, 1);
                const char *which = (ctl && ctl->kind == ObjKind::String) ? ctl->str.c_str() : "";
                if (jvm::jmeDebug())
                    fprintf(stderr, "[midp] Player.getControl(%s)\n", which);
                if (strstr(which, "Volume") || strstr(which, "volume"))
                {
                    Obj *vc = makeInstance("javax/microedition/media/control/VolumeControl");
                    setRef(ctx, vc);
                    return;
                }
                setRef(ctx, nullptr);
            }
            static void mp_getLong(NativeContext *ctx)
            {
                Obj *p = ctx->thisObj;
                SoundPlayer *s = sndSlotFor(p);
                if (s && s->kind == 1)
                    setLong(ctx, (int64_t)(s->pcm.size()) * 1000 / audio::kSampleRate);
                else if (s && s->kind == 2)
                {
                    int64_t total = 0;
                    for (auto &e : s->seq)
                        total += e.frames;
                    setLong(ctx, total * 1000 / audio::kSampleRate);
                }
                else
                    setLong(ctx, 1000);
            }
            static void mp_getMediaTime(NativeContext *ctx)
            {
                Obj *p = ctx->thisObj;
                SoundPlayer *s = sndSlotFor(p);
                if (s && s->voice >= 0 && audio::voiceActive(s->voice))
                    setLong(ctx, (int64_t)audio::voicePos(s->voice) * 1000 / audio::kSampleRate);
                else
                    setLong(ctx, 0);
            }
            static void mp_setMediaTime(NativeContext *ctx)
            {
                // Pas de seek implémenté : on rend le temps demandé (incompressible).
                setLong(ctx, argLongL(ctx, 1));
            }
            static void mp_getContentType(NativeContext *ctx)
            {
                Obj *p = ctx->thisObj;
                setRef(ctx, (p && p->cells && p->cells[2].o) ? p->cells[2].o : g_rt->heap().newString(""));
            }

            // --- VolumeControl : map au volume maître du kernel audio ---
            static void vc_getLevel(NativeContext *ctx)
            {
                setInt(ctx, (int)lroundf(audio::masterVolume() * 100.0f));
            }
            static void vc_setLevel(NativeContext *ctx)
            {
                int lv = argInt(ctx, 1);
                if (lv < 0)
                    lv = 0;
                if (lv > 100)
                    lv = 100;
                audio::setMasterVolume(lv / 100.0f);
                if (jvm::jmeDebug())
                    fprintf(stderr, "[midp] VolumeControl.setLevel(%d)\n", lv);
                setInt(ctx, lv); // MIDP : retourne le niveau effectivement réglé
            }
            static void vc_getMute(NativeContext *ctx) { setInt(ctx, audio::masterVolume() <= 0.0f ? 1 : 0); }
            static void vc_setMute(NativeContext *ctx)
            {
                int m = argInt(ctx, 1);
                audio::setMasterVolume(m ? 0.0f : 1.0f);
            }
            static void mp_playTone(NativeContext *ctx)
            {
                int note = argInt(ctx, 0);
                int durMs = argInt(ctx, 1);
                int vol = argInt(ctx, 2);
                float freq = (note > 0) ? 440.0f * powf(2.0f, (note - 69) / 12.0f) : 0.0f;
                float amp = (vol > 0 ? (float)vol : 100.0f) / 100.0f * 0.8f;
                audio::playTone(freq, durMs, amp);
                if (jvm::jmeDebug())
                    fprintf(stderr, "[midp] Manager.playTone(note=%d, dur=%dms, vol=%d)\n", note, durMs, vol);
            }
            void registerMediaNatives()
            {
                regN("com/nokia/mid/sound/Sound.play:(I)V", n_sound_play);
                regN("com/nokia/mid/sound/Sound.<init>:(IJ)V", n_sound_init_intlong);
                regN("com/nokia/mid/sound/Sound.<init>:([BI)V", n_sound_init_bytesint);
                regN("javax/microedition/media/Manager.createPlayer:(Ljava/io/InputStream;Ljava/lang/String;)Ljavax/microedition/media/Player;", mp_createPlayerIS);
                regN("javax/microedition/media/Manager.createPlayer:(Ljava/lang/String;)Ljavax/microedition/media/Player;", mp_createPlayerStr);
                regN("javax/microedition/media/Manager.playTone:(III)V", mp_playTone);
                regN("javax/microedition/media/Player.realize:()V", mp_realize);
                regN("javax/microedition/media/Player.prefetch:()V", mp_prefetch);
                regN("javax/microedition/media/Player.start:()V", mp_start);
                regN("javax/microedition/media/Player.stop:()V", mp_stop);
                regN("javax/microedition/media/Player.deallocate:()V", mp_deallocate);
                regN("javax/microedition/media/Player.close:()V", mp_close);
                regN("javax/microedition/media/Player.setLoopCount:(I)V", mp_setLoopCount);
                regN("javax/microedition/media/Player.getState:()I", mp_getInt);
                regN("javax/microedition/media/Player.getDuration:()J", mp_getLong);
                regN("javax/microedition/media/Player.getMediaTime:()J", mp_getMediaTime);
                regN("javax/microedition/media/Player.setMediaTime:(J)J", mp_setMediaTime);
                regN("javax/microedition/media/Player.getContentType:()Ljava/lang/String;", mp_getContentType);
                regN("javax/microedition/media/Player.getControl:(Ljava/lang/String;)Ljavax/microedition/media/Control;", mp_getControl);
                regN("javax/microedition/media/Player.addPlayerListener:(Ljavax/microedition/media/PlayerListener;)V", mp_addListener);
                regN("javax/microedition/media/Player.removePlayerListener:(Ljavax/microedition/media/PlayerListener;)V", mp_noop);
                regN("javax/microedition/media/control/VolumeControl.getLevel:()I", vc_getLevel);
                regN("javax/microedition/media/control/VolumeControl.setLevel:(I)I", vc_setLevel);
                regN("javax/microedition/media/control/VolumeControl.getMute:()Z", vc_getMute);
                regN("javax/microedition/media/control/VolumeControl.setMute:(Z)V", vc_setMute);
                regN("javax/microedition/media/control/VolumeControl.getAbsoluteLevel:()I", vc_getLevel);
            }
        } // namespace detail
    } // namespace midp
} // namespace jvm
