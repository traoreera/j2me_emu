#include <dirent.h>
#include "core/debug.h"
#include "core/native.h"
#include "core/interpreter.h"

#include <algorithm>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <vector>
#include <unordered_map>
#include <ucontext.h>

namespace jvm
{

namespace
{
// clés = "Classe.méthode:descripteur"
std::unordered_map<std::string, NativeFn> &registry()
{
    static std::unordered_map<std::string, NativeFn> r;
    return r;
}

Obj *argRef(NativeContext *ctx, int i)
{
    if (i < 0 || i >= ctx->nargs)
        return nullptr;
    return ctx->args[i].o;
}

int32_t argInt(NativeContext *ctx, int i)
{
    if (i < 0 || i >= ctx->nargs)
        return 0;
    return ctx->args[i].i;
}

int64_t argLong(NativeContext *ctx, int i)
{
    if (i < 0 || i >= ctx->nargs)
        return 0;
    return ctx->args[i].l;
}

float argFloat(NativeContext *ctx, int i)
{
    if (i < 0 || i >= ctx->nargs)
        return 0;
    return ctx->args[i].f;
}

void setIntResult(NativeContext *ctx, int32_t v)
{
    if (ctx->result)
        *ctx->result = Value::fromInt(v);
}

void setLongResult(NativeContext *ctx, int64_t v)
{
    if (ctx->result)
        *ctx->result = Value::fromLong(v);
}

void setRefResult(NativeContext *ctx, Obj *o)
{
    if (ctx->result)
        *ctx->result = Value::fromRef(o);
}

// Obj "String" -> contenu
const std::string &strOf(Obj *o)
{
    static const std::string empty;
    if (!o || o->kind != ObjKind::String)
        return empty;
    return o->str;
}

// Constante String depuis le pool de la classe courante (utilisée par les
// natives String qui manipulent des literals). Ici on simplifie : on ne
// l'utilise pas.
} // namespace

NativeFn findNative(const std::string &key)
{
    auto it = registry().find(key);
    if (it != registry().end())
        return it->second;
    return nullptr;
}

const NativeFn *findNativePtr(const std::string &key)
{
    auto it = registry().find(key);
    return it != registry().end() ? &it->second : nullptr;
}

static std::string g_rmsDir;
void setRmsDir(const std::string &dir) { g_rmsDir = dir; }

void throwJava(NativeContext *ctx, const char *className)
{
    if (!ctx || !ctx->rt || ctx->exception)
        return;
    ClassInfo *c = ctx->rt->classInfoOfName(className);
    if (!c)
        c = ctx->rt->classInfoOfName("java/lang/RuntimeException");
    if (c)
        ctx->exception = ctx->rt->heap().newInstance(c);
}

void registerNative(const std::string &key, NativeFn fn)
{
    registry()[key] = fn;
}

// --- java.lang.Thread (scheduling coopératif par trames JME) —
// (registre de threads visible de midp_natives.cpp : déclaré au niveau jvm)
static std::vector<Obj *> g_threads;
void jme_threadStart(Obj *r)
{
    if (!r) return;
    for (Obj *t : g_threads)
        if (t == r) return;
    g_threads.push_back(r);
}
const std::vector<Obj *> &jme_threads() { return g_threads; }

// ---------------------------------------------------------------------
// Fibres coopératives (ucontext) : permettent à Thread.sleep()/yield(), ou
// à l'épuisement du budget d'instructions (cf. Interpreter::setYieldFn),
// de suspendre RÉELLEMENT l'exécution d'un run() -- pc, locales et pile
// d'appel C++ intacts (swapcontext) -- et de la reprendre exactement là à
// la trame suivante, au lieu de relancer run() depuis le début à chaque
// trame (ce qui empêchait toute progression pour les jeux dont la boucle
// principale dépasse le budget par trame, ex. limiteurs de FPS écrits en
// bytecode qui font des dizaines/centaines de milliers d'itérations).
// ---------------------------------------------------------------------
namespace
{
struct JmeFiber
{
    ucontext_t ctx{};
    ucontext_t callerCtx{};
    std::vector<char> stack;
    std::vector<uint8_t> arena;        // arène de frames propre à la fibre (cf. Interpreter::swapArena)
    uint8_t *aBase = nullptr;
    size_t aSize = 0, aOff = 0;
    bool finished = false;
    Obj *runnable = nullptr;
    ClassInfo *cls = nullptr;
    Interpreter *interp = nullptr;
};

std::unordered_map<Obj *, JmeFiber *> &fiberMap()
{
    static std::unordered_map<Obj *, JmeFiber *> m;
    return m;
}

// État d'ordonnancement d'un thread (clé : son Runnable). wakeUs : instant (horloge du jeu) avant lequel il ne doit
// pas être repris (0 = prêt à la prochaine trame). Thread.sleep(ms) / Object.wait(ms) le posent puis suspendent la fibre.
struct SchedState
{
    int64_t wakeUs = 0;
    Obj *waitObj = nullptr; // objet sur lequel le thread fait wait(ms) : notify() le réveille
    bool sleptSinceFlush = false; // un sleep() a eu lieu depuis le dernier flushGraphics() : la boucle se cadence seule
    int64_t lastFlushUs = -1;
    int tickSeen = -1;
    int runs = 0;           // reprises pendant la trame courante
    bool ranImmediate = false;
};
std::unordered_map<Obj *, SchedState> &schedMap()
{
    static std::unordered_map<Obj *, SchedState> m;
    return m;
}
int g_schedTick = 0;
int64_t g_schedEndUs = 0;
// JME_SLEEP=frame : ancien comportement (sleep = céder jusqu'à la trame suivante, durée ignorée).
bool sleepIsFrame()
{
    static const bool v = []() { const char *e = getenv("JME_SLEEP"); return e && strcmp(e, "frame") == 0; }();
    return v;
}

JmeFiber *g_startingFiber = nullptr; // passage d'argument au trampoline (makecontext ne
                                      // garantit pas le passage fiable de pointeurs 64 bits)
JmeFiber *g_currentFiber = nullptr;  // fibre en cours d'exécution, pour Thread.sleep/yield

void fiberTrampoline()
{
    JmeFiber *f = g_startingFiber;
    Value res;
    f->interp->invokeVirtual(f->cls, "run", "()V", f->runnable, nullptr, 0, res);
    f->finished = true;
    // Le retour normal de cette fonction déclenche le swapcontext vers
    // callerCtx via uc_link (cf. jme_threadResume).
}
} // namespace

// Reprend (ou démarre) le run() de `r` pour une trame. Retourne true si
// run() est allé jusqu'au bout (thread terminé, à oublier), false s'il a
// été suspendu (budget épuisé ou sleep/yield) et devra être repris à la
// prochaine trame.
bool jme_threadResume(Obj *r, Interpreter *interp, ClassInfo *cls)
{
    if (!r) return true;
    JmeFiber *&f = fiberMap()[r];
    if (jvm::jmeDebug())
        fprintf(stderr, "jme_threadResume r=%p %s fiber\n", (void*)r, f ? "resuming existing" : "CREATING NEW");
    if (!f)
    {
        f = new JmeFiber();
        f->stack.resize(512 * 1024);
        f->arena.resize(192 * 1024);
        f->aBase = f->arena.data();
        f->aSize = f->arena.size();
        f->aOff = 0;
        f->runnable = r;
        f->cls = cls;
        f->interp = interp;
        getcontext(&f->ctx);
        f->ctx.uc_stack.ss_sp = f->stack.data();
        f->ctx.uc_stack.ss_size = f->stack.size();
        f->ctx.uc_link = &f->callerCtx;
        makecontext(&f->ctx, fiberTrampoline, 0);
    }
    JmeFiber *prevCurrent = g_currentFiber;
    g_currentFiber = f;
    g_startingFiber = f;
    interp->setYieldFn([f]() { swapcontext(&f->ctx, &f->callerCtx); });
    interp->swapArena(f->aBase, f->aSize, f->aOff); // installe l'arène de la fibre (f->a* = celle de l'appelant)
    const char *callerLow = interp->stackLow();
    interp->setStackLow(f->stack.data() + 48 * 1024); // marge pour les natives et le déroulement de l'exception
    swapcontext(&f->callerCtx, &f->ctx);
    interp->setStackLow(callerLow);
    interp->swapArena(f->aBase, f->aSize, f->aOff); // rend l'arène de l'appelant (f->a* = celle de la fibre)
    interp->clearYieldFn();
    g_currentFiber = prevCurrent;
    return f->finished;
}

// Appelé par Thread.sleep()/Thread.yield() : suspend immédiatement la
// fibre courante (no-op si on n'est pas dans une fibre, ex. run() appelé
// synchroniquement hors ordonnanceur).
void jme_yieldNow()
{
    if (g_currentFiber)
        swapcontext(&g_currentFiber->ctx, &g_currentFiber->callerCtx);
}

// Ordonnanceur par événements d'une trame : la trame couvre [début, endUs] de l'horloge du jeu. Un thread qui dort
// jusqu'à un instant DANS la trame est repris à cet instant (l'horloge que voit le jeu vaut alors cet instant), donc
// sleep(20) donne 50 itérations/s même avec des trames de 16,7 ms. Les threads « immédiats » (yield, flushGraphics,
// budget épuisé) sont repris une seule fois par trame, à la fin de celle-ci.
void jme_schedBegin(int tickN, int64_t endUs)
{
    g_schedTick = tickN;
    g_schedEndUs = endUs;
}

Obj *jme_schedNext(const std::vector<Obj *> &threads)
{
    Obj *best = nullptr;
    int64_t bestEff = 0;
    for (Obj *r : threads)
    {
        auto fit = fiberMap().find(r);
        if (fit != fiberMap().end() && fit->second->finished)
            continue;
        SchedState &st = schedMap()[r];
        if (st.tickSeen != g_schedTick)
        {
            st.tickSeen = g_schedTick;
            st.runs = 0;
            st.ranImmediate = false;
        }
        const bool immediate = st.wakeUs <= 0;
        if (immediate && st.ranImmediate)
            continue;
        const int64_t eff = immediate ? g_schedEndUs : st.wakeUs;
        if (eff > g_schedEndUs || st.runs >= 6)
            continue;
        if (!best || eff < bestEff)
        {
            best = r;
            bestEff = eff;
        }
    }
    if (!best)
        return nullptr;
    SchedState &st = schedMap()[best];
    if (st.wakeUs <= 0)
        st.ranImmediate = true;
    st.runs++;
    st.wakeUs = 0;
    st.waitObj = nullptr;
    setVirtualMicros(bestEff);
    return best;
}

// flushGraphics() : sur un vrai téléphone il est synchronisé sur l'affichage, et les boucles de jeu SANS sleep() s'y
// cadencent (Stalker : 300 images/s sans cela). On cède donc la main, avec au moins 33 ms (30 images/s) entre deux
// flush du même thread. Si la boucle dort déjà (sleep/wait entre deux flush), elle est cadencée par ses propres délais :
// aucune attente en plus.
void jme_flushYield()
{
    if (!g_currentFiber)
        return;
    if (sleepIsFrame())
    {
        jme_yieldNow();
        return;
    }
    SchedState &st = schedMap()[g_currentFiber->runnable];
    if (st.sleptSinceFlush)
    {
        st.sleptSinceFlush = false;
        st.lastFlushUs = virtualMicros();
        return;
    }
    const int64_t now = virtualMicros();
    constexpr int64_t kMinFlushPeriodUs = 33333;
    if (st.lastFlushUs >= 0 && now < st.lastFlushUs + kMinFlushPeriodUs)
        st.wakeUs = st.lastFlushUs + kMinFlushPeriodUs;
    st.lastFlushUs = st.wakeUs > now ? st.wakeUs : now;
    jme_yieldNow();
}

void jme_threadForget(Obj *r)
{
    schedMap().erase(r);
    for (size_t i = 0; i < g_threads.size(); i++)
        if (g_threads[i] == r) { g_threads.erase(g_threads.begin() + (ptrdiff_t)i); break; }
    auto it = fiberMap().find(r);
    if (it != fiberMap().end())
    {
        delete it->second;
        fiberMap().erase(it);
    }
}

// GC : borne HAUTE de la pile C++ RÉELLEMENT active en ce moment -- celle de la fibre en cours si une fibre
// tourne (son `ucontext` a littéralement échangé le registre SP dessus), sinon aucune (le fil principal est
// couvert séparément par src/app/main.cpp, qui connaît sa propre pile). Indispensable : un handler natif
// (ex. img_createWH) qui alloue un objet, le garde un instant dans une variable C++ LOCALE (avant de
// l'écrire dans un champ/tableau Java), puis alloue ENCORE avant de s'en servir, expose cette variable à
// AUCUNE racine Java -- seul un balayage conservateur de la pile C++ elle-même la voit. Sans ça, un cycle GC
// déclenché par cette deuxième allocation pouvait libérer puis RÉUTILISER la mémoire du premier objet
// pendant que le handler s'apprêtait encore à écrire dedans -- corruption du tas (repéré avec
// JME_GC_STRESS=1 : `arrayLen` d'un bloc libre devenu un entier négatif, interprété en taille énorme).
bool jme_currentFiberStackTop(uint8_t *&top)
{
    if (!g_currentFiber)
        return false;
    top = reinterpret_cast<uint8_t *>(g_currentFiber->stack.data() + g_currentFiber->stack.size());
    return true;
}

void jme_gcScanThreadingRoots(Heap::Marker &m)
{
    for (Obj *r : g_threads)
        m.markObj(r);
    for (auto &kv : schedMap())
    {
        m.markObj(kv.first);
        m.markObj(kv.second.waitObj);
    }
    // Fibres SUSPENDUES : leur arène (`aBase`/`aOff`) contient alors leurs VRAIES locales+pile Java figées.
    // La fibre ACTIVE (g_currentFiber) est exclue -- pendant qu'elle tourne, `swapArena` a échangé ses champs
    // avec ceux de l'appelant (typiquement une arène "à vide", rien à perdre à l'ignorer ici) ; sa vraie
    // arène active est déjà scannée via `Interpreter::scanActiveFrames` par l'appelant de cette fonction.
    for (auto &kv : fiberMap())
    {
        JmeFiber *f = kv.second;
        if (!f || f == g_currentFiber)
            continue;
        m.scan(f->aBase, f->aOff);
    }
}

namespace
{
void n_Object_init(NativeContext *) {}
void n_Object_getClass(NativeContext *ctx)
{
    // o->cls n'est peuplé que pour ObjKind::Instance : les String/tableaux
    // (o->cls == nullptr par construction, cf. Heap::newString/newArray)
    // faisaient planter ceci sur o->cls->name. Même correspondance que
    // runtimeClassOf() dans interpreter.cpp.
    Obj *o = argRef(ctx, 0);
    std::string cn;
    if (o)
    {
        switch (o->kind)
        {
        case ObjKind::String: cn = "java/lang/String"; break;
        case ObjKind::Class: cn = "java/lang/Class"; break;
        case ObjKind::ByteArray: case ObjKind::ShortArray: case ObjKind::IntArray:
        case ObjKind::LongArray: case ObjKind::FloatArray: case ObjKind::DoubleArray:
        case ObjKind::CharArray: case ObjKind::BoolArray: case ObjKind::ObjArray:
            cn = "java/lang/Object"; break;
        default: cn = o->cls ? o->cls->name : ""; break;
        }
    }
    Obj *co = ctx->rt->heap().classObjFor(cn);
    setRefResult(ctx, co);
}
void n_Object_equals(NativeContext *ctx)
{
    setIntResult(ctx, (argRef(ctx, 0) == argRef(ctx, 1)) ? 1 : 0);
}
void n_Object_hashCode(NativeContext *ctx)
{
    setIntResult(ctx, static_cast<int32_t>(reinterpret_cast<uintptr_t>(argRef(ctx, 0))));
}
void n_Object_toString(NativeContext *ctx)
{
    Obj *o = argRef(ctx, 0);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s@%p", o && o->cls ? o->cls->name.c_str() : "null", (void *)o);
    setRefResult(ctx, ctx->rt->heap().newString(buf));
}
// Thread.sleep(ms) : la fibre ne reprend qu'une fois l'horloge du jeu à +ms (cf. jme_schedNext). Hors fibre
// (startApp/paint/keyPressed appelés directement) rien à suspendre : sans effet, comme avant.
void sleepFiber(int64_t ms, Obj *waitObj)
{
    if (!g_currentFiber)
        return;
    static const bool dbg = getenv("JME_SLEEPDBG") != nullptr;
    if (dbg)
    {
        // Diagnostic : nombre de sleep()/wait(ms) par seconde de temps de JEU et de temps RÉEL.
        static int64_t lastGameUs = 0;
        static auto lastReal = std::chrono::steady_clock::now();
        static int n = 0;
        static int64_t sumMs = 0;
        n++;
        sumMs += ms;
        if (virtualMicros() - lastGameUs >= 1000000)
        {
            auto nr = std::chrono::steady_clock::now();
            fprintf(stderr, "[sleepdbg] %d sleep en %.2fs de jeu / %.2fs reels (arg moyen %lld ms)\n", n,
                    (virtualMicros() - lastGameUs) / 1e6, std::chrono::duration<double>(nr - lastReal).count(),
                    (long long)(sumMs / n));
            lastGameUs = virtualMicros();
            lastReal = nr;
            n = 0;
            sumMs = 0;
        }
    }
    if (ms > 0 && !sleepIsFrame())
    {
        // Plancher : beaucoup de boucles de jeu font sleep(5)/sleep(10) parce que leur travail par image occupait
        // déjà l'essentiel des ~33 ms d'un téléphone. Ici ce travail ne coûte presque rien : honorer ces délais
        // à la lettre ferait tourner le jeu 5 à 10 fois trop vite. On ne descend donc pas sous ~30 itérations/s
        // (JME_MIN_SLEEP=ms pour changer, 0 = à la lettre). Les délais plus longs sont respectés exactement.
        static const int64_t minSleepMs = []() {
            const char *e = getenv("JME_MIN_SLEEP");
            return e ? static_cast<int64_t>(atoi(e)) : 33;
        }();
        if (ms < minSleepMs)
            ms = minSleepMs;
        SchedState &st = schedMap()[g_currentFiber->runnable];
        st.wakeUs = virtualMicros() + ms * 1000;
        st.waitObj = waitObj;
        st.sleptSinceFlush = true;
    }
    jme_yieldNow();
}
// Object.wait()/wait(long)/notify()/notifyAll() : l'émulateur n'a pas de
// moniteur réel. Pacing de boucle de jeu = reposer la fibre jusqu'à la
// prochaine trame (cf. Thread.sleep), notify/notifyAll = no-op. Sans ces
// registrations, wait() était une méthode introuvable → Erreur non capturable
// par les handlers `catch (Exception)` (NoSuchMethodError est un Error), ce
// qui tuait silencieusement les fibres des jeux dont le thread principal
// rythme sa boucle à l'aide de wait(long) (ex. games/jump.jar : scène figée).
// wait() sans délai : reprise à la prochaine trame (l'appelant reteste sa condition). wait(ms) : dort ms, sauf si
// un notify()/notifyAll() sur le même objet le réveille avant.
void n_Object_wait(NativeContext *ctx) { (void)ctx; jme_yieldNow(); }
void n_Object_waitL(NativeContext *ctx) { sleepFiber(argLong(ctx, 1), ctx->thisObj); }
void n_Object_waitI(NativeContext *ctx) { sleepFiber(argInt(ctx, 1), ctx->thisObj); }
void n_Object_notify(NativeContext *ctx)
{
    for (auto &kv : schedMap())
        if (kv.second.waitObj && kv.second.waitObj == ctx->thisObj)
        {
            kv.second.wakeUs = 0;
            kv.second.waitObj = nullptr;
        }
}
void n_Object_notifyAll(NativeContext *ctx) { n_Object_notify(ctx); }

// java.lang.Throwable : seule la racine déclare ses natives/son champ
// message (cells[0]) -- Exception/RuntimeException et toutes les
// sous-classes concrètes (NullPointerException, IOException, ...) se
// contentent d'étendre Throwable sans rien redéclarer, et héritent donc
// <init>/getMessage/toString/printStackTrace via la même résolution
// virtuelle que n'importe quelle méthode Java normale -- inutile de
// dupliquer ces natives sur chaque sous-classe.
void n_Throwable_init(NativeContext *ctx)
{
    if (ctx->thisObj) ctx->thisObj->cells[0] = Value::fromRef(nullptr);
}
void n_Throwable_initMsg(NativeContext *ctx)
{
    if (ctx->thisObj) ctx->thisObj->cells[0] = Value::fromRef(argRef(ctx, 1));
}
void n_Throwable_getMessage(NativeContext *ctx)
{
    setRefResult(ctx, ctx->thisObj ? ctx->thisObj->cells[0].o : nullptr);
}
void n_Throwable_toString(NativeContext *ctx)
{
    Obj *self = ctx->thisObj;
    std::string cn = self && self->cls ? self->cls->name : "java/lang/Throwable";
    for (auto &ch : cn) if (ch == '/') ch = '.';
    Obj *msg = self ? self->cells[0].o : nullptr;
    std::string s = cn;
    if (msg && msg->kind == ObjKind::String) { s += ": "; s += msg->str; }
    setRefResult(ctx, ctx->rt->heap().newString(s));
}
void n_Throwable_printStackTrace(NativeContext *ctx)
{
    Obj *self = ctx->thisObj;
    std::string cn = self && self->cls ? self->cls->name : "?";
    Obj *msg = self ? self->cells[0].o : nullptr;
    if (msg && msg->kind == ObjKind::String)
        fprintf(stderr, "%s: %s\n", cn.c_str(), msg->str.c_str());
    else
        fprintf(stderr, "%s\n", cn.c_str());
}

// --- encodages : les String du projet sont des octets 8 bits (Latin-1). UTF-8 <-> Latin-1 : les caractères
// hors Latin-1 (cyrillique, thaï, CJK...) deviennent « ? » à la décodage (limite connue).
bool encIsUtf8(Obj *enc)
{
    if (!enc || enc->kind != ObjKind::String) return false;
    std::string e = enc->str;
    for (char &c : e) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return e == "UTF-8" || e == "UTF8";
}
std::string utf8ToLatin1(const uint8_t *p, size_t n)
{
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n;)
    {
        uint8_t b = p[i];
        if (b < 0x80) { out += static_cast<char>(b); i++; }
        else if ((b & 0xE0) == 0xC0 && i + 1 < n)
        {
            uint32_t cp = ((b & 0x1Fu) << 6) | (p[i + 1] & 0x3Fu);
            out += cp < 256 ? static_cast<char>(cp) : '?';
            i += 2;
        }
        else if ((b & 0xF0) == 0xE0 && i + 2 < n) { out += '?'; i += 3; }
        else if ((b & 0xF8) == 0xF0 && i + 3 < n) { out += '?'; i += 4; }
        else { out += '?'; i++; }
    }
    return out;
}
std::string latin1ToUtf8(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
    {
        if (c < 0x80) out += static_cast<char>(c);
        else { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return out;
}

void n_String_initBytes(NativeContext *ctx)
{
    Obj *self = argRef(ctx, 0);
    Obj *data = argRef(ctx, 1);
    if (!self || !data || data->kind != ObjKind::ByteArray) return;
    self->kind = ObjKind::String;
    self->str.clear();
    std::string raw;
    raw.reserve(static_cast<size_t>(data->arrayLen));
    for (int i = 0; i < data->arrayLen; i++)
        raw += static_cast<char>(data->cells[i].u & 0xFF);
    self->str = encIsUtf8(argRef(ctx, 2)) ? utf8ToLatin1(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()) : raw;
}

void n_String_initBytesRange(NativeContext *ctx)
{
    Obj *self = argRef(ctx, 0);
    Obj *data = argRef(ctx, 1);
    int off = argInt(ctx, 2);
    int len = argInt(ctx, 3);
    if (!self || !data || data->kind != ObjKind::ByteArray) return;
    if (off < 0) off = 0;
    if (len < 0) len = 0;
    if (off + len > data->arrayLen) len = data->arrayLen - off > 0 ? data->arrayLen - off : 0;
    self->kind = ObjKind::String;
    self->str.clear();
    std::string raw;
    raw.reserve(static_cast<size_t>(len));
    for (int i = 0; i < len; i++)
        raw += static_cast<char>(data->cells[off + i].u & 0xFF);
    self->str = encIsUtf8(argRef(ctx, 4)) ? utf8ToLatin1(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()) : raw;
}

void n_String_initCharsRange(NativeContext *ctx)
{
    Obj *self = argRef(ctx, 0);
    Obj *data = argRef(ctx, 1);
    int off = argInt(ctx, 2);
    int len = argInt(ctx, 3);
    if (!self || !data || data->kind != ObjKind::CharArray) return;
    if (off < 0) off = 0;
    if (len < 0) len = 0;
    if (off + len > data->arrayLen) len = data->arrayLen - off > 0 ? data->arrayLen - off : 0;
    self->kind = ObjKind::String;
    self->str.clear();
    self->str.reserve(static_cast<size_t>(len));
    if (jvm::jmeDebug() && len > 0 && len < 64)
    {
        fprintf(stderr, "STRCHARS len=%d:", len);
        for (int i = 0; i < len; i++) fprintf(stderr, " %x", (unsigned)(data->cells[off + i].u & 0xFFFF));
        fprintf(stderr, "\n");
    }
    for (int i = 0; i < len; i++)
        self->str += static_cast<char>(data->cells[off + i].u & 0xFF);
}
void n_String_length(NativeContext *ctx)
{
    setIntResult(ctx, static_cast<int32_t>(strOf(argRef(ctx, 0)).size()));
}
void n_String_charAt(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    int i = argInt(ctx, 1);
    if (i < 0 || i >= static_cast<int>(s.size()))
    {
        throwJava(ctx, "java/lang/StringIndexOutOfBoundsException");
        return;
    }
    setIntResult(ctx, static_cast<unsigned char>(s[i]));
}
void n_String_toCharArray(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    Obj *o = ctx->rt->heap().newArray(ObjKind::CharArray, static_cast<int32_t>(s.size()));
    if (o)
        for (size_t i = 0; i < s.size(); i++)
            o->cells[i].u = static_cast<unsigned char>(s[i]);
    setRefResult(ctx, o);
}
void n_String_concat(NativeContext *ctx)
{
    Obj *a = argRef(ctx, 0);
    Obj *b = argRef(ctx, 1);
    setRefResult(ctx, ctx->rt->heap().newStringCat(a, b));
}
void n_String_equals(NativeContext *ctx)
{
    setIntResult(ctx, (strOf(argRef(ctx, 0)) == strOf(argRef(ctx, 1))) ? 1 : 0);
}
void n_String_equalsIgnoreCase(NativeContext *ctx)
{
    std::string a = strOf(argRef(ctx, 0)), b = strOf(argRef(ctx, 1));
    for (auto &ch : a) ch = static_cast<char>(tolower(ch));
    for (auto &ch : b) ch = static_cast<char>(tolower(ch));
    setIntResult(ctx, (a == b) ? 1 : 0);
}
void n_String_substring1(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    int b = argInt(ctx, 1);
    if (b < 0 || b > static_cast<int>(s.size()))
    {
        throwJava(ctx, "java/lang/StringIndexOutOfBoundsException");
        return;
    }
    setRefResult(ctx, ctx->rt->heap().newString(s.substr(b)));
}
void n_String_substring2(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    int b = argInt(ctx, 1);
    int e = argInt(ctx, 2);
    if (b < 0 || e > static_cast<int>(s.size()) || e < b)
    {
        throwJava(ctx, "java/lang/StringIndexOutOfBoundsException");
        return;
    }
    setRefResult(ctx, ctx->rt->heap().newString(s.substr(b, e - b)));
}
void n_String_indexOf(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    const std::string &sub = strOf(argRef(ctx, 1));
    setIntResult(ctx, static_cast<int32_t>(s.find(sub)));
}
void n_String_indexOfChar(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    char ch = static_cast<char>(argInt(ctx, 1));
    setIntResult(ctx, static_cast<int32_t>(s.find(ch)));
}
void n_String_indexOfCharFrom(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    char ch = static_cast<char>(argInt(ctx, 1));
    int from = argInt(ctx, 2);
    if (from < 0) from = 0;
    setIntResult(ctx, static_cast<int32_t>(s.find(ch, static_cast<size_t>(from))));
}
void n_String_indexOfStrFrom(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    const std::string &sub = strOf(argRef(ctx, 1));
    int from = argInt(ctx, 2);
    if (from < 0) from = 0;
    setIntResult(ctx, static_cast<int32_t>(s.find(sub, static_cast<size_t>(from))));
}
void n_String_trim(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { setRefResult(ctx, ctx->rt->heap().newString("")); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    setRefResult(ctx, ctx->rt->heap().newString(s.substr(a, b - a + 1)));
}
void n_String_toLowerCase(NativeContext *ctx)
{
    std::string s = strOf(argRef(ctx, 0));
    for (auto &ch : s) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    setRefResult(ctx, ctx->rt->heap().newString(s));
}
void n_String_toUpperCase(NativeContext *ctx)
{
    std::string s = strOf(argRef(ctx, 0));
    for (auto &ch : s) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    setRefResult(ctx, ctx->rt->heap().newString(s));
}
void n_String_compareTo(NativeContext *ctx)
{
    const std::string &a = strOf(argRef(ctx, 0));
    const std::string &b = strOf(argRef(ctx, 1));
    setIntResult(ctx, static_cast<int32_t>(a.compare(b)));
}
void n_String_startsWith(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    const std::string &p = strOf(argRef(ctx, 1));
    setIntResult(ctx, (s.rfind(p, 0) == 0) ? 1 : 0);
}
void n_String_endsWith(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    const std::string &p = strOf(argRef(ctx, 1));
    setIntResult(ctx, (s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0) ? 1 : 0);
}
void n_String_regionMatches(NativeContext *ctx)
{
    (void)ctx;
    setIntResult(ctx, 0);
}

void n_Math_abs(NativeContext *ctx)
{
    int32_t v = argInt(ctx, 0);
    setIntResult(ctx, v < 0 ? -v : v);
}
void n_Math_absL(NativeContext *ctx)
{
    int64_t v = argLong(ctx, 0);
    setLongResult(ctx, v < 0 ? -v : v);
}
void n_Math_min(NativeContext *ctx)
{
    int32_t a = argInt(ctx, 0), b = argInt(ctx, 1);
    setIntResult(ctx, a < b ? a : b);
}
void n_Math_max(NativeContext *ctx)
{
    int32_t a = argInt(ctx, 0), b = argInt(ctx, 1);
    setIntResult(ctx, a > b ? a : b);
}
void n_Math_sqrt(NativeContext *ctx)
{
    double d; std::memcpy(&d, &ctx->args[0].l, 8);
    double r = std::sqrt(d);
    setLongResult(ctx, *reinterpret_cast<int64_t *>(&r));
}
void n_Math_floor(NativeContext *ctx)
{
    double d; std::memcpy(&d, &ctx->args[0].l, 8);
    double r = std::floor(d);
    setLongResult(ctx, *reinterpret_cast<int64_t *>(&r));
}
void n_Math_ceil(NativeContext *ctx)
{
    double d; std::memcpy(&d, &ctx->args[0].l, 8);
    double r = std::ceil(d);
    setLongResult(ctx, *reinterpret_cast<int64_t *>(&r));
}
void n_Math_round(NativeContext *ctx)
{
    double d; std::memcpy(&d, &ctx->args[0].l, 8);
    setLongResult(ctx, static_cast<int64_t>(std::round(d)));
}
void n_Math_pow(NativeContext *ctx)
{
    double a, b; std::memcpy(&a, &ctx->args[0].l, 8); std::memcpy(&b, &ctx->args[1].l, 8);
    double r = std::pow(a, b);
    setLongResult(ctx, *reinterpret_cast<int64_t *>(&r));
}
void n_Math_random(NativeContext *ctx)
{
    double r = static_cast<double>(rand()) / RAND_MAX;
    setLongResult(ctx, *reinterpret_cast<int64_t *>(&r));
}

// --- java.lang.Math (variantes long) ---
void n_Math_minL(NativeContext *ctx)
{
    // chaque long = 2 slots de pile (1 Value + 1 slot vide) -> 2e long en args[2]
    int64_t a = argLong(ctx, 0), b = argLong(ctx, 2);
    setLongResult(ctx, a < b ? a : b);
}
void n_Math_maxL(NativeContext *ctx)
{
    int64_t a = argLong(ctx, 0), b = argLong(ctx, 2);
    setLongResult(ctx, a > b ? a : b);
}

ClassInfo *clsOf(NativeContext *ctx, const char *name)
{
    return ctx->rt ? ctx->rt->classInfoOfName(name) : nullptr;
}

std::string itos(int64_t v)
{
    char b[32];
    snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
    return std::string(b);
}

// --- java.lang.Integer ---
void n_Integer_init(NativeContext *ctx)
{
    if (ctx->thisObj && ctx->thisObj->cells)
        ctx->thisObj->cells[0] = Value::fromInt(argInt(ctx, 1));
}
void n_Integer_intValue(NativeContext *ctx)
{
    setIntResult(ctx, ctx->thisObj ? ctx->thisObj->cells[0].i : 0);
}
void n_Integer_byteValue(NativeContext *ctx)
{
    setIntResult(ctx, static_cast<int8_t>(ctx->thisObj ? ctx->thisObj->cells[0].i : 0));
}
void n_Integer_shortValue(NativeContext *ctx)
{
    setIntResult(ctx, static_cast<int16_t>(ctx->thisObj ? ctx->thisObj->cells[0].i : 0));
}
void n_Integer_longValue(NativeContext *ctx)
{
    setLongResult(ctx, ctx->thisObj ? ctx->thisObj->cells[0].i : 0);
}
void n_Integer_hashCode(NativeContext *ctx)
{
    setIntResult(ctx, ctx->thisObj ? ctx->thisObj->cells[0].i : 0);
}
void n_Integer_toString(NativeContext *ctx)
{
    setRefResult(ctx, ctx->rt->heap().newString(itos(ctx->thisObj ? ctx->thisObj->cells[0].i : 0)));
}
void n_String_intern(NativeContext *ctx)
{
    Obj *o = ctx->thisObj;
    setRefResult(ctx, (o && o->kind == ObjKind::String) ? ctx->rt->heap().internString(o->str) : o);
}
void n_Integer_toStringS(NativeContext *ctx)
{
    setRefResult(ctx, ctx->rt->heap().newString(itos(argInt(ctx, 0))));
}
void n_Integer_valueOf(NativeContext *ctx)
{
    ClassInfo *c = clsOf(ctx, "java/lang/Integer");
    Obj *o = c ? ctx->rt->heap().newInstance(c) : nullptr;
    if (o) o->cells[0] = Value::fromInt(argInt(ctx, 0));
    setRefResult(ctx, o);
}
// Analyse stricte à la Java : [+-]?chiffres, dans la plage du type, sinon NumberFormatException.
bool parseJavaInt(const std::string &t, int radix, int64_t lo, int64_t hi, int64_t &out)
{
    if (t.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (t[0] == '-' || t[0] == '+') { neg = (t[0] == '-'); i = 1; }
    if (i >= t.size()) return false;
    int64_t v = 0;
    for (; i < t.size(); i++)
    {
        int c = static_cast<unsigned char>(t[i]), d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else return false;
        if (d >= radix) return false;
        v = v * radix + d;
        if (v > (static_cast<int64_t>(1) << 62)) return false;
    }
    if (neg) v = -v;
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}
void n_Integer_parseInt(NativeContext *ctx)
{
    Obj *s = argRef(ctx, 0);
    int64_t v;
    if (!s || s->kind != ObjKind::String || !parseJavaInt(s->str, 10, INT32_MIN, INT32_MAX, v))
    {
        throwJava(ctx, "java/lang/NumberFormatException");
        return;
    }
    setIntResult(ctx, static_cast<int32_t>(v));
}
void n_Integer_equals(NativeContext *ctx)
{
    Obj *k = argRef(ctx, 1);
    int v = ctx->thisObj ? ctx->thisObj->cells[0].i : 0;
    setIntResult(ctx, (k && k->kind == ObjKind::Instance) ? (v == k->cells[0].i) : 0);
}
void n_Integer_compareTo(NativeContext *ctx)
{
    Obj *k = argRef(ctx, 1);
    int v = ctx->thisObj ? ctx->thisObj->cells[0].i : 0;
    int kv = (k && k->kind == ObjKind::Instance) ? k->cells[0].i : 0;
    setIntResult(ctx, v < kv ? -1 : (v > kv ? 1 : 0));
}

// --- java.lang.StringBuffer ---
void n_SB_setStr(NativeContext *ctx, const std::string &s)
{
    if (ctx->thisObj && ctx->thisObj->cells)
        ctx->thisObj->cells[0] = Value::fromRef(ctx->rt->heap().newString(s));
    // append() renvoie `this` (StringBuffer) pour permettre le chaînage
    // (sb.append(a).append(b)...). Sans ceci, l'appel suivant de la chaîne
    // recevait un récepteur null. Sans effet sur les <init> (void), qui
    // partagent ce helper et ignorent simplement le résultat.
    if (ctx->result)
        *ctx->result = Value::fromRef(ctx->thisObj);
}
std::string sbStr(Obj *sb)
{
    if (sb && sb->kind == ObjKind::Instance && sb->cells)
    {
        Obj *s = sb->cells[0].o;
        if (s && s->kind == ObjKind::String) return s->str;
    }
    return "";
}
void n_SB_init0(NativeContext *ctx) { n_SB_setStr(ctx, ""); }
void n_SB_initCap(NativeContext *ctx) { n_SB_setStr(ctx, ""); }
void n_SB_init(NativeContext *ctx)
{
    Obj *s = argRef(ctx, 1);
    n_SB_setStr(ctx, (s && s->kind == ObjKind::String) ? s->str : "");
}
void n_SB_appendS(NativeContext *ctx)
{
    n_SB_setStr(ctx, sbStr(ctx->thisObj) + strOf(argRef(ctx, 1)));
}
void n_SB_appendI(NativeContext *ctx)
{
    n_SB_setStr(ctx, sbStr(ctx->thisObj) + itos(argInt(ctx, 1)));
}
void n_SB_appendL(NativeContext *ctx)
{
    n_SB_setStr(ctx, sbStr(ctx->thisObj) + itos(argLong(ctx, 1)));
}
void n_SB_appendC(NativeContext *ctx)
{
    std::string cur = sbStr(ctx->thisObj);
    cur += static_cast<char>(argInt(ctx, 1));
    n_SB_setStr(ctx, cur);
}
void n_SB_appendZ(NativeContext *ctx)
{
    n_SB_setStr(ctx, sbStr(ctx->thisObj) + (argInt(ctx, 1) ? "true" : "false"));
}
void n_SB_appendO(NativeContext *ctx)
{
    Obj *o = argRef(ctx, 1);
    std::string cur = sbStr(ctx->thisObj);
    if (o && o->kind == ObjKind::String) cur += o->str;
    else if (o && o->kind == ObjKind::Instance) cur += strOf(o);
    else cur += "null";
    n_SB_setStr(ctx, cur);
}
void n_SB_appendF(NativeContext *ctx)
{
    char b[32];
    snprintf(b, sizeof b, "%.7g", static_cast<double>(argFloat(ctx, 1)));
    n_SB_setStr(ctx, sbStr(ctx->thisObj) + b);
}
void n_SB_appendD(NativeContext *ctx)
{
    int64_t bits = argLong(ctx, 1);
    double d;
    std::memcpy(&d, &bits, 8);
    char b[32];
    snprintf(b, sizeof b, "%.14g", d);
    n_SB_setStr(ctx, sbStr(ctx->thisObj) + b);
}
void n_SB_toString(NativeContext *ctx)
{
    Obj *s = ctx->thisObj ? ctx->thisObj->cells[0].o : nullptr;
    setRefResult(ctx, (s && s->kind == ObjKind::String) ? s : ctx->rt->heap().newString(""));
}
void n_SB_length(NativeContext *ctx)
{
    setIntResult(ctx, static_cast<int>(sbStr(ctx->thisObj).size()));
}
void n_SB_charAt(NativeContext *ctx)
{
    std::string s = sbStr(ctx->thisObj);
    int i = argInt(ctx, 1);
    setIntResult(ctx, (i >= 0 && i < static_cast<int>(s.size())) ? static_cast<unsigned char>(s[i]) : 0);
}
void n_SB_setCharAt(NativeContext *ctx)
{
    std::string s = sbStr(ctx->thisObj);
    int i = argInt(ctx, 1);
    if (i >= 0 && i < static_cast<int>(s.size()))
    {
        s[i] = static_cast<char>(argInt(ctx, 2));
        n_SB_setStr(ctx, s);
    }
}
void n_SB_setLength(NativeContext *ctx)
{
    std::string s = sbStr(ctx->thisObj);
    int n = argInt(ctx, 1);
    if (n < 0) n = 0;
    if (static_cast<int>(s.size()) > n) s.resize(static_cast<size_t>(n));
    else for (int i = static_cast<int>(s.size()); i < n; i++) s += '\0';
    n_SB_setStr(ctx, s);
}
void n_SB_delete(NativeContext *ctx)
{
    std::string s = sbStr(ctx->thisObj);
    int st = argInt(ctx, 1), en = argInt(ctx, 2);
    if (st < 0) st = 0;
    int sz = static_cast<int>(s.size());
    if (st < sz)
    {
        if (en > sz) en = sz;
        s.erase(st, en - st);
        n_SB_setStr(ctx, s);
    }
}

// --- java.lang.Thread ---
void n_Thread_init(NativeContext *ctx)
{
    if (jvm::jmeDebug())
        fprintf(stderr, "Thread.<init>(runnable=%p)\n", (void *)argRef(ctx, 1));
    if (ctx->thisObj && ctx->thisObj->cells)
        ctx->thisObj->cells[0] = Value::fromRef(argRef(ctx, 1));
}
void n_Thread_start(NativeContext *ctx)
{
    // Un Thread amorcé avec un Runnable le stocke dans cells[0]. Mais le
    // pattern "sous-classe de Thread qui override run()" (très courant,
    // ex. net/frog_parrot/jump/GameThread) ne passe AUCUN Runnable :
    // cells[0] reste nul. Dans ce cas le "runnable" à lancer est le Thread
    // lui-même (sa run() redéfini est résolu par invocation virtuelle dans
    // la fibre). Sans ce fallback, ces jeux lancent un thread fantôme qui
    // n'exécute jamais rien (écran à noir).
    Obj *r = ctx->thisObj ? ctx->thisObj->cells[0].o : nullptr;
    if (!r)
        r = ctx->thisObj;
    if (jvm::jmeDebug())
        fprintf(stderr, "Thread.start(runnable=%p cls=%s)\n", (void *)r,
                r && r->cls ? r->cls->name.c_str() : "-");
    jme_threadStart(r);
}
void n_Thread_sleep(NativeContext *ctx) { sleepFiber(argLong(ctx, 0), nullptr); }
// Contrairement à sleep(), yield() n'est qu'une suggestion à l'ordonnanceur
// -- rien ne garantit, sur un vrai appareil, qu'un cycle de repaint complet
// s'intercale avant que le thread ne reprenne. Le traiter comme sleep()
// (suspension réelle jusqu'à la trame suivante) est trop fort : un yield()
// en tout début de run(), avant que le thread n'ait fini sa propre
// initialisation, laisse alors paint() s'exécuter entre les deux -- avec
// un état parfois incohérent si le bytecode du jeu suppose (comme sur un
// vrai device) que son préambule s'exécute d'un bloc avant tout repaint.
// Observé sur games/mortal_combat_new_b_240x320_173007.jar : run()
// réinitialise un champ d'état juste après son tout premier yield(),
// écrasant la transition que paint() venait de faire entre-temps --
// boucle infinie de réinitialisation de l'écran d'intro, jamais de
// progression vers le menu/gameplay. On ne suspend donc réellement que si
// le budget d'instructions de la trame est presque épuisé (même garde-fou
// anti-boucle-infinie que l'épuisement de budget silencieux dans
// l'interpréteur) ; sinon on continue immédiatement dans la même fibre.
void n_Thread_yield(NativeContext *ctx)
{
    int64_t left = ctx->interp->instrBudgetLeft();
    if (left >= 0 && left <= 100)
    {
        jme_yieldNow();
        return;
    }
    // Détection d'attente active ("spin-wait") : un `while (!cond)
    // Thread.yield();` exécute yield() toutes les quelques instructions et
    // consommerait TOUT le budget de la trame (200k instructions, ~30 ms) à
    // tourner en rond -- alors que la condition attendue (horloge virtuelle,
    // autre thread) ne peut de toute façon changer qu'entre deux trames.
    // Mesuré sur games/prince_of_persia_th : ~30 000 yield()/trame, 34 ms de
    // CPU par trame pour ne rien faire. À l'inverse, un yield() posé au milieu
    // d'un vrai travail (chargement, décodage) laisse passer beaucoup plus
    // d'instructions entre deux appels : on ne suspend donc que sur une
    // longue série de yield() séparés par très peu d'instructions.
    static int64_t lastLeft = -1;
    static int tinyGaps = 0;
    if (left >= 0 && lastLeft >= 0 && lastLeft - left >= 0 && lastLeft - left < 64)
    {
        if (++tinyGaps >= 64)
        {
            tinyGaps = 0;
            lastLeft = -1;
            jme_yieldNow();
            return;
        }
    }
    else
        tinyGaps = 0;
    lastLeft = left;
}
void n_Thread_setPriority(NativeContext *ctx) { (void)ctx; }
void n_Thread_interrupt(NativeContext *ctx) { (void)ctx; }
void n_Thread_join(NativeContext *ctx) { (void)ctx; }
void n_Thread_isAlive(NativeContext *ctx) { setIntResult(ctx, 1); }
void n_Thread_currentThread(NativeContext *ctx)
{
    ClassInfo *c = clsOf(ctx, "java/lang/Thread");
    static Obj *t = nullptr;
    if (!t && c) t = ctx->rt->heap().newInstance(c);
    setRefResult(ctx, t);
}

// --- javax.microedition.rms.RecordStore ---
// Implémentation minimale, en mémoire (non persistante d'un lancement à
// l'autre -- suffisant pour tester le chemin "aucune sauvegarde existante"
// d'un jeu, cf. games/prince.jar qui fait juste
// `if (rs.getNumRecords() > 0) charger... else initDefaut()`). Registre
// tenu côté C++, indexé par nom de magasin (deux openRecordStore(même nom)
// partagent les mêmes enregistrements, comme le vrai RMS).
enum { RS_NAME = 0 };

// --- java.lang.Boolean ---
void n_Boolean_init(NativeContext *ctx)
{
    if (ctx->thisObj && ctx->thisObj->cells)
        ctx->thisObj->cells[0] = Value::fromInt(argInt(ctx, 1) ? 1 : 0);
}
void n_Boolean_booleanValue(NativeContext *ctx)
{
    setIntResult(ctx, ctx->thisObj && ctx->thisObj->cells ? ctx->thisObj->cells[0].i : 0);
}
void n_Boolean_toString(NativeContext *ctx)
{
    bool v = ctx->thisObj && ctx->thisObj->cells && ctx->thisObj->cells[0].i;
    setRefResult(ctx, ctx->rt->heap().newString(v ? "true" : "false"));
}
void n_Boolean_hashCode(NativeContext *ctx)
{
    bool v = ctx->thisObj && ctx->thisObj->cells && ctx->thisObj->cells[0].i;
    setIntResult(ctx, v ? 1231 : 1237);
}
void n_Boolean_equals(NativeContext *ctx)
{
    Obj *k = argRef(ctx, 1);
    bool v = ctx->thisObj && ctx->thisObj->cells && ctx->thisObj->cells[0].i;
    setIntResult(ctx, (k && k->kind == ObjKind::Instance && k->cls == ctx->thisObj->cls && (k->cells[0].i != 0) == v) ? 1 : 0);
}
// Boolean.valueOf(boolean) : renvoie les singletons TRUE/FALSE (statics amorcés par midp::init).
void n_Boolean_valueOf(NativeContext *ctx)
{
    ClassInfo *c = clsOf(ctx, "java/lang/Boolean");
    bool v = argInt(ctx, 0) != 0;
    Obj *o = nullptr;
    if (c)
    {
        const MethodRecord *f = c->findField(v ? "TRUE" : "FALSE", "Ljava/lang/Boolean;");
        if (f && (size_t)f->slot < c->statics.size())
            o = c->statics[f->slot].o;
        if (!o)
        {
            o = ctx->rt->heap().newInstance(c);
            if (o) o->cells[0] = Value::fromInt(v ? 1 : 0);
        }
    }
    setRefResult(ctx, o);
}

// ---------------------------------------------------------------------------------------------
// javax.microedition.rms : RecordStore + RecordEnumeration
// ---------------------------------------------------------------------------------------------
// Modèle : `Store` par nom (deux open() du même nom partagent les données) ; identifiants de records
// STABLES et jamais réutilisés (deleteRecord laisse un trou, nextId ne recule pas). Persistance : un fichier
// par magasin dans rmsDir() ("<jeu>.rms/"). Format v1 (magasin dense, ids 1..n) : u32 nbRecords puis
// (u32 taille + octets) par record, little-endian ; format v2 (ids creux) : u32 0xFFFFFFF2, u32 nextId,
// u32 nbRecords, puis (u32 id, u32 taille, octets). Un fichier corrompu/tronqué = magasin vide, jamais fatal.
struct RsStore
{
    std::map<int, std::vector<uint8_t>> recs;
    int nextId = 1;
    int version = 0;
    int64_t modified = 0;
};
std::map<std::string, RsStore> &rsRegistry()
{
    static std::map<std::string, RsStore> m;
    return m;
}

std::string &rmsDir() { return g_rmsDir; }
std::string rsFile(const std::string &name)
{
    std::string f;
    for (char c : name)
        f += (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') ? c : '_';
    return rmsDir() + "/" + f + ".rms";
}
void rsSave(const std::string &name)
{
    if (rmsDir().empty()) return;
    auto it = rsRegistry().find(name);
    if (it == rsRegistry().end()) return;
    std::string cmd = "mkdir -p '" + rmsDir() + "'";
    if (system(cmd.c_str()) != 0) return;
    FILE *f = fopen(rsFile(name).c_str(), "wb");
    if (!f) return;
    auto w32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; fwrite(b, 1, 4, f); };
    const RsStore &st = it->second;
    bool dense = static_cast<int>(st.recs.size()) == st.nextId - 1;
    if (dense)
    {
        int expect = 1;
        for (const auto &kv : st.recs)
            if (kv.first != expect++) { dense = false; break; }
    }
    if (dense)
        w32(static_cast<uint32_t>(st.recs.size()));
    else
    {
        w32(0xFFFFFFF2u);
        w32(static_cast<uint32_t>(st.nextId));
        w32(static_cast<uint32_t>(st.recs.size()));
    }
    for (const auto &kv : st.recs)
    {
        if (!dense) w32(static_cast<uint32_t>(kv.first));
        w32(static_cast<uint32_t>(kv.second.size()));
        if (!kv.second.empty()) fwrite(kv.second.data(), 1, kv.second.size(), f);
    }
    fclose(f);
}
bool rsLoad(const std::string &name, RsStore &out)
{
    if (rmsDir().empty()) return false;
    FILE *f = fopen(rsFile(name).c_str(), "rb");
    if (!f) return false;
    auto r32 = [&](uint32_t &v) { uint8_t b[4]; if (fread(b, 1, 4, f) != 4) return false; v = b[0] | (b[1] << 8) | (b[2] << 16) | (uint32_t(b[3]) << 24); return true; };
    uint32_t n = 0, nextId = 0;
    bool v2 = false, ok = r32(n);
    if (ok && n == 0xFFFFFFF2u)
    {
        v2 = true;
        ok = r32(nextId) && r32(n);
    }
    ok = ok && n < 100000;
    for (uint32_t i = 0; ok && i < n; i++)
    {
        uint32_t id = i + 1, len = 0;
        if (v2 && !r32(id)) { ok = false; break; }
        if (!r32(len) || len > (16u << 20)) { ok = false; break; }
        std::vector<uint8_t> rec(len);
        if (len && fread(rec.data(), 1, len, f) != len) { ok = false; break; }
        out.recs[static_cast<int>(id)] = std::move(rec);
    }
    fclose(f);
    if (!ok) { out.recs.clear(); out.nextId = 1; return false; }
    out.nextId = v2 ? static_cast<int>(nextId) : static_cast<int>(n) + 1;
    if (out.nextId < 1) out.nextId = 1;
    for (const auto &kv : out.recs)
        if (kv.first >= out.nextId) out.nextId = kv.first + 1;
    return true;
}
bool rsExists(const std::string &name)
{
    if (rsRegistry().count(name)) return true;
    if (rmsDir().empty()) return false;
    FILE *f = fopen(rsFile(name).c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
RsStore &rsGet(const std::string &name)
{
    auto it = rsRegistry().find(name);
    if (it == rsRegistry().end())
    {
        RsStore st;
        rsLoad(name, st); // magasin rechargé du disque, ou neuf
        it = rsRegistry().emplace(name, std::move(st)).first;
    }
    return it->second;
}
void rsTouch(RsStore &st) { st.version++; st.modified = virtualMillis(); }

enum { RS_OPEN = 1 };
std::string rsName(NativeContext *ctx)
{
    Obj *n = ctx->thisObj && ctx->thisObj->cells ? ctx->thisObj->cells[RS_NAME].o : nullptr;
    return (n && n->kind == ObjKind::String) ? n->str : "";
}
// Magasin de l'objet RecordStore ; lève RecordStoreNotOpenException s'il est fermé.
RsStore *rsOpen(NativeContext *ctx)
{
    Obj *o = ctx->thisObj;
    if (!o || !o->cells || (o->cellCount > RS_OPEN && !o->cells[RS_OPEN].i))
    {
        throwJava(ctx, "javax/microedition/rms/RecordStoreNotOpenException");
        return nullptr;
    }
    return &rsGet(rsName(ctx));
}
Obj *rsBytes(NativeContext *ctx, const std::vector<uint8_t> &rec)
{
    Obj *b = ctx->rt->heap().newArray(ObjKind::ByteArray, static_cast<int>(rec.size()));
    if (b)
        for (size_t i = 0; i < rec.size(); i++)
            b->cells[i].u = rec[i];
    return b;
}

void rsOpenImpl(NativeContext *ctx, Obj *nameObj, bool create)
{
    if (!nameObj || nameObj->kind != ObjKind::String || nameObj->str.empty() || nameObj->str.size() > 32)
    {
        throwJava(ctx, "java/lang/IllegalArgumentException");
        return;
    }
    const std::string &name = nameObj->str;
    if (!rsExists(name) && !create)
    {
        throwJava(ctx, "javax/microedition/rms/RecordStoreNotFoundException");
        return;
    }
    rsGet(name);
    ClassInfo *c = clsOf(ctx, "javax/microedition/rms/RecordStore");
    Obj *rs = c ? ctx->rt->heap().newInstance(c) : nullptr;
    if (rs && rs->cells)
    {
        rs->cells[RS_NAME] = Value::fromRef(nameObj);
        if (rs->cellCount > RS_OPEN) rs->cells[RS_OPEN] = Value::fromInt(1);
    }
    setRefResult(ctx, rs);
}
void n_RS_open(NativeContext *ctx) { rsOpenImpl(ctx, argRef(ctx, 0), argInt(ctx, 1) != 0); }
void n_RS_open4(NativeContext *ctx) { rsOpenImpl(ctx, argRef(ctx, 0), argInt(ctx, 1) != 0); } // (name, create, authmode, writable)
void n_RS_openVendor(NativeContext *ctx) { throwJava(ctx, "javax/microedition/rms/RecordStoreNotFoundException"); }
void n_RS_close(NativeContext *ctx)
{
    if (ctx->thisObj && ctx->thisObj->cells && ctx->thisObj->cellCount > RS_OPEN)
    {
        if (!ctx->thisObj->cells[RS_OPEN].i) { throwJava(ctx, "javax/microedition/rms/RecordStoreNotOpenException"); return; }
        ctx->thisObj->cells[RS_OPEN] = Value::fromInt(0);
    }
}
void n_RS_getNumRecords(NativeContext *ctx) { if (RsStore *st = rsOpen(ctx)) setIntResult(ctx, static_cast<int32_t>(st->recs.size())); }
void n_RS_getName(NativeContext *ctx) { if (rsOpen(ctx)) setRefResult(ctx, ctx->rt->heap().newString(rsName(ctx))); }
void n_RS_getVersion(NativeContext *ctx) { if (RsStore *st = rsOpen(ctx)) setIntResult(ctx, st->version); }
void n_RS_getLastModified(NativeContext *ctx) { if (RsStore *st = rsOpen(ctx)) setLongResult(ctx, st->modified); }
void n_RS_getNextRecordID(NativeContext *ctx) { if (RsStore *st = rsOpen(ctx)) setIntResult(ctx, st->nextId); }
int64_t rsBytesUsed(const RsStore &st)
{
    int64_t n = 0;
    for (const auto &kv : st.recs) n += static_cast<int64_t>(kv.second.size()) + 8;
    return n;
}
void n_RS_getSize(NativeContext *ctx) { if (RsStore *st = rsOpen(ctx)) setIntResult(ctx, static_cast<int32_t>(rsBytesUsed(*st) + 32)); }
void n_RS_getSizeAvailable(NativeContext *ctx)
{
    if (RsStore *st = rsOpen(ctx))
    {
        int64_t avail = (1 << 20) - rsBytesUsed(*st); // « 1 Mo » de RMS par magasin
        setIntResult(ctx, static_cast<int32_t>(avail > 0 ? avail : 0));
    }
}
void n_RS_getRecord(NativeContext *ctx) // (int id, byte[] buf, int offset) -> taille copiée
{
    RsStore *st = rsOpen(ctx);
    if (!st) return;
    auto it = st->recs.find(argInt(ctx, 1));
    Obj *buf = argRef(ctx, 2);
    int off = argInt(ctx, 3);
    if (it == st->recs.end()) { throwJava(ctx, "javax/microedition/rms/InvalidRecordIDException"); return; }
    if (!buf || off < 0 || off + static_cast<int>(it->second.size()) > buf->arrayLen) { throwJava(ctx, "java/lang/ArrayIndexOutOfBoundsException"); return; }
    for (size_t i = 0; i < it->second.size(); i++)
        buf->cells[off + i].u = it->second[i];
    setIntResult(ctx, static_cast<int32_t>(it->second.size()));
}
void n_RS_getRecordBytes(NativeContext *ctx) // (int id) -> byte[]
{
    RsStore *st = rsOpen(ctx);
    if (!st) return;
    auto it = st->recs.find(argInt(ctx, 1));
    if (it == st->recs.end()) { throwJava(ctx, "javax/microedition/rms/InvalidRecordIDException"); return; }
    setRefResult(ctx, rsBytes(ctx, it->second));
}
void n_RS_getRecordSize(NativeContext *ctx)
{
    RsStore *st = rsOpen(ctx);
    if (!st) return;
    auto it = st->recs.find(argInt(ctx, 1));
    if (it == st->recs.end()) { throwJava(ctx, "javax/microedition/rms/InvalidRecordIDException"); return; }
    setIntResult(ctx, static_cast<int32_t>(it->second.size()));
}
std::vector<uint8_t> rsSlice(Obj *buf, int off, int len)
{
    std::vector<uint8_t> rec(static_cast<size_t>(len < 0 ? 0 : len));
    if (buf)
        for (int i = 0; i < len && off + i < buf->arrayLen; i++)
            if (off + i >= 0) rec[i] = static_cast<uint8_t>(buf->cells[off + i].u);
    return rec;
}
void n_RS_setRecord(NativeContext *ctx) // (int id, byte[] data, int offset, int numBytes)
{
    RsStore *st = rsOpen(ctx);
    if (!st) return;
    auto it = st->recs.find(argInt(ctx, 1));
    if (it == st->recs.end()) { throwJava(ctx, "javax/microedition/rms/InvalidRecordIDException"); return; }
    it->second = rsSlice(argRef(ctx, 2), argInt(ctx, 3), argInt(ctx, 4));
    rsTouch(*st);
    rsSave(rsName(ctx));
}
void n_RS_addRecord(NativeContext *ctx) // (byte[] data, int offset, int numBytes) -> id
{
    RsStore *st = rsOpen(ctx);
    if (!st) return;
    int len = argInt(ctx, 3);
    if (rsBytesUsed(*st) + len > (1 << 20)) { throwJava(ctx, "javax/microedition/rms/RecordStoreFullException"); return; }
    int id = st->nextId++;
    st->recs[id] = rsSlice(argRef(ctx, 1), argInt(ctx, 2), len);
    rsTouch(*st);
    rsSave(rsName(ctx));
    setIntResult(ctx, id);
}
void n_RS_deleteRecord(NativeContext *ctx)
{
    RsStore *st = rsOpen(ctx);
    if (!st) return;
    if (!st->recs.erase(argInt(ctx, 1))) { throwJava(ctx, "javax/microedition/rms/InvalidRecordIDException"); return; }
    rsTouch(*st);
    rsSave(rsName(ctx));
}
void n_RS_deleteStore(NativeContext *ctx)
{
    Obj *nameObj = argRef(ctx, 0);
    std::string name = (nameObj && nameObj->kind == ObjKind::String) ? nameObj->str : "";
    if (!rsExists(name)) { throwJava(ctx, "javax/microedition/rms/RecordStoreNotFoundException"); return; }
    rsRegistry().erase(name);
    if (!rmsDir().empty())
        remove(rsFile(name).c_str());
}
void n_RS_noop(NativeContext *) {}
// RecordStore.listRecordStores() : magasins connus (mémoire + fichiers "<nom>.rms"), null si aucun.
void n_RS_list(NativeContext *ctx)
{
    std::vector<std::string> names;
    for (const auto &kv : rsRegistry())
        names.push_back(kv.first);
    if (!rmsDir().empty())
    {
        if (DIR *d = opendir(rmsDir().c_str()))
        {
            while (dirent *e = readdir(d))
            {
                std::string fn = e->d_name;
                if (fn.size() > 4 && fn.compare(fn.size() - 4, 4, ".rms") == 0)
                {
                    bool known = false;
                    for (const auto &n : names)
                        if (rsFile(n) == rmsDir() + "/" + fn) { known = true; break; }
                    if (!known) names.push_back(fn.substr(0, fn.size() - 4));
                }
            }
            closedir(d);
        }
    }
    if (names.empty()) { setRefResult(ctx, nullptr); return; }
    std::sort(names.begin(), names.end());
    Obj *arr = ctx->rt->heap().newArray(ObjKind::ObjArray, static_cast<int>(names.size()));
    if (arr)
        for (size_t i = 0; i < names.size(); i++)
            arr->cells[i] = Value::fromRef(ctx->rt->heap().newString(names[i]));
    setRefResult(ctx, arr);
}

// enumerateRecords(filter, comparator, keepUpdated) : liste des ids retenus par `filter.matches([B)Z` (appelé pour de
// vrai), triés par `comparator.compare([B,[B)I` (PRECEDES=-1, EQUIVALENT=0, FOLLOWS=1) si fourni, sinon par id.
// L'énumération est un INSTANTANÉ : cells[0]=nom du magasin, cells[1]=int[] des ids, cells[2]=position, cells[3]=
// keepUpdated (non suivi), cells[4]=filtre, cells[5]=comparateur (pour rebuild()).
enum { RE_STORE = 0, RE_IDS = 1, RE_POS = 2, RE_KEEP = 3, RE_FILTER = 4, RE_CMP = 5 };
void reBuild(NativeContext *ctx, Obj *e, const std::string &name, Obj *filter, Obj *cmp)
{
    RsStore &st = rsGet(name);
    std::vector<int> ids;
    for (const auto &kv : st.recs)
    {
        bool keep = true;
        if (filter && filter->kind == ObjKind::Instance)
        {
            Value args[2] = {Value::fromRef(filter), Value::fromRef(rsBytes(ctx, kv.second))}, res;
            keep = ctx->interp->invokeVirtual(filter->cls, "matches", "([B)Z", filter, args, 2, res) && res.i != 0;
        }
        if (keep) ids.push_back(kv.first);
    }
    if (cmp && cmp->kind == ObjKind::Instance && ids.size() > 1)
    {
        // tri par insertion stable (le comparateur est du bytecode : pas de std::sort, qui exige un ordre strict cohérent)
        for (size_t i = 1; i < ids.size(); i++)
        {
            int cur = ids[i];
            size_t j = i;
            while (j > 0)
            {
                Value args[3] = {Value::fromRef(cmp), Value::fromRef(rsBytes(ctx, st.recs[ids[j - 1]])), Value::fromRef(rsBytes(ctx, st.recs[cur]))}, res;
                bool ok = ctx->interp->invokeVirtual(cmp->cls, "compare", "([B[B)I", cmp, args, 3, res);
                if (ok && res.i == 1) { ids[j] = ids[j - 1]; j--; } // ids[j-1] SUIT cur : cur passe devant
                else break;
            }
            ids[j] = cur;
        }
    }
    Obj *arr = ctx->rt->heap().newArray(ObjKind::IntArray, static_cast<int>(ids.size()));
    if (arr)
        for (size_t i = 0; i < ids.size(); i++) arr->cells[i] = Value::fromInt(ids[i]);
    e->cells[RE_IDS] = Value::fromRef(arr);
    e->cells[RE_POS] = Value::fromInt(0);
}
void n_RS_enumerate(NativeContext *ctx)
{
    if (!rsOpen(ctx)) return;
    ClassInfo *c = clsOf(ctx, "javax/microedition/rms/RecordEnumerationImpl");
    Obj *e = c ? ctx->rt->heap().newInstance(c) : nullptr;
    if (e && e->cells && e->cellCount >= 6)
    {
        Obj *filter = argRef(ctx, 1), *cmp = argRef(ctx, 2);
        e->cells[RE_STORE] = ctx->thisObj->cells[RS_NAME];
        e->cells[RE_KEEP] = Value::fromInt(argInt(ctx, 3));
        e->cells[RE_FILTER] = Value::fromRef(filter);
        e->cells[RE_CMP] = Value::fromRef(cmp);
        reBuild(ctx, e, rsName(ctx), filter, cmp);
    }
    setRefResult(ctx, e);
}
int reCount(Obj *e) { return (e && e->cellCount >= 6 && e->cells[RE_IDS].o) ? e->cells[RE_IDS].o->arrayLen : 0; }
int rePos(Obj *e) { return (e && e->cellCount >= 6) ? e->cells[RE_POS].i : 0; }
const std::vector<uint8_t> *reRecord(Obj *e, int id)
{
    Obj *n = (e && e->cellCount >= 6) ? e->cells[RE_STORE].o : nullptr;
    if (!n || n->kind != ObjKind::String) return nullptr;
    RsStore &st = rsGet(n->str);
    auto it = st.recs.find(id);
    return it == st.recs.end() ? nullptr : &it->second;
}
void n_RE_hasNext(NativeContext *ctx) { setIntResult(ctx, rePos(ctx->thisObj) < reCount(ctx->thisObj) ? 1 : 0); }
void n_RE_hasPrev(NativeContext *ctx) { setIntResult(ctx, rePos(ctx->thisObj) > 0 ? 1 : 0); }
void n_RE_nextId(NativeContext *ctx)
{
    Obj *e = ctx->thisObj;
    int p = rePos(e);
    if (p >= reCount(e)) { throwJava(ctx, "javax/microedition/rms/InvalidRecordIDException"); return; }
    e->cells[RE_POS] = Value::fromInt(p + 1);
    setIntResult(ctx, e->cells[RE_IDS].o->cells[p].i);
}
void n_RE_prevId(NativeContext *ctx)
{
    Obj *e = ctx->thisObj;
    int p = rePos(e) - 1;
    if (p < 0 || p >= reCount(e)) { throwJava(ctx, "javax/microedition/rms/InvalidRecordIDException"); return; }
    e->cells[RE_POS] = Value::fromInt(p);
    setIntResult(ctx, e->cells[RE_IDS].o->cells[p].i);
}
void reBytes(NativeContext *ctx, int id)
{
    const std::vector<uint8_t> *rec = reRecord(ctx->thisObj, id);
    if (!rec) { throwJava(ctx, "javax/microedition/rms/InvalidRecordIDException"); return; }
    setRefResult(ctx, rsBytes(ctx, *rec));
}
void n_RE_next(NativeContext *ctx)
{
    Obj *e = ctx->thisObj;
    int p = rePos(e);
    if (p >= reCount(e)) { throwJava(ctx, "javax/microedition/rms/InvalidRecordIDException"); return; }
    e->cells[RE_POS] = Value::fromInt(p + 1);
    reBytes(ctx, e->cells[RE_IDS].o->cells[p].i);
}
void n_RE_prev(NativeContext *ctx)
{
    Obj *e = ctx->thisObj;
    int p = rePos(e) - 1;
    if (p < 0 || p >= reCount(e)) { throwJava(ctx, "javax/microedition/rms/InvalidRecordIDException"); return; }
    e->cells[RE_POS] = Value::fromInt(p);
    reBytes(ctx, e->cells[RE_IDS].o->cells[p].i);
}
void n_RE_numRecords(NativeContext *ctx) { setIntResult(ctx, reCount(ctx->thisObj)); }
void n_RE_destroy(NativeContext *) { }
void n_RE_reset(NativeContext *ctx)
{
    if (ctx->thisObj && ctx->thisObj->cellCount >= 6) ctx->thisObj->cells[RE_POS] = Value::fromInt(0);
}
void n_RE_rebuild(NativeContext *ctx)
{
    Obj *e = ctx->thisObj;
    Obj *n = (e && e->cellCount >= 6) ? e->cells[RE_STORE].o : nullptr;
    if (n && n->kind == ObjKind::String) reBuild(ctx, e, n->str, e->cells[RE_FILTER].o, e->cells[RE_CMP].o);
}
void n_RE_isKept(NativeContext *ctx) { setIntResult(ctx, (ctx->thisObj && ctx->thisObj->cellCount >= 6) ? ctx->thisObj->cells[RE_KEEP].i : 0); }
void n_RE_keepUpdated(NativeContext *ctx) { if (ctx->thisObj && ctx->thisObj->cellCount >= 6) ctx->thisObj->cells[RE_KEEP] = Value::fromInt(argInt(ctx, 1)); }

// --- java.util.Hashtable ---
int htFieldOff(NativeContext *ctx, const char *cls, const char *fld)
{
    ClassInfo *c = clsOf(ctx, cls);
    if (!c) return -1;
    const MethodRecord *m = c->findFieldRecursive(fld);
    return m ? m->slot : -1;
}
Obj *htTable(NativeContext *ctx)
{
    int off = htFieldOff(ctx, "java/util/Hashtable", "table");
    return (off >= 0 && ctx->thisObj && ctx->thisObj->cells) ? ctx->thisObj->cells[off].o : nullptr;
}
int htCount(NativeContext *ctx)
{
    int off = htFieldOff(ctx, "java/util/Hashtable", "count");
    return (off >= 0 && ctx->thisObj && ctx->thisObj->cells) ? ctx->thisObj->cells[off].i : 0;
}
void htSetCount(NativeContext *ctx, int v)
{
    int off = htFieldOff(ctx, "java/util/Hashtable", "count");
    if (off >= 0 && ctx->thisObj && ctx->thisObj->cells)
        ctx->thisObj->cells[off] = Value::fromInt(v);
}
bool keyEq(Obj *a, Obj *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind == ObjKind::String && b->kind == ObjKind::String) return a->str == b->str;
    if (a->kind == ObjKind::Instance && b->kind == ObjKind::Instance && a->cls && b->cls &&
        a->cls->name == "java/lang/Integer" && b->cls->name == "java/lang/Integer")
        return a->cells[0].i == b->cells[0].i;
    return false;
}
void n_HT_init(NativeContext *ctx)
{
    int off = htFieldOff(ctx, "java/util/Hashtable", "table");
    if (off >= 0 && ctx->thisObj && ctx->thisObj->cells)
        ctx->thisObj->cells[off] = Value::fromRef(ctx->rt->heap().newArray(ObjKind::ObjArray, 8));
}
void n_HT_get(NativeContext *ctx)
{
    Obj *t = htTable(ctx);
    Obj *key = argRef(ctx, 1);
    if (t && key)
    {
        int cap = t->arrayLen / 2;
        for (int i = 0; i < cap; i++)
            if (keyEq(t->cells[i * 2].o, key)) { setRefResult(ctx, t->cells[i * 2 + 1].o); return; }
    }
    setRefResult(ctx, nullptr);
}
void n_HT_put(NativeContext *ctx)
{
    Obj *key = argRef(ctx, 1);
    Obj *val = argRef(ctx, 2);
    if (!key) { setRefResult(ctx, nullptr); return; }
    Obj *t = htTable(ctx);
    if (!t)
    {
        n_HT_init(ctx);
        t = htTable(ctx);
        if (!t) { setRefResult(ctx, nullptr); return; }
    }
    int cap = t->arrayLen / 2;
    for (int i = 0; i < cap; i++)
        if (keyEq(t->cells[i * 2].o, key))
        {
            Obj *old = t->cells[i * 2 + 1].o;
            t->cells[i * 2 + 1] = Value::fromRef(val);
            setRefResult(ctx, old);
            return;
        }
    int count = htCount(ctx);
    if (count >= cap)
    {
        int nn = cap ? cap * 2 : 4;
        Obj *nt = ctx->rt->heap().newArray(ObjKind::ObjArray, nn * 2);
        for (int i = 0; i < cap; i++)
        {
            nt->cells[i * 2] = t->cells[i * 2];
            nt->cells[i * 2 + 1] = t->cells[i * 2 + 1];
        }
        int off = htFieldOff(ctx, "java/util/Hashtable", "table");
        if (off >= 0 && ctx->thisObj && ctx->thisObj->cells)
            ctx->thisObj->cells[off] = Value::fromRef(nt);
        t = nt;
    }
    t->cells[count * 2] = Value::fromRef(key);
    t->cells[count * 2 + 1] = Value::fromRef(val);
    htSetCount(ctx, count + 1);
    setRefResult(ctx, nullptr);
}
void n_HT_remove(NativeContext *ctx)
{
    Obj *t = htTable(ctx);
    Obj *key = argRef(ctx, 1);
    int count = htCount(ctx);
    if (t && key)
    {
        int cap = t->arrayLen / 2;
        for (int i = 0; i < cap; i++)
            if (keyEq(t->cells[i * 2].o, key))
            {
                Obj *old = t->cells[i * 2 + 1].o;
                for (int j = i; j < count - 1; j++)
                {
                    t->cells[j * 2] = t->cells[(j + 1) * 2];
                    t->cells[j * 2 + 1] = t->cells[(j + 1) * 2 + 1];
                }
                if (count > 0)
                {
                    t->cells[(count - 1) * 2] = Value::fromRef(nullptr);
                    t->cells[(count - 1) * 2 + 1] = Value::fromRef(nullptr);
                }
                htSetCount(ctx, count - 1);
                setRefResult(ctx, old);
                return;
            }
    }
    setRefResult(ctx, nullptr);
}
void n_HT_containsKey(NativeContext *ctx)
{
    Obj *t = htTable(ctx);
    Obj *key = argRef(ctx, 1);
    if (t && key)
        for (int i = 0; i < t->arrayLen / 2; i++)
            if (keyEq(t->cells[i * 2].o, key)) { setIntResult(ctx, 1); return; }
    setIntResult(ctx, 0);
}
void n_HT_clear(NativeContext *ctx)
{
    int off = htFieldOff(ctx, "java/util/Hashtable", "table");
    if (off >= 0 && ctx->thisObj && ctx->thisObj->cells)
        ctx->thisObj->cells[off] = Value::fromRef(ctx->rt->heap().newArray(ObjKind::ObjArray, 8));
    htSetCount(ctx, 0);
}
void n_HT_size(NativeContext *ctx) { setIntResult(ctx, htCount(ctx)); }
void n_HT_isEmpty(NativeContext *ctx) { setIntResult(ctx, htCount(ctx) == 0 ? 1 : 0); }

// --- java.util.Vector ---
Obj *vecData(NativeContext *ctx)
{
    int off = htFieldOff(ctx, "java/util/Vector", "elementData");
    return (off >= 0 && ctx->thisObj && ctx->thisObj->cells) ? ctx->thisObj->cells[off].o : nullptr;
}
int vecCount(NativeContext *ctx)
{
    int off = htFieldOff(ctx, "java/util/Vector", "elementCount");
    return (off >= 0 && ctx->thisObj && ctx->thisObj->cells) ? ctx->thisObj->cells[off].i : 0;
}
void vecSetCount(NativeContext *ctx, int v)
{
    int off = htFieldOff(ctx, "java/util/Vector", "elementCount");
    if (off >= 0 && ctx->thisObj && ctx->thisObj->cells)
        ctx->thisObj->cells[off] = Value::fromInt(v);
}
void vecSetData(NativeContext *ctx, Obj *arr)
{
    int off = htFieldOff(ctx, "java/util/Vector", "elementData");
    if (off >= 0 && ctx->thisObj && ctx->thisObj->cells)
        ctx->thisObj->cells[off] = Value::fromRef(arr);
}
void vecEnsureCapacity(NativeContext *ctx, int minCap)
{
    Obj *d = vecData(ctx);
    int cap = d ? d->arrayLen : 0;
    if (cap >= minCap) return;
    int nn = cap ? cap * 2 : 10;
    if (nn < minCap) nn = minCap;
    Obj *nd = ctx->rt->heap().newArray(ObjKind::ObjArray, nn);
    int count = vecCount(ctx);
    for (int i = 0; i < count && d; i++) nd->cells[i] = d->cells[i];
    vecSetData(ctx, nd);
}
void n_Vec_init0(NativeContext *ctx)
{
    vecSetData(ctx, ctx->rt->heap().newArray(ObjKind::ObjArray, 10));
    vecSetCount(ctx, 0);
}
void n_Vec_initCap(NativeContext *ctx)
{
    int cap = argInt(ctx, 1);
    vecSetData(ctx, ctx->rt->heap().newArray(ObjKind::ObjArray, cap > 0 ? cap : 1));
    vecSetCount(ctx, 0);
}
void n_Vec_addElement(NativeContext *ctx)
{
    int count = vecCount(ctx);
    vecEnsureCapacity(ctx, count + 1);
    Obj *d = vecData(ctx);
    d->cells[count] = Value::fromRef(argRef(ctx, 1));
    vecSetCount(ctx, count + 1);
}
void n_Vec_elementAt(NativeContext *ctx)
{
    int idx = argInt(ctx, 1);
    Obj *d = vecData(ctx);
    int count = vecCount(ctx);
    if (!d || idx < 0 || idx >= count)
    {
        throwJava(ctx, "java/lang/ArrayIndexOutOfBoundsException");
        return;
    }
    setRefResult(ctx, d->cells[idx].o);
}
void n_Vec_setElementAt(NativeContext *ctx)
{
    Obj *val = argRef(ctx, 1);
    int idx = argInt(ctx, 2);
    Obj *d = vecData(ctx);
    int count = vecCount(ctx);
    if (d && idx >= 0 && idx < count) d->cells[idx] = Value::fromRef(val);
}
void n_Vec_insertElementAt(NativeContext *ctx)
{
    Obj *val = argRef(ctx, 1);
    int idx = argInt(ctx, 2);
    int count = vecCount(ctx);
    if (idx < 0 || idx > count) return;
    vecEnsureCapacity(ctx, count + 1);
    Obj *d = vecData(ctx);
    for (int i = count; i > idx; i--) d->cells[i] = d->cells[i - 1];
    d->cells[idx] = Value::fromRef(val);
    vecSetCount(ctx, count + 1);
}
void n_Vec_removeElementAt(NativeContext *ctx)
{
    int idx = argInt(ctx, 1);
    Obj *d = vecData(ctx);
    int count = vecCount(ctx);
    if (!d || idx < 0 || idx >= count) return;
    for (int i = idx; i < count - 1; i++) d->cells[i] = d->cells[i + 1];
    d->cells[count - 1] = Value::fromRef(nullptr);
    vecSetCount(ctx, count - 1);
}
void n_Vec_removeElement(NativeContext *ctx)
{
    Obj *val = argRef(ctx, 1);
    Obj *d = vecData(ctx);
    int count = vecCount(ctx);
    if (d)
        for (int i = 0; i < count; i++)
            if (keyEq(d->cells[i].o, val))
            {
                for (int j = i; j < count - 1; j++) d->cells[j] = d->cells[j + 1];
                d->cells[count - 1] = Value::fromRef(nullptr);
                vecSetCount(ctx, count - 1);
                setIntResult(ctx, 1);
                return;
            }
    setIntResult(ctx, 0);
}
void n_Vec_removeAllElements(NativeContext *ctx)
{
    Obj *d = vecData(ctx);
    int count = vecCount(ctx);
    if (d) for (int i = 0; i < count; i++) d->cells[i] = Value::fromRef(nullptr);
    vecSetCount(ctx, 0);
}
void n_Vec_size(NativeContext *ctx) { setIntResult(ctx, vecCount(ctx)); }
void n_Vec_isEmpty(NativeContext *ctx) { setIntResult(ctx, vecCount(ctx) == 0 ? 1 : 0); }
void n_Vec_contains(NativeContext *ctx)
{
    Obj *val = argRef(ctx, 1);
    Obj *d = vecData(ctx);
    int count = vecCount(ctx);
    if (d) for (int i = 0; i < count; i++) if (keyEq(d->cells[i].o, val)) { setIntResult(ctx, 1); return; }
    setIntResult(ctx, 0);
}
void n_Vec_indexOf(NativeContext *ctx)
{
    Obj *val = argRef(ctx, 1);
    Obj *d = vecData(ctx);
    int count = vecCount(ctx);
    if (d) for (int i = 0; i < count; i++) if (keyEq(d->cells[i].o, val)) { setIntResult(ctx, i); return; }
    setIntResult(ctx, -1);
}
void n_Vec_firstElement(NativeContext *ctx)
{
    Obj *d = vecData(ctx);
    int count = vecCount(ctx);
    if (!d || count <= 0) { throwJava(ctx, "java/util/NoSuchElementException"); return; }
    setRefResult(ctx, d->cells[0].o);
}
void n_Vec_lastElement(NativeContext *ctx)
{
    Obj *d = vecData(ctx);
    int count = vecCount(ctx);
    if (!d || count <= 0) { throwJava(ctx, "java/util/NoSuchElementException"); return; }
    setRefResult(ctx, d->cells[count - 1].o);
}

// --- java.util.Random (LCG 63 bits) ---
uint64_t rndUpdate(Obj *r)
{
    uint64_t s = r ? r->cells[0].l : 0;
    s = (s * 6364136223846793005ULL + 1442695040888963407ULL) & 0x7FFFFFFFFFFFFFFFULL;
    if (r) r->cells[0] = Value::fromLong(static_cast<int64_t>(s));
    return s;
}
void n_Random_init(NativeContext *ctx)
{
    uint64_t s = static_cast<uint64_t>(argLong(ctx, 1)) & 0xFFFFFFFFULL;
    if (ctx->thisObj) ctx->thisObj->cells[0] = Value::fromLong(static_cast<int64_t>(s ? s : 0x4d595df4d0f33173ULL));
}
void n_Random_init0(NativeContext *ctx)
{
    if (ctx->thisObj) ctx->thisObj->cells[0] = Value::fromLong(0x4d595df4d0f33173ULL);
}
void n_Random_setSeed(NativeContext *ctx)
{
    if (ctx->thisObj) ctx->thisObj->cells[0] = Value::fromLong(argLong(ctx, 1));
}
void n_Random_nextInt(NativeContext *ctx)
{
    uint64_t s = rndUpdate(ctx->thisObj);
    setIntResult(ctx, static_cast<int32_t>((s >> 32) & 0x7FFFFFFF));
}
void n_Random_nextIntBound(NativeContext *ctx)
{
    uint64_t s = rndUpdate(ctx->thisObj);
    int b = argInt(ctx, 1);
    if (b <= 0) b = 1;
    setIntResult(ctx, static_cast<int32_t>(((s >> 32) & 0x7FFFFFFF) % b));
}
void n_Random_nextLong(NativeContext *ctx)
{
    uint64_t s = rndUpdate(ctx->thisObj);
    setLongResult(ctx, static_cast<int64_t>(s));
}
void n_Random_nextDouble(NativeContext *ctx)
{
    uint64_t s = rndUpdate(ctx->thisObj);
    double d = static_cast<double>((s >> 11) & 0x3fffffffffffffULL) / 0x40000000000000ULL;
    setLongResult(ctx, *reinterpret_cast<int64_t *>(&d));
}
void n_Random_nextFloat(NativeContext *ctx)
{
    uint64_t s = rndUpdate(ctx->thisObj);
    Value v;
    v.f = static_cast<float>((s >> 33) & 0xFFFFFF) / static_cast<float>(0x1000000);
    if (ctx->result) *ctx->result = v;
}
void n_Random_nextBoolean(NativeContext *ctx)
{
    setIntResult(ctx, (rndUpdate(ctx->thisObj) >> 63) ? 1 : 0);
}

void n_System_currentTimeMillis(NativeContext *ctx)
{
    setLongResult(ctx, virtualMillis());
}
void n_System_arraycopy(NativeContext *ctx)
{
    Obj *src = argRef(ctx, 0);
    int spos = argInt(ctx, 1);
    Obj *dst = argRef(ctx, 2);
    int dpos = argInt(ctx, 3);
    int n = argInt(ctx, 4);
    if (!src || !dst)
        return;
    for (int i = 0; i < n; i++)
    {
        if (spos + i >= src->arrayLen || dpos + i >= dst->arrayLen)
            break;
        dst->cells[dpos + i] = src->cells[spos + i];
    }
}
void n_System_getProperty(NativeContext *ctx)
{
    Obj *keyObj = argRef(ctx, 0);
    std::string key = (keyObj && keyObj->kind == ObjKind::String) ? keyObj->str : "";
    // Propriétés CLDC/MIDP standard connues. Pour le reste (souvent utilisé
    // par les jeux pour détecter un modèle de téléphone précis via
    // "microedition.platform".startsWith("NokiaXXXX") et choisir un mapping
    // de touches propriétaire), on renvoie "" plutôt que de prétendre être
    // un appareil Nokia/Siemens/etc. spécifique : notre hal::input.cpp émule
    // un clavier numérique + softkeys standard, pas un layout propriétaire.
    std::string val;
    bool known = true;
    if (key == "microedition.configuration") val = "CLDC-1.1";
    else if (key == "microedition.profiles") val = "MIDP-2.0";
    else if (key == "microedition.locale") val = "en-US";
    else if (key == "microedition.encoding") val = "ISO-8859-1";
    else if (key == "microedition.platform") val = ""; // jamais null : les jeux font `.indexOf/.startsWith` dessus
    else if (key == "microedition.media.version") val = "1.1";
    else if (key == "supports.mixing") val = "true";
    else if (key == "supports.audio.capture" || key == "supports.video.capture" || key == "supports.recording") val = "false";
    else if (key == "file.separator") val = "/";
    else known = false;
    // Propriété inconnue : `null` (spec CLDC) -- p.ex. `fileconn.dir.*`, `microedition.hostname`,
    // `microedition.io.file.FileConnection.version` : les jeux testent null pour détecter l'absence
    // de la fonctionnalité (nmania faisait `.charAt(0)` sur la chaîne vide qu'on renvoyait).
    if (jvm::jmeDebug())
        fprintf(stderr, "[cldc] System.getProperty(\"%s\") -> %s%s%s\n", key.c_str(), known ? "\"" : "null", known ? val.c_str() : "", known ? "\"" : "");
    setRefResult(ctx, known ? ctx->rt->heap().newString(val) : nullptr);
}
void n_System_gc(NativeContext *) {}
void n_System_identityHashCode(NativeContext *ctx)
{
    setIntResult(ctx, static_cast<int32_t>(reinterpret_cast<uintptr_t>(argRef(ctx, 0))));
}
// --- java.lang.Runtime (utilisé par ex. games/prince_of_persia_th pour le suivi mémoire) ---
void n_Runtime_getRuntime(NativeContext *ctx)
{
    ClassInfo *c = clsOf(ctx, "java/lang/Runtime");
    setRefResult(ctx, c ? ctx->rt->heap().newInstance(c) : nullptr);
}
void n_Runtime_totalMemory(NativeContext *ctx)
{
    (void)ctx;
    setLongResult(ctx, static_cast<int64_t>(ctx->rt->heap().capacity()));
}
void n_Runtime_freeMemory(NativeContext *ctx)
{
    (void)ctx;
    size_t cap = ctx->rt->heap().capacity(), used = ctx->rt->heap().used();
    setLongResult(ctx, static_cast<int64_t>(cap > used ? cap - used : 0));
}
void n_Runtime_maxMemory(NativeContext *ctx)
{
    (void)ctx;
    setLongResult(ctx, static_cast<int64_t>(ctx->rt->heap().capacity()));
}
void n_PrintStream_println(NativeContext *ctx)
{
    Obj *s = argRef(ctx, 1);
    if (s && s->kind == ObjKind::String)
        printf("%s\n", s->str.c_str());
    else
        printf("\n");
}
void n_PrintStream_print(NativeContext *ctx)
{
    Obj *s = argRef(ctx, 1);
    if (s && s->kind == ObjKind::String)
        printf("%s", s->str.c_str());
}
void n_PrintStream_printInt(NativeContext *ctx)
{
    printf("%d", argInt(ctx, 1));
}
void n_PrintStream_printlnInt(NativeContext *ctx)
{
    printf("%d\n", argInt(ctx, 1));
}
void n_PrintStream_printlnObj(NativeContext *ctx)
{
    Obj *o = argRef(ctx, 1);
    if (o && o->kind == ObjKind::String) printf("%s\n", o->str.c_str());
    else printf("%p\n", (void *)o);
}
void n_PrintStream_flush(NativeContext *ctx)
{
    (void)ctx;
    fflush(stdout);
}

void n_Class_getName(NativeContext *ctx)
{
    Obj *co = argRef(ctx, 0);
    if (!co) { setRefResult(ctx, ctx->rt->heap().newString("")); return; }
    std::string n = co->str;
    if (n.empty() && co->cls) n = co->cls->name;
    for (auto &ch : n) if (ch == '/') ch = '.';
    setRefResult(ctx, ctx->rt->heap().newString(n));
}
// Class.forName(name) : classe du JAR ou native ; ClassNotFoundException sinon.
void n_Class_forName(NativeContext *ctx)
{
    Obj *nm = argRef(ctx, 0);
    if (!nm || nm->kind != ObjKind::String) { throwJava(ctx, "java/lang/NullPointerException"); return; }
    std::string n = nm->str;
    for (char &c : n) if (c == '.') c = '/';
    ClassInfo *ci = ctx->rt->classInfoOfName(n);
    if (!ci) ci = ctx->rt->loadFromJar(n);
    if (!ci) { throwJava(ctx, "java/lang/ClassNotFoundException"); return; }
    setRefResult(ctx, ctx->rt->heap().classObjFor(n));
}
// Class.newInstance() : instance + constructeur sans argument (bytecode).
void n_Class_newInstance(NativeContext *ctx)
{
    Obj *co = ctx->thisObj;
    if (!co || co->kind != ObjKind::Class) { setRefResult(ctx, nullptr); return; }
    ClassInfo *ci = ctx->rt->classInfoOfName(co->str);
    if (!ci) ci = ctx->rt->loadFromJar(co->str);
    if (!ci) { throwJava(ctx, "java/lang/InstantiationException"); return; }
    Obj *o = ctx->rt->heap().newInstance(ci);
    if (!o) { setRefResult(ctx, nullptr); return; }
    ctx->interp->ensureInit(ci);
    Value a[1] = {Value::fromRef(o)}, r;
    if (!ctx->interp->invokeSpecial(ci, "<init>", "()V", o, a, 1, r))
    {
        throwJava(ctx, "java/lang/InstantiationException");
        return;
    }
    setRefResult(ctx, o);
}

// ---- compléments CLDC 1.1 (String/StringBuffer/Math/Integer/Long/Character/Float/Double/Vector/Hashtable/Stack/Date...)
#include "cldc/natives_extra.inc"

} // namespace

// Horloge du JEU (currentTimeMillis, Timer, Thread.sleep...) en microsecondes. Elle avance à chaque trame de la
// durée RÉELLE écoulée (midp::tick), ou d'un pas fixe si JME_FRAME_TIME est posé (runs déterministes).
static int64_t g_virtualUs = 0;
int64_t virtualMicros() { return g_virtualUs; }
void setVirtualMicros(int64_t us) { if (us > g_virtualUs) g_virtualUs = us; }
int64_t virtualMillis() { return g_virtualUs / 1000; }
void advanceVirtualMillis(int64_t ms) { g_virtualUs += ms * 1000; }

void initNatives()
{
    using namespace std::placeholders;

    // java.lang.Object
    registerNative("java/lang/Object.<init>:()V", n_Object_init);
    registerNative("java/lang/Object.getClass:()Ljava/lang/Class;", n_Object_getClass);
    registerNative("java/lang/Class.newInstance:()Ljava/lang/Object;", n_Class_newInstance);
    registerNative("java/lang/Object.equals:(Ljava/lang/Object;)Z", n_Object_equals);
    registerNative("java/lang/Object.hashCode:()I", n_Object_hashCode);
    registerNative("java/lang/Object.toString:()Ljava/lang/String;", n_Object_toString);
    registerNative("java/lang/Object.wait:()V", n_Object_wait);
    registerNative("java/lang/Object.wait:(I)V", n_Object_waitI);
    registerNative("java/lang/Object.wait:(J)V", n_Object_waitL);
    registerNative("java/lang/Object.notify:()V", n_Object_notify);
    registerNative("java/lang/Object.notifyAll:()V", n_Object_notifyAll);

    // java.lang.Throwable (racine de toute la hiérarchie Exception/Error)
    registerNative("java/lang/Throwable.<init>:()V", n_Throwable_init);
    registerNative("java/lang/Throwable.<init>:(Ljava/lang/String;)V", n_Throwable_initMsg);
    registerNative("java/lang/Throwable.getMessage:()Ljava/lang/String;", n_Throwable_getMessage);
    registerNative("java/lang/Throwable.toString:()Ljava/lang/String;", n_Throwable_toString);
    registerNative("java/lang/Throwable.printStackTrace:()V", n_Throwable_printStackTrace);

    // java.lang.String
    registerNative("java/lang/String.<init>:()V", n_Object_init);
    registerNative("java/lang/String.<init>:([BLjava/lang/String;)V", n_String_initBytes);
    registerNative("java/lang/String.<init>:([B)V", n_String_initBytes);
    registerNative("java/lang/String.<init>:([BII)V", n_String_initBytesRange);
    registerNative("java/lang/String.<init>:([BIILjava/lang/String;)V", n_String_initBytesRange);
    registerNative("java/lang/String.<init>:([CII)V", n_String_initCharsRange);
    registerNative("java/lang/String.length:()I", n_String_length);
    registerNative("java/lang/String.charAt:(I)C", n_String_charAt);
    registerNative("java/lang/String.toCharArray:()[C", n_String_toCharArray);
    registerNative("java/lang/String.concat:(Ljava/lang/String;)Ljava/lang/String;", n_String_concat);
    registerNative("java/lang/String.equals:(Ljava/lang/Object;)Z", n_String_equals);
    registerNative("java/lang/String.equalsIgnoreCase:(Ljava/lang/String;)Z", n_String_equalsIgnoreCase);
    registerNative("java/lang/String.substring:(I)Ljava/lang/String;", n_String_substring1);
    registerNative("java/lang/String.substring:(II)Ljava/lang/String;", n_String_substring2);
    registerNative("java/lang/String.indexOf:(Ljava/lang/String;)I", n_String_indexOf);
    registerNative("java/lang/String.indexOf:(Ljava/lang/String;I)I", n_String_indexOfStrFrom);
    registerNative("java/lang/String.indexOf:(I)I", n_String_indexOfChar);
    registerNative("java/lang/String.indexOf:(II)I", n_String_indexOfCharFrom);
    registerNative("java/lang/String.trim:()Ljava/lang/String;", n_String_trim);
    registerNative("java/lang/String.toLowerCase:()Ljava/lang/String;", n_String_toLowerCase);
    registerNative("java/lang/String.toUpperCase:()Ljava/lang/String;", n_String_toUpperCase);
    registerNative("java/lang/String.compareTo:(Ljava/lang/String;)I", n_String_compareTo);
    registerNative("java/lang/String.startsWith:(Ljava/lang/String;)Z", n_String_startsWith);
    registerNative("java/lang/String.endsWith:(Ljava/lang/String;)Z", n_String_endsWith);
    registerNative("java/lang/String.regionMatches:(ILjava/lang/String;II)Z", n_String_regionMatches);
    // String.valueOf(int) est statique (pas de thisObj), même convention
    // d'argument (args[0] = l'entier) qu'Integer.toString(int) -- même
    // implémentation réutilisée telle quelle.
    registerNative("java/lang/String.valueOf:(I)Ljava/lang/String;", n_Integer_toStringS);
    registerNative("java/lang/String.intern:()Ljava/lang/String;", n_String_intern);

    // java.lang.Math
    registerNative("java/lang/Math.abs:(I)I", n_Math_abs);
    registerNative("java/lang/Math.abs:(J)J", n_Math_absL);
    registerNative("java/lang/Math.min:(II)I", n_Math_min);
    registerNative("java/lang/Math.max:(II)I", n_Math_max);
    registerNative("java/lang/Math.sqrt:(D)D", n_Math_sqrt);
    registerNative("java/lang/Math.floor:(D)D", n_Math_floor);
    registerNative("java/lang/Math.ceil:(D)D", n_Math_ceil);
    registerNative("java/lang/Math.round:(D)J", n_Math_round);
    registerNative("java/lang/Math.pow:(DD)D", n_Math_pow);
    registerNative("java/lang/Math.random:()D", n_Math_random);
    registerNative("java/lang/Math.min:(JJ)J", n_Math_minL);
    registerNative("java/lang/Math.max:(JJ)J", n_Math_maxL);

    // java.lang.Integer
    registerNative("java/lang/Boolean.<init>:(Z)V", n_Boolean_init);
    registerNative("java/lang/Boolean.booleanValue:()Z", n_Boolean_booleanValue);
    registerNative("java/lang/Boolean.toString:()Ljava/lang/String;", n_Boolean_toString);
    registerNative("java/lang/Boolean.hashCode:()I", n_Boolean_hashCode);
    registerNative("java/lang/Boolean.equals:(Ljava/lang/Object;)Z", n_Boolean_equals);
    registerNative("java/lang/Boolean.valueOf:(Z)Ljava/lang/Boolean;", n_Boolean_valueOf);
    registerNative("java/lang/Integer.<init>:(I)V", n_Integer_init);
    registerNative("java/lang/Integer.intValue:()I", n_Integer_intValue);
    registerNative("java/lang/Integer.byteValue:()B", n_Integer_byteValue);
    registerNative("java/lang/Integer.shortValue:()S", n_Integer_shortValue);
    registerNative("java/lang/Integer.longValue:()J", n_Integer_longValue);
    registerNative("java/lang/Integer.hashCode:()I", n_Integer_hashCode);
    registerNative("java/lang/Integer.toString:()Ljava/lang/String;", n_Integer_toString);
    registerNative("java/lang/Integer.toString:(I)Ljava/lang/String;", n_Integer_toStringS);
    registerNative("java/lang/Integer.valueOf:(I)Ljava/lang/Integer;", n_Integer_valueOf);
    registerNative("java/lang/Integer.parseInt:(Ljava/lang/String;)I", n_Integer_parseInt);
    registerNative("java/lang/Integer.equals:(Ljava/lang/Object;)Z", n_Integer_equals);
    registerNative("java/lang/Integer.compareTo:(Ljava/lang/Integer;)I", n_Integer_compareTo);

    // java.lang.StringBuffer
    registerNative("java/lang/StringBuffer.<init>:()V", n_SB_init0);
    registerNative("java/lang/StringBuffer.<init>:(Ljava/lang/String;)V", n_SB_init);
    registerNative("java/lang/StringBuffer.<init>:(I)V", n_SB_initCap);
    registerNative("java/lang/StringBuffer.append:(Ljava/lang/String;)Ljava/lang/StringBuffer;", n_SB_appendS);
    registerNative("java/lang/StringBuffer.append:(I)Ljava/lang/StringBuffer;", n_SB_appendI);
    registerNative("java/lang/StringBuffer.append:(C)Ljava/lang/StringBuffer;", n_SB_appendC);
    registerNative("java/lang/StringBuffer.append:(J)Ljava/lang/StringBuffer;", n_SB_appendL);
    registerNative("java/lang/StringBuffer.append:(Z)Ljava/lang/StringBuffer;", n_SB_appendZ);
    registerNative("java/lang/StringBuffer.append:(Ljava/lang/Object;)Ljava/lang/StringBuffer;", n_SB_appendO);
    registerNative("java/lang/StringBuffer.append:(F)Ljava/lang/StringBuffer;", n_SB_appendF);
    registerNative("java/lang/StringBuffer.append:(D)Ljava/lang/StringBuffer;", n_SB_appendD);
    registerNative("java/lang/StringBuffer.toString:()Ljava/lang/String;", n_SB_toString);
    registerNative("java/lang/StringBuffer.length:()I", n_SB_length);
    registerNative("java/lang/StringBuffer.charAt:(I)C", n_SB_charAt);
    registerNative("java/lang/StringBuffer.setCharAt:(IC)V", n_SB_setCharAt);
    registerNative("java/lang/StringBuffer.setLength:(I)V", n_SB_setLength);
    registerNative("java/lang/StringBuffer.delete:(II)Ljava/lang/StringBuffer;", n_SB_delete);

    // java.lang.Thread
    registerNative("java/lang/Thread.<init>:(Ljava/lang/Runnable;)V", n_Thread_init);
    registerNative("java/lang/Thread.<init>:(Ljava/lang/Runnable;Ljava/lang/String;)V", n_Thread_init); // nom ignoré
    registerNative("java/lang/Thread.start:()V", n_Thread_start);
    registerNative("java/lang/Thread.run:()V", n_Thread_start);
    registerNative("java/lang/Thread.sleep:(J)V", n_Thread_sleep);
    registerNative("java/lang/Thread.yield:()V", n_Thread_yield);
    registerNative("java/lang/Thread.currentThread:()Ljava/lang/Thread;", n_Thread_currentThread);
    registerNative("java/lang/Thread.setPriority:(I)V", n_Thread_setPriority);
    registerNative("java/lang/Thread.interrupt:()V", n_Thread_interrupt);
    registerNative("java/lang/Thread.isAlive:()Z", n_Thread_isAlive);
    registerNative("java/lang/Thread.join:()V", n_Thread_join);

    // javax.microedition.rms.RecordStore
    registerNative("javax/microedition/rms/RecordStore.openRecordStore:(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;", n_RS_open);
    registerNative("javax/microedition/rms/RecordStore.openRecordStore:(Ljava/lang/String;ZIZ)Ljavax/microedition/rms/RecordStore;", n_RS_open4);
    registerNative("javax/microedition/rms/RecordStore.openRecordStore:(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljavax/microedition/rms/RecordStore;", n_RS_openVendor);
    registerNative("javax/microedition/rms/RecordStore.getNumRecords:()I", n_RS_getNumRecords);
    registerNative("javax/microedition/rms/RecordStore.getName:()Ljava/lang/String;", n_RS_getName);
    registerNative("javax/microedition/rms/RecordStore.getVersion:()I", n_RS_getVersion);
    registerNative("javax/microedition/rms/RecordStore.getSize:()I", n_RS_getSize);
    registerNative("javax/microedition/rms/RecordStore.getSizeAvailable:()I", n_RS_getSizeAvailable);
    registerNative("javax/microedition/rms/RecordStore.getLastModified:()J", n_RS_getLastModified);
    registerNative("javax/microedition/rms/RecordStore.getNextRecordID:()I", n_RS_getNextRecordID);
    registerNative("javax/microedition/rms/RecordStore.getRecord:(I[BI)I", n_RS_getRecord);
    registerNative("javax/microedition/rms/RecordStore.getRecord:(I)[B", n_RS_getRecordBytes);
    registerNative("javax/microedition/rms/RecordStore.getRecordSize:(I)I", n_RS_getRecordSize);
    registerNative("javax/microedition/rms/RecordStore.setRecord:(I[BII)V", n_RS_setRecord);
    registerNative("javax/microedition/rms/RecordStore.addRecord:([BII)I", n_RS_addRecord);
    registerNative("javax/microedition/rms/RecordStore.deleteRecord:(I)V", n_RS_deleteRecord);
    registerNative("javax/microedition/rms/RecordStore.closeRecordStore:()V", n_RS_close);
    registerNative("javax/microedition/rms/RecordStore.deleteRecordStore:(Ljava/lang/String;)V", n_RS_deleteStore);
    registerNative("javax/microedition/rms/RecordStore.listRecordStores:()[Ljava/lang/String;", n_RS_list);
    registerNative("javax/microedition/rms/RecordStore.addRecordListener:(Ljavax/microedition/rms/RecordListener;)V", n_RS_noop);
    registerNative("javax/microedition/rms/RecordStore.removeRecordListener:(Ljavax/microedition/rms/RecordListener;)V", n_RS_noop);
    registerNative("javax/microedition/rms/RecordStore.setMode:(IZ)V", n_RS_noop);
    registerNative("javax/microedition/rms/RecordStore.enumerateRecords:(Ljavax/microedition/rms/RecordFilter;Ljavax/microedition/rms/RecordComparator;Z)Ljavax/microedition/rms/RecordEnumeration;", n_RS_enumerate);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.hasNextElement:()Z", n_RE_hasNext);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.hasPreviousElement:()Z", n_RE_hasPrev);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.nextRecordId:()I", n_RE_nextId);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.previousRecordId:()I", n_RE_prevId);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.nextRecord:()[B", n_RE_next);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.previousRecord:()[B", n_RE_prev);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.numRecords:()I", n_RE_numRecords);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.destroy:()V", n_RE_destroy);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.reset:()V", n_RE_reset);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.rebuild:()V", n_RE_rebuild);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.isKeptUpdated:()Z", n_RE_isKept);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.keepUpdated:(Z)V", n_RE_keepUpdated);

    // java.util.Hashtable
    registerNative("java/util/Hashtable.<init>:()V", n_HT_init);
    registerNative("java/util/Hashtable.get:(Ljava/lang/Object;)Ljava/lang/Object;", n_HT_get);
    registerNative("java/util/Hashtable.put:(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", n_HT_put);
    registerNative("java/util/Hashtable.remove:(Ljava/lang/Object;)Ljava/lang/Object;", n_HT_remove);
    registerNative("java/util/Hashtable.containsKey:(Ljava/lang/Object;)Z", n_HT_containsKey);
    registerNative("java/util/Hashtable.clear:()V", n_HT_clear);
    registerNative("java/util/Hashtable.size:()I", n_HT_size);
    registerNative("java/util/Hashtable.isEmpty:()Z", n_HT_isEmpty);

    // java.util.Vector
    registerNative("java/util/Vector.<init>:()V", n_Vec_init0);
    registerNative("java/util/Vector.<init>:(I)V", n_Vec_initCap);
    registerNative("java/util/Vector.<init>:(II)V", n_Vec_initCap);
    registerNative("java/util/Vector.addElement:(Ljava/lang/Object;)V", n_Vec_addElement);
    registerNative("java/util/Vector.elementAt:(I)Ljava/lang/Object;", n_Vec_elementAt);
    registerNative("java/util/Vector.setElementAt:(Ljava/lang/Object;I)V", n_Vec_setElementAt);
    registerNative("java/util/Vector.insertElementAt:(Ljava/lang/Object;I)V", n_Vec_insertElementAt);
    registerNative("java/util/Vector.removeElement:(Ljava/lang/Object;)Z", n_Vec_removeElement);
    registerNative("java/util/Vector.removeElementAt:(I)V", n_Vec_removeElementAt);
    registerNative("java/util/Vector.removeAllElements:()V", n_Vec_removeAllElements);
    registerNative("java/util/Vector.size:()I", n_Vec_size);
    registerNative("java/util/Vector.isEmpty:()Z", n_Vec_isEmpty);
    registerNative("java/util/Vector.contains:(Ljava/lang/Object;)Z", n_Vec_contains);
    registerNative("java/util/Vector.indexOf:(Ljava/lang/Object;)I", n_Vec_indexOf);
    registerNative("java/util/Vector.firstElement:()Ljava/lang/Object;", n_Vec_firstElement);
    registerNative("java/util/Vector.lastElement:()Ljava/lang/Object;", n_Vec_lastElement);

    // java.util.Random
    registerNative("java/util/Random.<init>:(J)V", n_Random_init);
    registerNative("java/util/Random.<init>:()V", n_Random_init0);
    registerNative("java/util/Random.setSeed:(J)V", n_Random_setSeed);
    registerNative("java/util/Random.nextInt:()I", n_Random_nextInt);
    registerNative("java/util/Random.nextInt:(I)I", n_Random_nextIntBound);
    registerNative("java/util/Random.nextLong:()J", n_Random_nextLong);
    registerNative("java/util/Random.nextDouble:()D", n_Random_nextDouble);
    registerNative("java/util/Random.nextFloat:()F", n_Random_nextFloat);
    registerNative("java/util/Random.nextBoolean:()Z", n_Random_nextBoolean);

    // java.lang.System
    registerNative("java/lang/System.currentTimeMillis:()J", n_System_currentTimeMillis);
    registerNative("java/lang/System.arraycopy:(Ljava/lang/Object;ILjava/lang/Object;II)V", n_System_arraycopy);
    registerNative("java/lang/System.gc:()V", n_System_gc);
    registerNative("java/lang/System.getProperty:(Ljava/lang/String;)Ljava/lang/String;", n_System_getProperty);
    registerNative("java/lang/System.identityHashCode:(Ljava/lang/Object;)I", n_System_identityHashCode);
    registerNative("java/lang/Runtime.getRuntime:()Ljava/lang/Runtime;", n_Runtime_getRuntime);
    registerNative("java/lang/Runtime.totalMemory:()J", n_Runtime_totalMemory);
    registerNative("java/lang/Runtime.freeMemory:()J", n_Runtime_freeMemory);
    registerNative("java/lang/Runtime.maxMemory:()J", n_Runtime_maxMemory);
    registerNative("java/lang/Runtime.gc:()V", n_System_gc);

    // java.io.PrintStream
    registerNative("java/io/PrintStream.println:(Ljava/lang/String;)V", n_PrintStream_println);
    registerNative("java/io/PrintStream.println:(I)V", n_PrintStream_printlnInt);
    registerNative("java/io/PrintStream.println:()V", n_PrintStream_println);
    registerNative("java/io/PrintStream.println:(Ljava/lang/Object;)V", n_PrintStream_printlnObj);
    registerNative("java/io/PrintStream.print:(Ljava/lang/String;)V", n_PrintStream_print);
    registerNative("java/io/PrintStream.print:(I)V", n_PrintStream_printInt);
    registerNative("java/io/PrintStream.flush:()V", n_PrintStream_flush);

    // java.lang.Class
    registerNative("java/lang/Class.getName:()Ljava/lang/String;", n_Class_getName);
    registerNative("java/lang/Class.forName:(Ljava/lang/String;)Ljava/lang/Class;", n_Class_forName);
    registerExtraNatives(); // en dernier : prime sur les enregistrements précédents (ex. Math.pow corrigé)
}

} // namespace jvm