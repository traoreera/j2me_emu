#include "native.h"
#include "interpreter.h"

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
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "jme_threadResume r=%p %s fiber\n", (void*)r, f ? "resuming existing" : "CREATING NEW");
    if (!f)
    {
        f = new JmeFiber();
        f->stack.resize(256 * 1024);
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
    swapcontext(&f->callerCtx, &f->ctx);
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

void jme_threadForget(Obj *r)
{
    for (size_t i = 0; i < g_threads.size(); i++)
        if (g_threads[i] == r) { g_threads.erase(g_threads.begin() + (ptrdiff_t)i); break; }
    auto it = fiberMap().find(r);
    if (it != fiberMap().end())
    {
        delete it->second;
        fiberMap().erase(it);
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
// Object.wait()/wait(long)/notify()/notifyAll() : l'émulateur n'a pas de
// moniteur réel. Pacing de boucle de jeu = reposer la fibre jusqu'à la
// prochaine trame (cf. Thread.sleep), notify/notifyAll = no-op. Sans ces
// registrations, wait() était une méthode introuvable → Erreur non capturable
// par les handlers `catch (Exception)` (NoSuchMethodError est un Error), ce
// qui tuait silencieusement les fibres des jeux dont le thread principal
// rythme sa boucle à l'aide de wait(long) (ex. games/jump.jar : scène figée).
void n_Object_wait(NativeContext *ctx) { (void)ctx; jme_yieldNow(); }
void n_Object_notify(NativeContext *ctx) { (void)ctx; }
void n_Object_notifyAll(NativeContext *ctx) { (void)ctx; }

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

void n_String_initBytes(NativeContext *ctx)
{
    Obj *self = argRef(ctx, 0);
    Obj *data = argRef(ctx, 1);
    if (!self || !data || data->kind != ObjKind::ByteArray) return;
    self->kind = ObjKind::String;
    self->str.clear();
    self->str.reserve(static_cast<size_t>(data->arrayLen));
    for (int i = 0; i < data->arrayLen; i++)
        self->str += static_cast<char>(data->cells[i].u & 0xFF);
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
    self->str.reserve(static_cast<size_t>(len));
    for (int i = 0; i < len; i++)
        self->str += static_cast<char>(data->cells[off + i].u & 0xFF);
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
    setIntResult(ctx, (i >= 0 && i < static_cast<int>(s.size())) ? s[i] : 0);
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
    if (b < 0) b = 0;
    if (b > static_cast<int>(s.size())) b = static_cast<int>(s.size());
    setRefResult(ctx, ctx->rt->heap().newString(s.substr(b)));
}
void n_String_substring2(NativeContext *ctx)
{
    const std::string &s = strOf(argRef(ctx, 0));
    int b = argInt(ctx, 1);
    int e = argInt(ctx, 2);
    if (b < 0) b = 0;
    if (e > static_cast<int>(s.size())) e = static_cast<int>(s.size());
    if (e < b) e = b;
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
void n_Integer_parseInt(NativeContext *ctx)
{
    Obj *s = argRef(ctx, 0);
    setIntResult(ctx, (s && s->kind == ObjKind::String) ? atoi(s->str.c_str()) : 0);
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
    if (getenv("JME_DEBUG"))
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
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "Thread.start(runnable=%p cls=%s)\n", (void *)r,
                r && r->cls ? r->cls->name.c_str() : "-");
    jme_threadStart(r);
}
void n_Thread_sleep(NativeContext *ctx) { (void)ctx; jme_yieldNow(); }
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
        jme_yieldNow();
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

std::unordered_map<std::string, std::vector<std::vector<uint8_t>>> &rsRegistry()
{
    static std::unordered_map<std::string, std::vector<std::vector<uint8_t>>> m;
    return m;
}

std::string rsName(NativeContext *ctx)
{
    Obj *n = ctx->thisObj && ctx->thisObj->cells ? ctx->thisObj->cells[RS_NAME].o : nullptr;
    return (n && n->kind == ObjKind::String) ? n->str : "";
}

void n_RS_open(NativeContext *ctx)
{
    Obj *nameObj = argRef(ctx, 0);
    std::string name = (nameObj && nameObj->kind == ObjKind::String) ? nameObj->str : "";
    rsRegistry()[name]; // crée l'entrée si absente (magasin neuf, vide)
    ClassInfo *c = clsOf(ctx, "javax/microedition/rms/RecordStore");
    Obj *rs = c ? ctx->rt->heap().newInstance(c) : nullptr;
    if (rs && rs->cells) rs->cells[RS_NAME] = Value::fromRef(nameObj);
    setRefResult(ctx, rs);
}
void n_RS_getNumRecords(NativeContext *ctx)
{
    setIntResult(ctx, static_cast<int32_t>(rsRegistry()[rsName(ctx)].size()));
}
void n_RS_getRecord(NativeContext *ctx)
{
    auto &recs = rsRegistry()[rsName(ctx)];
    int id = argInt(ctx, 1);
    Obj *buf = argRef(ctx, 2);
    int off = argInt(ctx, 3);
    if (id < 1 || (size_t)id > recs.size() || !buf) { setIntResult(ctx, 0); return; }
    const auto &rec = recs[id - 1];
    for (size_t i = 0; i < rec.size() && off + (int)i < buf->arrayLen; i++)
        buf->cells[off + i].u = rec[i];
    setIntResult(ctx, static_cast<int32_t>(rec.size()));
}
void n_RS_setRecord(NativeContext *ctx)
{
    auto &recs = rsRegistry()[rsName(ctx)];
    int id = argInt(ctx, 1);
    Obj *buf = argRef(ctx, 2);
    int off = argInt(ctx, 3), len = argInt(ctx, 4);
    if (id < 1 || (size_t)id > recs.size() || !buf || len < 0) return;
    std::vector<uint8_t> rec(static_cast<size_t>(len));
    for (int i = 0; i < len && off + i < buf->arrayLen; i++)
        rec[i] = static_cast<uint8_t>(buf->cells[off + i].u);
    recs[id - 1] = std::move(rec);
}
void n_RS_addRecord(NativeContext *ctx)
{
    auto &recs = rsRegistry()[rsName(ctx)];
    Obj *buf = argRef(ctx, 1);
    int off = argInt(ctx, 2), len = argInt(ctx, 3);
    std::vector<uint8_t> rec(static_cast<size_t>(len < 0 ? 0 : len));
    if (buf)
        for (int i = 0; i < len && off + i < buf->arrayLen; i++)
            rec[i] = static_cast<uint8_t>(buf->cells[off + i].u);
    recs.push_back(std::move(rec));
    setIntResult(ctx, static_cast<int32_t>(recs.size()));
}
void n_RS_getRecordSize(NativeContext *ctx)
{
    auto &recs = rsRegistry()[rsName(ctx)];
    int id = argInt(ctx, 1);
    setIntResult(ctx, (id >= 1 && (size_t)id <= recs.size()) ? static_cast<int32_t>(recs[id - 1].size()) : 0);
}
void n_RS_close(NativeContext *) {}
void n_RS_deleteStore(NativeContext *ctx)
{
    Obj *nameObj = argRef(ctx, 0);
    std::string name = (nameObj && nameObj->kind == ObjKind::String) ? nameObj->str : "";
    rsRegistry().erase(name);
}
// getRecord:(I)[B -- variante qui retourne le record sous forme de tableau.
void n_RS_getRecordBytes(NativeContext *ctx)
{
    auto &recs = rsRegistry()[rsName(ctx)];
    int id = argInt(ctx, 1);
    if (id < 1 || (size_t)id > recs.size()) { setRefResult(ctx, nullptr); return; }
    const auto &rec = recs[id - 1];
    Obj *b = ctx->rt ? ctx->rt->heap().newArray(ObjKind::ByteArray, static_cast<int>(rec.size())) : nullptr;
    if (b)
        for (size_t i = 0; i < rec.size(); i++)
            b->cells[i].u = rec[i];
    setRefResult(ctx, b);
}
// enumerateRecords: énumération vide (comportement "aucune sauvegarde") : on ne
// stocke pas la liste dans le wrapper, hasNextElement() renvoie toujours faux.
void n_RS_enumerate(NativeContext *ctx)
{
    ClassInfo *c = clsOf(ctx, "javax/microedition/rms/RecordEnumerationImpl");
    Obj *e = c ? ctx->rt->heap().newInstance(c) : nullptr;
    setRefResult(ctx, e);
}
void n_RE_hasNext(NativeContext *ctx) { (void)ctx; setIntResult(ctx, 0); }
void n_RE_hasPrev(NativeContext *ctx) { (void)ctx; setIntResult(ctx, 0); }
void n_RE_nextId(NativeContext *ctx)
{
    if (!ctx->thisObj) return;
    setIntResult(ctx, -1);
}
void n_RE_prevId(NativeContext *ctx)
{
    if (!ctx->thisObj) return;
    setIntResult(ctx, -1);
}
void n_RE_next(NativeContext *ctx) { (void)ctx; setRefResult(ctx, nullptr); }
void n_RE_prev(NativeContext *ctx) { (void)ctx; setRefResult(ctx, nullptr); }
void n_RE_numRecords(NativeContext *ctx)
{
    (void)ctx;
    setIntResult(ctx, 0);
}
void n_RE_destroy(NativeContext *) { }
void n_RE_reset(NativeContext *) { }

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
    setRefResult(ctx, (d && idx >= 0 && idx < count) ? d->cells[idx].o : nullptr);
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
    setRefResult(ctx, (d && count > 0) ? d->cells[0].o : nullptr);
}
void n_Vec_lastElement(NativeContext *ctx)
{
    Obj *d = vecData(ctx);
    int count = vecCount(ctx);
    setRefResult(ctx, (d && count > 0) ? d->cells[count - 1].o : nullptr);
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
    if (key == "microedition.configuration") val = "CLDC-1.1";
    else if (key == "microedition.profiles") val = "MIDP-2.0";
    else if (key == "microedition.locale") val = "en-US";
    else if (key == "microedition.encoding") val = "ISO-8859-1";
    else val = "";
    setRefResult(ctx, ctx->rt->heap().newString(val));
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
void n_Class_forName(NativeContext *ctx)
{
    (void)ctx;
    setRefResult(ctx, nullptr);
}

} // namespace

static int64_t g_virtualMillis = 0;
int64_t virtualMillis() { return g_virtualMillis; }
void advanceVirtualMillis(int64_t ms) { g_virtualMillis += ms; }

void initNatives()
{
    using namespace std::placeholders;

    // java.lang.Object
    registerNative("java/lang/Object.<init>:()V", n_Object_init);
    registerNative("java/lang/Object.getClass:()Ljava/lang/Class;", n_Object_getClass);
    registerNative("java/lang/Object.equals:(Ljava/lang/Object;)Z", n_Object_equals);
    registerNative("java/lang/Object.hashCode:()I", n_Object_hashCode);
    registerNative("java/lang/Object.toString:()Ljava/lang/String;", n_Object_toString);
    registerNative("java/lang/Object.wait:()V", n_Object_wait);
    registerNative("java/lang/Object.wait:(I)V", n_Object_wait);
    registerNative("java/lang/Object.wait:(J)V", n_Object_wait);
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
    registerNative("javax/microedition/rms/RecordStore.getNumRecords:()I", n_RS_getNumRecords);
    registerNative("javax/microedition/rms/RecordStore.getRecord:(I[BI)I", n_RS_getRecord);
    registerNative("javax/microedition/rms/RecordStore.getRecordSize:(I)I", n_RS_getRecordSize);
    registerNative("javax/microedition/rms/RecordStore.setRecord:(I[BII)V", n_RS_setRecord);
    registerNative("javax/microedition/rms/RecordStore.addRecord:([BII)I", n_RS_addRecord);
    registerNative("javax/microedition/rms/RecordStore.closeRecordStore:()V", n_RS_close);
    registerNative("javax/microedition/rms/RecordStore.deleteRecordStore:(Ljava/lang/String;)V", n_RS_deleteStore);
    registerNative("javax/microedition/rms/RecordStore.enumerateRecords:(Ljavax/microedition/rms/RecordFilter;Ljavax/microedition/rms/RecordComparator;Z)Ljavax/microedition/rms/RecordEnumeration;", n_RS_enumerate);
    registerNative("javax/microedition/rms/RecordStore.getRecord:(I)[B", n_RS_getRecordBytes);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.hasNextElement:()Z", n_RE_hasNext);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.hasPreviousElement:()Z", n_RE_hasPrev);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.nextRecordId:()I", n_RE_nextId);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.previousRecordId:()I", n_RE_prevId);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.nextRecord:()[B", n_RE_next);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.previousRecord:()[B", n_RE_prev);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.numRecords:()I", n_RE_numRecords);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.destroy:()V", n_RE_destroy);
    registerNative("javax/microedition/rms/RecordEnumerationImpl.reset:()V", n_RE_reset);

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
}

} // namespace jvm