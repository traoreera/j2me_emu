// midp_io.cpp -- java.io (flux d'octets, Data*Stream), ressources JAR, java.util.Timer
// Découpé de l'ancien midp_natives.cpp ; état partagé : midp_internal.h.

#include "midp_internal.h"

namespace jvm
{
    namespace midp
    {
        namespace detail
        {


            // ---------------------------------------------------------------------
            // I/O natives : flux octets (array-backed), getResourceAsStream, PNG.
            // Layout d'un InputStream (cells entiers) : 0 = données [B, 1 = pos,
            // 2 = limite. ByteArrayInputStream partage ce layout ; DataInputStream
            // garde son flux sous-jacent en cells[0].
            // ---------------------------------------------------------------------

            static Obj *makeStream(Obj *bytes, int limit)
            {
                Obj *s = makeInstance("java/io/InputStream");
                if (!s)
                    return nullptr;
                s->cells[0] = Value::fromRef(bytes);
                s->cells[1] = Value::fromInt(0);
                s->cells[2] = Value::fromInt(limit);
                return s;
            }

            static int32_t streamByte(Obj *s)
            {
                if (!s || s->cellCount < 3)
                    return -1;
                Obj *data = s->cells[0].o;
                int pos = s->cells[1].i, lim = s->cells[2].i;
                if (!data || pos >= lim)
                    return -1;
                s->cells[1] = Value::fromInt(pos + 1);
                return static_cast<int32_t>(data->cells[pos].u & 0xFF);
            }

            int32_t streamFill(Obj *s, Obj *dst, int off, int len)
            {
                if (!s || !dst || dst->kind != ObjKind::ByteArray)
                    return -1;
                if (off < 0 || len < 0 || off + len > dst->arrayLen)
                    return -1;
                Obj *data = s->cells[0].o;
                int pos = s->cells[1].i, lim = s->cells[2].i;
                int n = lim - pos;
                if (n > len)
                    n = len;
                if (n < 0)
                    n = 0;
                if (data)
                    for (int i = 0; i < n; i++)
                        dst->cells[off + i].u = data->cells[pos + i].u;
                s->cells[1] = Value::fromInt(pos + n);
                return n;
            }

static void native_TimerTask_init(NativeContext *ctx)
{
    (void)ctx;
}

            static void is_read(NativeContext *ctx) { setInt(ctx, streamByte(ctx->thisObj)); }
            static void is_readArr(NativeContext *ctx)
            {
                Obj *d = argRef(ctx, 1);
                setInt(ctx, streamFill(ctx->thisObj, d, 0, d ? d->arrayLen : 0));
            }
            static void is_readArrII(NativeContext *ctx)
            {
                setInt(ctx, streamFill(ctx->thisObj, argRef(ctx, 1), argInt(ctx, 2), argInt(ctx, 3)));
            }
            static void is_available(NativeContext *ctx)
            {
                Obj *s = ctx->thisObj;
                setInt(ctx, (s && s->cellCount >= 3) ? (s->cells[2].i - s->cells[1].i) : 0);
            }
            static void is_skip(NativeContext *ctx)
            {
                Obj *s = ctx->thisObj;
                if (!s || s->cellCount < 3)
                {
                    setLong(ctx, 0);
                    return;
                }
                int64_t n = argLongL(ctx, 1);
                int32_t avail = s->cells[2].i - s->cells[1].i;
                int64_t k = (n > avail) ? avail : n;
                if (k < 0)
                    k = 0;
                s->cells[1] = Value::fromInt(s->cells[1].i + static_cast<int32_t>(k));
                setLong(ctx, k);
            }
            static void is_close(NativeContext *ctx) { (void)ctx; }
            static void is_markSupported(NativeContext *ctx) { setInt(ctx, 0); }
            static void bais_markSupported(NativeContext *ctx) { setInt(ctx, 1); }

            // ---------------------------------------------------------------------
            // java.util.Timer / java.util.TimerTask
            // ---------------------------------------------------------------------
            // Pas de vrais threads Java ici : chaque tâche planifiée est suivie
            // dans g_timerTasks et son run() est exécuté sincroniquement depuis
            // tick() (fireTimers) dès que la clock virtuelle atteint nextFire.
            // Timer.cancel()/TimerTask.cancel() marquent simplement les entrées
            // comme annulées ; les entrées mortes sont purgées à chaque tick.

            struct JmeTimerTask
            {
                Obj *timer;      // propriétaire java/util/Timer (pour cancel)
                Obj *task;       // java/util/TimerTask (ou sous-classe)
                int64_t nextFire; // prochaine exécution (virtualMillis())
                int64_t period;  // 0 = one-shot, >0 = périodique
                bool cancelled;
            };
            static std::vector<JmeTimerTask> g_timerTasks;

            static void timerAdd(Obj *timer, Obj *task, int64_t delay, int64_t period)
            {
                if (!timer || !task)
                    return;
                for (size_t i = 0; i < g_timerTasks.size();)
                {
                    if (g_timerTasks[i].timer == timer && g_timerTasks[i].task == task)
                        g_timerTasks.erase(g_timerTasks.begin() + static_cast<long>(i));
                    else
                        ++i;
                }
                JmeTimerTask e;
                e.timer = timer;
                e.task = task;
                e.nextFire = virtualMillis() + delay;
                e.period = period;
                e.cancelled = false;
                g_timerTasks.push_back(e);
            }

            static void n_Timer_init(NativeContext *ctx) { (void)ctx; }

            static void n_Timer_scheduleOnce(NativeContext *ctx)
            {
                timerAdd(ctx->thisObj, argRef(ctx, 1), argLongL(ctx, 2), 0);
            }

            static void n_Timer_schedulePeriodic(NativeContext *ctx)
            {
                timerAdd(ctx->thisObj, argRef(ctx, 1), argLongL(ctx, 2), argLongL(ctx, 4));
            }

            static void n_Timer_cancel(NativeContext *ctx)
            {
                Obj *timer = ctx->thisObj;
                for (auto &e : g_timerTasks)
                    if (e.timer == timer)
                        e.cancelled = true;
            }

            static void n_TimerTask_cancel(NativeContext *ctx)
            {
                Obj *task = ctx->thisObj;
                bool found = false;
                for (auto &e : g_timerTasks)
                    if (e.task == task)
                    {
                        e.cancelled = true;
                        found = true;
                    }
                setInt(ctx, found ? 1 : 0);
            }

            void fireTimers()
            {
                if (g_timerTasks.empty() || !g_interp)
                    return;
                const int64_t now = virtualMillis();
                constexpr int64_t kBudget = 200000;
                for (size_t i = 0; i < g_timerTasks.size(); i++)
                {
                    JmeTimerTask e = g_timerTasks[i];
                    if (e.cancelled || e.nextFire > now)
                        continue;
                    if (!e.task || !e.task->cls)
                    {
                        g_timerTasks[i].cancelled = true;
                        continue;
                    }
                    Value res;
                    g_interp->setInstrBudget(kBudget);
                    bool ok = g_interp->invokeVirtual(e.task->cls, "run", "()V", e.task, nullptr, 0, res);
                    g_interp->setInstrBudget(-1);
                    if (i >= g_timerTasks.size())
                        continue;
                    JmeTimerTask &live = g_timerTasks[i];
                    if (live.cancelled || live.task != e.task)
                        continue;
                    if (e.period > 0 && ok)
                        live.nextFire = now + e.period;
                    else
                        live.cancelled = true;
                }
                size_t w = 0;
                for (size_t j = 0; j < g_timerTasks.size(); j++)
                {
                    if (g_timerTasks[j].cancelled)
                        continue;
                    if (j != w)
                        g_timerTasks[w] = g_timerTasks[j];
                    w++;
                }
                g_timerTasks.resize(w);
            }

            static void bais_init(NativeContext *ctx)
            {
                Obj *bytes = argRef(ctx, 1);
                if (!bytes)
                {
                    setRef(ctx, nullptr);
                    return;
                }
                int n = bytes->arrayLen;
                ctx->thisObj->cells[0] = Value::fromRef(bytes);
                ctx->thisObj->cells[1] = Value::fromInt(0);
                ctx->thisObj->cells[2] = Value::fromInt(n);
            }
            static void bais_initOffLen(NativeContext *ctx)
            {
                Obj *bytes = argRef(ctx, 1);
                int off = argInt(ctx, 2), len = argInt(ctx, 3);
                if (!bytes)
                {
                    setRef(ctx, nullptr);
                    return;
                }
                if (off < 0) off = 0;
                if (off > bytes->arrayLen) off = bytes->arrayLen;
                int lim = off + len;
                if (lim < off) lim = off;
                if (lim > bytes->arrayLen) lim = bytes->arrayLen;
                ctx->thisObj->cells[0] = Value::fromRef(bytes);
                ctx->thisObj->cells[1] = Value::fromInt(off);
                ctx->thisObj->cells[2] = Value::fromInt(lim);
            }
            static void bais_mark(NativeContext *ctx) { (void)ctx; }
            static void bais_reset(NativeContext *ctx)
            {
                if (ctx->thisObj)
                    ctx->thisObj->cells[1] = Value::fromInt(0);
            }

            static void dis_init(NativeContext *ctx) { ctx->thisObj->cells[0] = Value::fromRef(argRef(ctx, 1)); }

            static void dis_mark(NativeContext *ctx)
            {
                // DataInputStream.mark(int) : on mémorise la position du flux
                // sous-jacent (ByteArrayInputStream via getResourceAsStream).
                Obj *self = ctx->thisObj;
                Obj *in = self && self->cellCount >= 2 ? self->cells[0].o : nullptr;
                if (self && self->cellCount >= 2 && in && in->cellCount >= 2)
                    self->cells[1] = Value::fromInt(in->cells[1].i);
            }
            static void dis_reset(NativeContext *ctx)
            {
                Obj *self = ctx->thisObj;
                Obj *in = self && self->cellCount >= 2 ? self->cells[0].o : nullptr;
                if (self && self->cellCount >= 2 && in && in->cellCount >= 2)
                    in->cells[1] = self->cells[1];
            }

            static int32_t diByte(Obj *self)
            {
                Obj *in = self && self->cellCount >= 1 ? self->cells[0].o : nullptr;
                return in ? streamByte(in) : -1;
            }
            static int32_t diFill(Obj *self, Obj *dst, int off, int len)
            {
                Obj *in = self && self->cellCount >= 1 ? self->cells[0].o : nullptr;
                return in ? streamFill(in, dst, off, len) : -1;
            }

            static void di_read(NativeContext *ctx) { setInt(ctx, diByte(ctx->thisObj)); }
            static void di_readArr(NativeContext *ctx)
            {
                Obj *d = argRef(ctx, 1);
                setInt(ctx, diFill(ctx->thisObj, d, 0, d ? d->arrayLen : 0));
            }
            static void di_readArrII(NativeContext *ctx) { setInt(ctx, diFill(ctx->thisObj, argRef(ctx, 1), argInt(ctx, 2), argInt(ctx, 3))); }
            static void di_readBoolean(NativeContext *ctx) { setInt(ctx, diByte(ctx->thisObj) != 0 ? 1 : 0); }
            static void di_readByte(NativeContext *ctx) { setInt(ctx, static_cast<int8_t>(diByte(ctx->thisObj))); }
            static void di_readUnsignedByte(NativeContext *ctx) { setInt(ctx, diByte(ctx->thisObj)); }

            static int64_t diReadN(Obj *self, int n)
            {
                uint64_t v = 0;
                for (int i = 0; i < n; i++)
                {
                    int b = diByte(self);
                    if (b < 0)
                        return -1;
                    v = (v << 8) | static_cast<uint64_t>(b);
                }
                return static_cast<int64_t>(v);
            }
            static void di_readShort(NativeContext *ctx) { setInt(ctx, static_cast<int16_t>(diReadN(ctx->thisObj, 2))); }
            static void di_readUnsignedShort(NativeContext *ctx) { setInt(ctx, static_cast<int32_t>(diReadN(ctx->thisObj, 2) & 0xFFFF)); }
            static void di_readChar(NativeContext *ctx) { setInt(ctx, static_cast<int32_t>(diReadN(ctx->thisObj, 2) & 0xFFFF)); }
            static void di_readInt(NativeContext *ctx) { setInt(ctx, static_cast<int32_t>(diReadN(ctx->thisObj, 4))); }
            static void di_readLong(NativeContext *ctx) { setLong(ctx, diReadN(ctx->thisObj, 8)); }
            static void di_readFully(NativeContext *ctx)
            {
                Obj *d = argRef(ctx, 1);
                if (d)
                    diFill(ctx->thisObj, d, 0, d->arrayLen);
            }
            static void di_readFullyII(NativeContext *ctx) { diFill(ctx->thisObj, argRef(ctx, 1), argInt(ctx, 2), argInt(ctx, 3)); }

            static void di_readUTF(NativeContext *ctx)
            {
                int lenH = diByte(ctx->thisObj), lenL = diByte(ctx->thisObj);
                if (lenH < 0 || lenL < 0)
                {
                    setRef(ctx, g_rt->heap().newString(""));
                    return;
                }
                int len = (lenH << 8) | lenL;
                std::string out;
                out.reserve(static_cast<size_t>(len) * 2);
                for (int i = 0; i < len;)
                {
                    int b = diByte(ctx->thisObj);
                    if (b < 0)
                        break;
                    if (b == 0xC0)
                    {
                        int b2 = diByte(ctx->thisObj);
                        if (b2 < 0)
                            break;
                        if (b == 0xC0 && b2 == 0x80)
                            out += '\0';
                        else
                        {
                            out += static_cast<char>(b);
                            out += static_cast<char>(b2);
                        }
                        i += 2;
                    }
                    else if (b < 0x80)
                    {
                        out += static_cast<char>(b);
                        i += 1;
                    }
                    else
                    {
                        int need = (b < 0xE0) ? 2 : 3;
                        std::string seq;
                        seq += static_cast<char>(b);
                        int ok = 1;
                        for (int k = 1; k < need; k++)
                        {
                            int b2 = diByte(ctx->thisObj);
                            if (b2 < 0)
                            {
                                ok = 0;
                                break;
                            }
                            seq += static_cast<char>(b2);
                        }
                        if (ok)
                            out += seq;
                        i += need;
                    }
                }
                setRef(ctx, g_rt->heap().newString(out));
            }

            static void di_skipBytes(NativeContext *ctx)
            {
                Obj *in = ctx->thisObj && ctx->thisObj->cellCount >= 1 ? ctx->thisObj->cells[0].o : nullptr;
                if (!in || in->cellCount < 3)
                {
                    setInt(ctx, 0);
                    return;
                }
                int n = argInt(ctx, 1);
                int32_t avail = in->cells[2].i - in->cells[1].i;
                if (n > avail)
                    n = avail;
                if (n < 0)
                    n = 0;
                in->cells[1] = Value::fromInt(in->cells[1].i + n);
                setInt(ctx, n);
            }

            // ---------------------------------------------------------------------
            // java.io.ByteArrayOutputStream / java.io.DataOutputStream
            // ---------------------------------------------------------------------
            // Symétrique du côté lecture ci-dessus, en beaucoup plus simple : pas de
            // dispatch OOP générique sur un OutputStream quelconque (comme le côté
            // lecture ne dispatche pas non plus sur un InputStream quelconque --
            // streamByte/streamFill lisent cells[0..2] à même la structure). Le
            // buffer accumulé est stocké comme un Obj de type String (cells[0]),
            // réutilisé comme conteneur d'octets bruts (std::string gère les octets
            // nuls sans souci) exactement comme StringBuffer stocke son contenu --
            // chaque write() remplace cells[0] par un nouvel Obj (bump allocator,
            // pas de réutilisation en place, cf. n_SB_setStr). DataOutputStream
            // enveloppe un ByteArrayOutputStream dans cells[0] et lit/écrit à
            // travers, comme DataInputStream le fait pour la lecture (diByte/diFill).

            static std::string baosStr(Obj *self)
            {
                if (self && self->cellCount >= 1)
                {
                    Obj *s = self->cells[0].o;
                    if (s && s->kind == ObjKind::String) return s->str;
                }
                return "";
            }
            static void baosSetStr(Obj *self, const std::string &s)
            {
                if (self && self->cellCount >= 1 && g_rt)
                    self->cells[0] = Value::fromRef(g_rt->heap().newString(s));
            }
            static void baos_init(NativeContext *ctx) { baosSetStr(ctx->thisObj, ""); }
            static void baos_initCap(NativeContext *ctx) { baosSetStr(ctx->thisObj, ""); }
            static void baos_writeByte(NativeContext *ctx)
            {
                baosSetStr(ctx->thisObj, baosStr(ctx->thisObj) + static_cast<char>(argInt(ctx, 1) & 0xFF));
            }
            static void baos_writeArr(NativeContext *ctx)
            {
                Obj *d = argRef(ctx, 1);
                if (!d || d->kind != ObjKind::ByteArray) return;
                std::string cur = baosStr(ctx->thisObj);
                for (int i = 0; i < d->arrayLen; i++)
                    cur += static_cast<char>(d->cells[i].i & 0xFF);
                baosSetStr(ctx->thisObj, cur);
            }
            static void baos_writeArrII(NativeContext *ctx)
            {
                Obj *d = argRef(ctx, 1);
                int off = argInt(ctx, 2), len = argInt(ctx, 3);
                if (!d || d->kind != ObjKind::ByteArray || off < 0 || len < 0 || off + len > d->arrayLen) return;
                std::string cur = baosStr(ctx->thisObj);
                for (int i = 0; i < len; i++)
                    cur += static_cast<char>(d->cells[off + i].i & 0xFF);
                baosSetStr(ctx->thisObj, cur);
            }
            static void baos_size(NativeContext *ctx) { setInt(ctx, static_cast<int32_t>(baosStr(ctx->thisObj).size())); }
            static void baos_reset(NativeContext *ctx) { baosSetStr(ctx->thisObj, ""); }
            static void baos_close(NativeContext *ctx) { (void)ctx; }
            static void baos_toByteArray(NativeContext *ctx)
            {
                std::string s = baosStr(ctx->thisObj);
                Obj *arr = ctx->rt->heap().newArray(ObjKind::ByteArray, static_cast<int32_t>(s.size()));
                if (arr)
                    for (size_t i = 0; i < s.size(); i++)
                        arr->cells[i] = Value::fromInt(static_cast<int8_t>(s[i]));
                setRef(ctx, arr);
            }

            static Obj *dosOut(Obj *self) { return self && self->cellCount >= 1 ? self->cells[0].o : nullptr; }
            static void dosWriteByte(Obj *self, int b)
            {
                Obj *out = dosOut(self);
                if (out) baosSetStr(out, baosStr(out) + static_cast<char>(b & 0xFF));
            }
            static void dosWriteN(Obj *self, int64_t v, int nbytes)
            {
                for (int i = nbytes - 1; i >= 0; i--)
                    dosWriteByte(self, static_cast<int>((v >> (i * 8)) & 0xFF));
            }
            static void dos_init(NativeContext *ctx) { ctx->thisObj->cells[0] = Value::fromRef(argRef(ctx, 1)); }
            static void dos_write(NativeContext *ctx) { dosWriteByte(ctx->thisObj, argInt(ctx, 1)); }
            static void dos_writeArr(NativeContext *ctx)
            {
                Obj *d = argRef(ctx, 1);
                if (!d || d->kind != ObjKind::ByteArray) return;
                for (int i = 0; i < d->arrayLen; i++)
                    dosWriteByte(ctx->thisObj, d->cells[i].i);
            }
            static void dos_writeArrII(NativeContext *ctx)
            {
                Obj *d = argRef(ctx, 1);
                int off = argInt(ctx, 2), len = argInt(ctx, 3);
                if (!d || d->kind != ObjKind::ByteArray || off < 0 || len < 0 || off + len > d->arrayLen) return;
                for (int i = 0; i < len; i++)
                    dosWriteByte(ctx->thisObj, d->cells[off + i].i);
            }
            static void dos_writeBoolean(NativeContext *ctx) { dosWriteByte(ctx->thisObj, argInt(ctx, 1) ? 1 : 0); }
            static void dos_writeByte(NativeContext *ctx) { dosWriteByte(ctx->thisObj, argInt(ctx, 1)); }
            static void dos_writeShort(NativeContext *ctx) { dosWriteN(ctx->thisObj, argInt(ctx, 1), 2); }
            static void dos_writeChar(NativeContext *ctx) { dosWriteN(ctx->thisObj, argInt(ctx, 1), 2); }
            static void dos_writeInt(NativeContext *ctx) { dosWriteN(ctx->thisObj, argInt(ctx, 1), 4); }
            static void dos_writeLong(NativeContext *ctx) { dosWriteN(ctx->thisObj, argLongL(ctx, 1), 8); }
            static void dos_writeUTF(NativeContext *ctx)
            {
                Obj *s = argRef(ctx, 1);
                std::string str = (s && s->kind == ObjKind::String) ? s->str : "";
                // Encodage "UTF-8 modifiée" simplifiée : ASCII tel quel, octet nul ->
                // 0xC0 0x80 (miroir exact de la lecture, di_readUTF plus haut). Les
                // codepoints >127 ne sont pas ré-encodés sur plusieurs octets (le
                // std::string du projet est déjà en octets bruts à ce stade) --
                // suffisant pour le texte ASCII que ces jeux écrivent réellement
                // (scores, noms, clés de préférences).
                std::string enc;
                enc.reserve(str.size());
                for (char c : str)
                {
                    if (c == '\0') { enc += '\xC0'; enc += '\x80'; }
                    else enc += c;
                }
                dosWriteN(ctx->thisObj, static_cast<int64_t>(enc.size()), 2);
                for (char c : enc)
                    dosWriteByte(ctx->thisObj, static_cast<unsigned char>(c));
            }
            static void dos_flush(NativeContext *ctx) { (void)ctx; }
            static void dos_close(NativeContext *ctx) { (void)ctx; }
            static void dos_size(NativeContext *ctx)
            {
                Obj *out = dosOut(ctx->thisObj);
                setInt(ctx, static_cast<int32_t>(baosStr(out).size()));
            }

            // --- Récupération de ressources depuis le JAR ---

            static void cl_getResourceAsStream(NativeContext *ctx)
            {
                Obj *name = argRef(ctx, 1);
                std::string path = (name && name->kind == ObjKind::String) ? name->str : "";
                while (!path.empty() && path[0] == '/')
                    path.erase(0, 1);
                jme::JarReader *jar = ctx->rt->jar();
                if (!jar || path.empty())
                {
                    setRef(ctx, nullptr);
                    return;
                }
                jme::JarEntry e;
                if (!jar->findEntry(path, e) || e.uncompressedSize > 4u * 1024 * 1024)
                {
                    if (jvm::jmeDebug())
                        fprintf(stderr, "[midp] res introuvable: \"%s\"\n", path.c_str());
                    setRef(ctx, nullptr);
                    return;
                }
                uint8_t *tmp = static_cast<uint8_t *>(std::malloc(e.uncompressedSize ? e.uncompressedSize : 1));
                if (!tmp)
                {
                    setRef(ctx, nullptr);
                    return;
                }
                size_t n = jar->extractEntry(path, tmp, e.uncompressedSize);
                if (n == 0)
                {
                    std::free(tmp);
                    setRef(ctx, nullptr);
                    return;
                }
                if (jvm::jmeDebug())
                    fprintf(stderr, "[midp] res \"%s\" : %zu octets\n", path.c_str(), n);
                Obj *bytes = g_rt->heap().newArray(ObjKind::ByteArray, static_cast<int32_t>(n));
                if (!bytes)
                {
                    std::free(tmp);
                    g_rt->reportOom();
                    setRef(ctx, nullptr);
                    return;
                }
                for (size_t i = 0; i < n; i++)
                    bytes->cells[i].u = tmp[i];
                std::free(tmp);
                setRef(ctx, makeStream(bytes, static_cast<int>(n)));
            }
            void registerIoNatives()
            {
                regN("java/lang/Class.getResourceAsStream:(Ljava/lang/String;)Ljava/io/InputStream;", cl_getResourceAsStream);
                regN("java/io/InputStream.read:()I", is_read);
                regN("java/io/InputStream.read:([B)I", is_readArr);
                regN("java/io/InputStream.read:([BII)I", is_readArrII);
                regN("java/io/InputStream.available:()I", is_available);
                regN("java/io/InputStream.skip:(J)J", is_skip);
                regN("java/io/InputStream.close:()V", is_close);
                regN("java/io/InputStream.markSupported:()Z", is_markSupported);
                regN("java/io/InputStream.mark:(I)V", bais_mark);
                regN("java/io/InputStream.reset:()V", bais_reset);
                regN("java/io/ByteArrayInputStream.<init>:([B)V", bais_init);
                regN("java/io/ByteArrayInputStream.<init>:([BII)V", bais_initOffLen);
                regN("java/io/ByteArrayInputStream.read:()I", is_read);
                regN("java/io/ByteArrayInputStream.read:([B)I", is_readArr);
                regN("java/io/ByteArrayInputStream.read:([BII)I", is_readArrII);
                regN("java/io/ByteArrayInputStream.available:()I", is_available);
                regN("java/io/ByteArrayInputStream.skip:(J)J", is_skip);
                regN("java/io/ByteArrayInputStream.close:()V", is_close);
                regN("java/io/ByteArrayInputStream.markSupported:()Z", bais_markSupported);
                regN("java/io/ByteArrayInputStream.mark:(I)V", bais_mark);
                regN("java/io/ByteArrayInputStream.reset:()V", bais_reset);
                regN("java/io/DataInputStream.<init>:(Ljava/io/InputStream;)V", dis_init);
                regN("java/io/DataInputStream.read:()I", di_read);
                regN("java/io/DataInputStream.read:([B)I", di_readArr);
                regN("java/io/DataInputStream.read:([BII)I", di_readArrII);
                regN("java/io/DataInputStream.readBoolean:()Z", di_readBoolean);
                regN("java/io/DataInputStream.readByte:()B", di_readByte);
                regN("java/io/DataInputStream.readUnsignedByte:()I", di_readUnsignedByte);
                regN("java/io/DataInputStream.readShort:()S", di_readShort);
                regN("java/io/DataInputStream.readUnsignedShort:()I", di_readUnsignedShort);
                regN("java/io/DataInputStream.readChar:()C", di_readChar);
                regN("java/io/DataInputStream.readInt:()I", di_readInt);
                regN("java/io/DataInputStream.readLong:()J", di_readLong);
                regN("java/io/DataInputStream.readFully:([B)V", di_readFully);
                regN("java/io/DataInputStream.readFully:([BII)V", di_readFullyII);
                regN("java/io/DataInputStream.readUTF:()Ljava/lang/String;", di_readUTF);
                regN("java/io/DataInputStream.close:()V", is_close);
                regN("java/io/DataInputStream.skipBytes:(I)I", di_skipBytes);
                regN("java/io/DataInputStream.mark:(I)V", dis_mark);
                regN("java/io/DataInputStream.reset:()V", dis_reset);
                regN("java/io/ByteArrayOutputStream.<init>:()V", baos_init);
                regN("java/io/ByteArrayOutputStream.<init>:(I)V", baos_initCap);
                regN("java/io/ByteArrayOutputStream.write:(I)V", baos_writeByte);
                regN("java/io/ByteArrayOutputStream.write:([B)V", baos_writeArr);
                regN("java/io/ByteArrayOutputStream.write:([BII)V", baos_writeArrII);
                regN("java/io/ByteArrayOutputStream.size:()I", baos_size);
                regN("java/io/ByteArrayOutputStream.reset:()V", baos_reset);
                regN("java/io/ByteArrayOutputStream.close:()V", baos_close);
                regN("java/io/ByteArrayOutputStream.toByteArray:()[B", baos_toByteArray);
                regN("java/io/DataOutputStream.<init>:(Ljava/io/OutputStream;)V", dos_init);
                regN("java/io/DataOutputStream.write:(I)V", dos_write);
                regN("java/io/DataOutputStream.write:([B)V", dos_writeArr);
                regN("java/io/DataOutputStream.write:([BII)V", dos_writeArrII);
                regN("java/io/DataOutputStream.writeBoolean:(Z)V", dos_writeBoolean);
                regN("java/io/DataOutputStream.writeByte:(I)V", dos_writeByte);
                regN("java/io/DataOutputStream.writeShort:(I)V", dos_writeShort);
                regN("java/io/DataOutputStream.writeChar:(I)V", dos_writeChar);
                regN("java/io/DataOutputStream.writeInt:(I)V", dos_writeInt);
                regN("java/io/DataOutputStream.writeLong:(J)V", dos_writeLong);
                regN("java/io/DataOutputStream.writeUTF:(Ljava/lang/String;)V", dos_writeUTF);
                regN("java/io/DataOutputStream.flush:()V", dos_flush);
                regN("java/io/DataOutputStream.close:()V", dos_close);
                regN("java/io/DataOutputStream.size:()I", dos_size);
                regN("java/util/TimerTask.<init>:()V", native_TimerTask_init);
                regN("java/util/TimerTask.cancel:()Z", n_TimerTask_cancel);
                regN("java/util/Timer.<init>:()V", n_Timer_init);
                regN("java/util/Timer.schedule:(Ljava/util/TimerTask;J)V", n_Timer_scheduleOnce);
                regN("java/util/Timer.schedule:(Ljava/util/TimerTask;JJ)V", n_Timer_schedulePeriodic);
                regN("java/util/Timer.scheduleAtFixedRate:(Ljava/util/TimerTask;J)V", n_Timer_scheduleOnce);
                regN("java/util/Timer.scheduleAtFixedRate:(Ljava/util/TimerTask;JJ)V", n_Timer_schedulePeriodic);
                regN("java/util/Timer.cancel:()V", n_Timer_cancel);
            }
        } // namespace detail
    } // namespace midp
} // namespace jvm
