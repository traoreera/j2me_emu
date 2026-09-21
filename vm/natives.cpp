#include "native.h"
#include "interpreter.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <vector>

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
void jme_threadForget(Obj *r)
{
    for (size_t i = 0; i < g_threads.size(); i++)
        if (g_threads[i] == r) { g_threads.erase(g_threads.begin() + (ptrdiff_t)i); return; }
}

namespace
{
void n_Object_init(NativeContext *) {}
void n_Object_getClass(NativeContext *ctx)
{
    Obj *o = argRef(ctx, 0);
    Obj *co = ctx->rt->heap().classObjFor(o ? o->cls->name : "");
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
    Obj *r = ctx->thisObj ? ctx->thisObj->cells[0].o : nullptr;
    if (getenv("JME_DEBUG"))
        fprintf(stderr, "Thread.start(runnable=%p cls=%s)\n", (void *)r,
                r && r->cls ? r->cls->name.c_str() : "-");
    jme_threadStart(r);
}
void n_Thread_sleep(NativeContext *ctx) { (void)ctx; }
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
    setLongResult(ctx, static_cast<int64_t>(clock() * 1000 / CLOCKS_PER_SEC));
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
void n_System_gc(NativeContext *) {}
void n_System_identityHashCode(NativeContext *ctx)
{
    setIntResult(ctx, static_cast<int32_t>(reinterpret_cast<uintptr_t>(argRef(ctx, 0))));
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

void initNatives()
{
    using namespace std::placeholders;

    // java.lang.Object
    registerNative("java/lang/Object.<init>:()V", n_Object_init);
    registerNative("java/lang/Object.getClass:()Ljava/lang/Class;", n_Object_getClass);
    registerNative("java/lang/Object.equals:(Ljava/lang/Object;)Z", n_Object_equals);
    registerNative("java/lang/Object.hashCode:()I", n_Object_hashCode);
    registerNative("java/lang/Object.toString:()Ljava/lang/String;", n_Object_toString);

    // java.lang.String
    registerNative("java/lang/String.<init>:()V", n_Object_init);
    registerNative("java/lang/String.length:()I", n_String_length);
    registerNative("java/lang/String.charAt:(I)C", n_String_charAt);
    registerNative("java/lang/String.toCharArray:()[C", n_String_toCharArray);
    registerNative("java/lang/String.concat:(Ljava/lang/String;)Ljava/lang/String;", n_String_concat);
    registerNative("java/lang/String.equals:(Ljava/lang/Object;)Z", n_String_equals);
    registerNative("java/lang/String.equalsIgnoreCase:(Ljava/lang/String;)Z", n_String_equalsIgnoreCase);
    registerNative("java/lang/String.substring:(I)Ljava/lang/String;", n_String_substring1);
    registerNative("java/lang/String.substring:(II)Ljava/lang/String;", n_String_substring2);
    registerNative("java/lang/String.indexOf:(Ljava/lang/String;)I", n_String_indexOf);
    registerNative("java/lang/String.indexOf:(I)I", n_String_indexOf);
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
    registerNative("java/lang/Thread.currentThread:()Ljava/lang/Thread;", n_Thread_currentThread);
    registerNative("java/lang/Thread.setPriority:(I)V", n_Thread_setPriority);
    registerNative("java/lang/Thread.interrupt:()V", n_Thread_interrupt);
    registerNative("java/lang/Thread.isAlive:()Z", n_Thread_isAlive);
    registerNative("java/lang/Thread.join:()V", n_Thread_join);

    // java.util.Hashtable
    registerNative("java/util/Hashtable.<init>:()V", n_HT_init);
    registerNative("java/util/Hashtable.get:(Ljava/lang/Object;)Ljava/lang/Object;", n_HT_get);
    registerNative("java/util/Hashtable.put:(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", n_HT_put);
    registerNative("java/util/Hashtable.remove:(Ljava/lang/Object;)Ljava/lang/Object;", n_HT_remove);
    registerNative("java/util/Hashtable.containsKey:(Ljava/lang/Object;)Z", n_HT_containsKey);
    registerNative("java/util/Hashtable.clear:()V", n_HT_clear);
    registerNative("java/util/Hashtable.size:()I", n_HT_size);
    registerNative("java/util/Hashtable.isEmpty:()Z", n_HT_isEmpty);

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
    registerNative("java/lang/System.identityHashCode:(Ljava/lang/Object;)I", n_System_identityHashCode);

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