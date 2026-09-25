#include "interpreter.h"
#include "native.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

namespace jvm
{

namespace
{
// Flags de debug lus UNE fois (getenv() = scan linéaire de environ ; ces tests
// sont dans le chemin chaud d'invoke/dispatch/new, évalués des millions de
// fois par seconde).
inline bool envDebug() { static const bool v = getenv("JME_DEBUG") != nullptr; return v; }
inline bool envTrace() { static const bool v = getenv("JME_TRACE") != nullptr; return v; }
inline bool envTraceAll() { static const bool v = getenv("JME_TRACEALL") != nullptr; return v; }

inline uint8_t rb(const uint8_t *c, int &pc) { return c[pc++]; }
inline int16_t rb16(const uint8_t *c, int &pc) { int v = (c[pc] << 8) | c[pc + 1]; pc += 2; return static_cast<int16_t>(v); }
inline uint16_t rbu16(const uint8_t *c, int &pc) { int v = (c[pc] << 8) | c[pc + 1]; pc += 2; return static_cast<uint16_t>(v); }
inline int32_t rb32(const uint8_t *c, int &pc) { int v = (c[pc] << 24) | (c[pc + 1] << 16) | (c[pc + 2] << 8) | c[pc + 3]; pc += 4; return v; }

// Mappe le caractère de type restant (après avoir consommé `dims` '[') vers
// le ObjKind des tableaux "feuille". 'L' ou '[' restants -> ObjArray (cas
// des dimensions non explicitement dimensionnées, ex. `new int[3][]` : les
// sous-tableaux restent naturellement null, ce qui est le comportement Java
// attendu).
ObjKind leafArrayKind(char baseChar)
{
    switch (baseChar)
    {
    case 'B': return ObjKind::ByteArray;
    case 'S': return ObjKind::ShortArray;
    case 'I': return ObjKind::IntArray;
    case 'J': return ObjKind::LongArray;
    case 'F': return ObjKind::FloatArray;
    case 'D': return ObjKind::DoubleArray;
    case 'C': return ObjKind::CharArray;
    case 'Z': return ObjKind::BoolArray;
    default: return ObjKind::ObjArray; // 'L...;' ou '[' (dimension non dimensionnée)
    }
}

// Construit récursivement un tableau multi-dimensionnel (multianewarray) :
// sizes[0] = taille de la dimension externe ... sizes[n-1] = taille de la
// dimension la plus interne dimensionnée explicitement.
Obj *buildMultiArray(Runtime *rt, ObjKind leafKind, const int32_t *sizes, int level, int totalDims)
{
    int32_t n = sizes[level];
    if (n < 0)
        return nullptr;
    if (level == totalDims - 1)
        return rt->heap().newArray(leafKind, n);
    Obj *outer = rt->heap().newArray(ObjKind::ObjArray, n);
    if (!outer)
        return nullptr;
    for (int32_t i = 0; i < n; i++)
    {
        Obj *inner = buildMultiArray(rt, leafKind, sizes, level + 1, totalDims);
        if (!inner)
            return nullptr;
        outer->cells[i] = Value::fromRef(inner);
    }
    return outer;
}

int slotsOfDesc(const std::string &d)
{
    char c = d.empty() ? 0 : d[0];
    if (c == 'J' || c == 'D')
        return 2;
    return 1;
}

int argSlots(const std::string &desc)
{
    const char *p = desc.c_str();
    if (*p != '(')
        return 0;
    p++;
    int n = 0;
    while (*p && *p != ')')
    {
        if (*p == 'L')
        {
            n++;
            while (*p && *p != ';')
                p++;
            if (*p)
                p++;
        }
        else if (*p == '[')
        {
            n++;
            while (*p == '[')
                p++;
            if (*p == 'L')
                while (*p && *p != ';')
                    p++;
            if (*p)
                p++;
        }
        else
        {
            n += slotsOfDesc(std::string(1, *p));
            p++;
        }
    }
    return n;
}

bool returnIsVoid(const std::string &desc)
{
    const char *p = desc.c_str();
    while (*p && *p != ')')
        p++;
    if (*p == ')')
        p++;
    return (*p == 'V');
}

ClassInfo *runtimeClassOf(Runtime *rt, Obj *o)
{
    if (!o)
        return nullptr;
    switch (o->kind)
    {
    case ObjKind::String: return rt->classInfoOfName("java/lang/String");
    case ObjKind::Class: return rt->classInfoOfName("java/lang/Class");
    case ObjKind::ByteArray:
    case ObjKind::ShortArray:
    case ObjKind::IntArray:
    case ObjKind::LongArray:
    case ObjKind::FloatArray:
    case ObjKind::DoubleArray:
    case ObjKind::CharArray:
    case ObjKind::BoolArray:
    case ObjKind::ObjArray: return rt->classInfoOfName("java/lang/Object");
    default: return o->cls;
    }
}

// Cherche, dans la table d'exceptions de `code`, un handler dont l'étendue
// [startPc,endPc) couvre `throwPc` (le site d'un athrow, ou d'un appel de
// méthode qui a échoué en propageant une exception) et dont le catchType
// correspond au type dynamique de `ex` (0 = catch-all/finally). Les
// handlers sont dans l'ordre du fichier .class, qui est déjà l'ordre de
// priorité JVM (premier match retenu).
bool findExceptionHandler(const CodeAttribute *code, const ConstantPool &cp,
                           int throwPc, Obj *ex, int &outHandlerPc)
{
    if (!ex || ex->kind != ObjKind::Instance)
        return false;
    for (const auto &h : code->handlers)
    {
        if (throwPc < h.startPc || throwPc >= h.endPc)
            continue;
        if (h.catchType == 0)
        {
            outHandlerPc = h.handlerPc;
            return true;
        }
        std::string wantName = cp.getClassName(h.catchType);
        for (ClassInfo *oc = ex->cls; oc; oc = oc->super)
        {
            if (oc->name == wantName)
            {
                outHandlerPc = h.handlerPc;
                return true;
            }
        }
    }
    return false;
}
} // namespace

// ---------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------

Interpreter::Interpreter(Runtime *rt) : rt_(rt)
{
    arenaSize_ = 128 * 1024;
    arena_ = new uint8_t[arenaSize_];
}

Interpreter::~Interpreter()
{
    delete[] arena_;
}

void *Interpreter::frameAlloc(size_t n)
{
    n = (n + 7) & ~size_t(7);
    if (arenaOff_ + n > arenaSize_)
    {
        fprintf(stderr, "JVM: arène de frames épuisée\n");
        return nullptr;
    }
    void *p = arena_ + arenaOff_;
    arenaOff_ += n;
    return p;
}

void Interpreter::frameFree(size_t mark)
{
    arenaOff_ = mark;
}

// ---------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------

bool Interpreter::dispatch(ClassInfo *cls, const MethodRecord *m, Obj *thisObj,
                           Value *args, int nargs, Value &result)
{
    if (m->mi)
    {
        // JME_TRACEM=Classe.methode : trace générique (une seule lecture de
        // l'environnement) des appels d'UNE méthode bytecode -- premiers
        // arguments et valeur retournée. Outil de diagnostic, pas de code
        // spécifique à un jeu.
        static const char *traceM = getenv("JME_TRACEM");
        if (traceM && traceM[0] == '*' && !traceM[1])
            fprintf(stderr, "TRACEALLM %s.%s%s\n", cls->name.c_str(), m->name.c_str(), m->desc.c_str());
        else if (traceM && (cls->name + "." + m->name) == traceM)
        {
            bool r = execBytecode(cls, m, thisObj, args, nargs, result);
            fprintf(stderr, "TRACEM %s.%s%s args=[", cls->name.c_str(), m->name.c_str(), m->desc.c_str());
            for (int i = 0; i < nargs && i < 8; i++)
                fprintf(stderr, "%s%d", i ? "," : "", args[i].i);
            fprintf(stderr, "] -> ok=%d ret=%d\n", r ? 1 : 0, result.i);
            return r;
        }
        return execBytecode(cls, m, thisObj, args, nargs, result);
    }
    else
    {
        std::string key = cls->name + "." + m->name + ":" + m->desc;
        NativeFn fn = findNative(key);
        if (!fn)
        {
            fprintf(stderr, "JVM: native manquante: %s\n", key.c_str());
            return false;
        }
        NativeContext ctx;
        ctx.rt = rt_;
        ctx.interp = this;
        ctx.args = args;
        ctx.nargs = nargs;
        ctx.thisObj = thisObj;
        ctx.result = &result;
        if (envTrace())
        {
            const std::string &mn = m->name;
            if (mn.find("reateImage") != std::string::npos || mn.find("etResource") != std::string::npos ||
                mn.find("ByteArray") != std::string::npos || mn.find("decode") != std::string::npos ||
                mn.find("drawRGB") != std::string::npos || mn.find("load") != std::string::npos)
                fprintf(stderr, "NATIVE %s.%s:%s\n", cls->name.c_str(), m->name.c_str(), m->desc.c_str());
        }
        if (envTraceAll())
            fprintf(stderr, "NAT %s.%s:%s%s\n", cls->name.c_str(), m->name.c_str(),
                    m->desc.c_str(), ctx.thisObj && ctx.thisObj->cls ? (std::string(" (caller=") + ctx.thisObj->cls->name + ")").c_str() : "");
        fn(&ctx);
        return true;
    }
}

bool Interpreter::invoke(ClassInfo *cls, const MethodRecord *m, Obj *thisObj,
                         Value *args, int nargs, Value &result)
{
    return dispatch(cls, m, thisObj, args, nargs, result);
}

bool Interpreter::invokeStatic(ClassInfo *declClass, const std::string &name, const std::string &desc,
                               int nargs, Value *args, Value &result)
{
    ClassInfo *c = declClass;
    while (c)
    {
        const MethodRecord *m = c->findMethod(name, desc);
        if (m)
        {
            if (!ensureInit(c))
            {
                if (envDebug())
                    fprintf(stderr, "invokeStatic: ensureInit echec sur %s (appel %s%s)\n",
                            c->name.c_str(), name.c_str(), desc.c_str());
                return false;
            }
            return dispatch(c, m, nullptr, args, nargs, result);
        }
        c = c->super;
    }
    if (envDebug())
        fprintf(stderr, "invokeStatic: méthode %s%s introuvable dans %s\n",
                name.c_str(), desc.c_str(), declClass ? declClass->name.c_str() : "(null)");
    return false;
}

bool Interpreter::invokeSpecial(ClassInfo *declClass, const std::string &name, const std::string &desc,
                                Obj *thisObj, Value *args, int nargs, Value &result)
{
    // invokespecial se résout statiquement à partir de la classe référencée
    // (declClass), en remontant toute la chaîne de super -- jamais depuis le
    // type runtime de thisObj. L'ancien fallback sur
    // runtimeClassOf(thisObj)->findMethodVirtual() était faux : pour un
    // super() vers un <init> natif non enregistré (ex. Canvas, qui n'a pas
    // de <init> dans midp_natives.cpp) ou vers une classe non résolue
    // (ex. com/nokia/mid/ui/FullCanvas, non implémentée), il retombait sur
    // le type réel de thisObj et pouvait retrouver la méthode <init>
    // ACTUELLEMENT EN COURS D'EXÉCUTION elle-même (même nom/descripteur),
    // provoquant une récursion infinie jusqu'à épuisement de l'arène de
    // frames -- observé sur tout MIDlet/Canvas dérivé avec un constructeur
    // sans argument (cas très courant), ex. games/mission.jar. Si declClass
    // est nul (classe non résolue), on échoue proprement plutôt que de
    // deviner via thisObj.
    const MethodRecord *m = declClass ? declClass->findMethodVirtual(name, desc) : nullptr;
    if (!m)
    {
        fprintf(stderr, "JVM: invokeSpecial: méthode %s%s introuvable dans %s\n",
                name.c_str(), desc.c_str(),
                declClass ? declClass->name.c_str() : "(null, classe non résolue)");
        return false;
    }
    return dispatch(m->owner, m, thisObj, args, nargs, result);
}

bool Interpreter::invokeVirtual(ClassInfo *declClass, const std::string &name, const std::string &desc,
                                Obj *thisObj, Value *args, int nargs, Value &result)
{
    if (!thisObj)
    {
        if (envDebug())
            fprintf(stderr, "invokeVirtual: thisObj NULL pour %s%s\n", name.c_str(), desc.c_str());
        return false;
    }
    (void)declClass;
    ClassInfo *rc = runtimeClassOf(rt_, thisObj);
    if (!rc)
    {
        if (envDebug())
            fprintf(stderr, "invokeVirtual: runtimeClass NULL pour %s%s (obj kind=%d cls=%s)\n",
                    name.c_str(), desc.c_str(), (int)thisObj->kind,
                    thisObj->cls ? thisObj->cls->name.c_str() : "?");
        return false;
    }
    const MethodRecord *m = rc->findMethodVirtual(name, desc);
    if (!m)
    {
        fprintf(stderr, "JVM: invokeVirtual: méthode %s%s introuvable dans %s\n",
                name.c_str(), desc.c_str(), rc->name.c_str());
        return false;
    }
    if (!ensureInit(rc))
    {
        if (envDebug())
            fprintf(stderr, "invokeVirtual: ensureInit echec sur %s (appel %s%s)\n",
                    rc->name.c_str(), name.c_str(), desc.c_str());
        return false;
    }
    if (envDebug() && name == "setFullScreenMode")
        fprintf(stderr, "invokeVirtual: dispatch %s.%s%s receiver=%s cls=%s mi=%d\n",
                m->owner->name.c_str(), m->name.c_str(), m->desc.c_str(),
                thisObj && thisObj->cls ? thisObj->cls->name.c_str() : "?", rc->name.c_str(),
                m->mi ? 1 : 0);
    bool dr = dispatch(m->owner, m, thisObj, args, nargs, result);
    if (envDebug() && name == "setFullScreenMode")
        fprintf(stderr, "invokeVirtual: dispatch-result=%d\n", dr ? 1 : 0);
    return dr;
}

bool Interpreter::ensureInit(ClassInfo *cls)
{
    if (cls->clinitDone)
        return true;
    cls->clinitDone = true;

    const MethodRecord *clinit = cls->findClinit();
    if (!clinit || !clinit->mi)
        return true;

    Value ignore;
    bool ok = dispatch(cls, clinit, nullptr, nullptr, 0, ignore);
    if (!ok)
        cls->clinitDone = false;
    return ok;
}

// ---------------------------------------------------------------------
// Bytecode
// ---------------------------------------------------------------------

bool Interpreter::execBytecode(ClassInfo *cls, const MethodRecord *m, Obj *thisObj,
                               Value *args, int nargs, Value &result)
{
    const CodeAttribute *code = m->mi->code();
    if (!code)
        return false;
    const uint8_t *c = code->code.data();
    int codeLen = static_cast<int>(code->code.size());
    const ConstantPool &cp = cls->cf->constantPool;

    size_t mark = arenaOff_;
    size_t nLocals = code->maxLocals;
    size_t nStack = code->maxStack;
    size_t nframes = nLocals * sizeof(Value) + (sizeof(Value) + sizeof(uint8_t)) * nStack;
    void *chunk = frameAlloc(nframes);
    if (!chunk)
        return false;
    std::memset(chunk, 0, nframes);

    Frame f;
    f.cls = cls;
    f.method = m;
    f.thisObj = thisObj;
    f.locals = reinterpret_cast<Value *>(chunk);
    f.localsCount = static_cast<int>(nLocals);
    f.stack = f.locals + nLocals;
    f.stackCap = static_cast<int>(nStack);
    f.stackCat = reinterpret_cast<uint8_t *>(f.stack + nStack);
    f.sp = 0;
    f.pc = 0;

    for (int i = 0; i < nargs && i < static_cast<int>(nLocals); i++)
        f.locals[i] = args[i];
    if (thisObj && nLocals > 0)
        f.locals[0] = Value::fromRef(thisObj);

    static bool traceOnce = []()
    {
        const char *tr = getenv("JME_TRACE");
        if (tr && (strcmp(tr, "1") == 0 || strcmp(tr, "on") == 0))
        {
            fprintf(stderr, "JME_TRACE active\n");
            return true;
        }
        return false;
    }();
    static bool trace = traceOnce;

    int &pc = f.pc;
    int &sp = f.sp;
    Value *st = f.stack;
    uint8_t *ct = f.stackCat;
    Value *lv = f.locals;

    auto push = [&](Value v, int k)
    {
        if (sp + 1 > f.stackCap) { sp = f.stackCap; pc = codeLen; return; }
        st[sp] = v; ct[sp] = static_cast<uint8_t>(k); sp++;
    };
    auto pushInt = [&](int32_t v) { push(Value::fromInt(v), 1); };
    auto pushRef = [&](Obj *o) { push(Value::fromRef(o), 1); };
    auto pop = [&]() -> Value
    {
        if (sp < 1) { sp = 0; pc = codeLen; return Value(); }
        return st[--sp];
    };
    auto popInt = [&]() -> int32_t { return pop().i; };
    auto popRef = [&]() -> Obj * { return pop().o; };
    auto pushLong = [&](int64_t v)
    {
        if (sp + 2 > f.stackCap) { sp = f.stackCap; pc = codeLen; return; }
        st[sp].l = v; ct[sp] = 2; ct[sp + 1] = 2; sp += 2;
    };
    auto popLong = [&]() -> int64_t
    {
        if (sp < 2) { sp = 0; pc = codeLen; return 0; }
        sp -= 2;
        return st[sp].l;
    };

    int64_t ret = 0;
    bool done = false;
    bool okResult = true;
    Value resultVal = Value();

    // Lève une exception Java (ex. NPE / AIOOBE) sur un accès tableau
    // invalide : l'interpréteur traitait arr==null / index hors bornes comme
    // un échec d'opcode « dur » (okResult=false), qui tuait toute la chaîne
    // d'appels sans jamais exécuter les catch(Exception) des jeux. Sur une
    // vraie JVM c'est une NullPointerException / ArrayIndexOutOfBoundsException
    // rattrapable (n'importe quel catch(Exception) — GAMELOFT wrappe tout son
    // rendu dedans). Retourne true si le handler de la frame courante a pris
    // le relais (peut continuer), false si elle doit se propager aux appelants.
    auto raiseJava = [&](ClassInfo *excls, int throwPc) -> bool
    {
        if (!excls)
        {
            if (envDebug())
                fprintf(stderr, "RAISE excls=NULL (classe exception non enregistrée) throwPc=%d in %s.%s\n",
                        throwPc, cls->name.c_str(), m->name.c_str());
            return false;
        }
        Obj *ex = rt_->heap().newInstance(excls);
        if (!ex)
        {
            rt_->reportOom();
            return false;
        }
        int handlerPc;
        if (findExceptionHandler(code, cp, throwPc, ex, handlerPc))
        {
            if (envDebug())
                fprintf(stderr, "RAISE %s caught->%d in %s.%s throwPc=%d\n",
                        excls->name.c_str(), handlerPc, cls->name.c_str(), m->name.c_str(), throwPc);
            sp = 0;
            pushRef(ex);
            pc = handlerPc;
            return true;
        }
        if (envDebug())
            fprintf(stderr, "RAISE %s NO-HANDLER in %s.%s throwPc=%d -> pending\n",
                    excls->name.c_str(), cls->name.c_str(), m->name.c_str(), throwPc);
        pendingException_ = ex;
        return false;
    };
    auto raiseIfBadArray = [&](Obj *arr, int idx, int throwPc) -> bool
    {
        if (!arr)
        {
            bool h = raiseJava(rt_->classInfoOfName("java/lang/NullPointerException"), throwPc);
            if (!h) { okResult = false; done = true; }
            return true;
        }
        if (idx < 0 || idx >= arr->arrayLen)
        {
            bool h = raiseJava(rt_->classInfoOfName("java/lang/ArrayIndexOutOfBoundsException"), throwPc);
            if (!h) { okResult = false; done = true; }
            return true;
        }
        return false;
    };

    while (!done && pc >= 0 && pc < codeLen)
    {
        if (instrBudget_ >= 0 && --instrBudget_ < 0)
        {
            if (yieldFn_)
            {
                // Suspend la fibre courante ; ne revient que réveillé par le
                // scheduler à une trame ultérieure. pc/sp/locals/pile C++
                // sont préservés intacts (swapcontext), on continue juste la
                // boucle avec un budget neuf pour cette nouvelle trame.
                yieldFn_();
                if (envDebug())
                    fprintf(stderr, "YIELD %s.%s pc=%d op=0x%02x\n", cls->name.c_str(), m->name.c_str(), pc, c[pc]);
                instrBudget_ = instrBudgetQuota_;
                continue;
            }
            okResult = false;
            done = true;
            break;
        }
        uint8_t op = rb(c, pc);
        if (trace)
            fprintf(stderr, "  [%s.%s pc=%d] op=0x%02x sp=%d local0=%p\n",
                    cls->name.c_str(), m->name.c_str(), pc, op, sp,
                    (f.localsCount > 0) ? (void *)f.locals[0].o : (void *)0);
        switch (op)
        {
        case 0x00: break;
        case 0x01: pushRef(nullptr); break;
        case 0x02: pushInt(-1); break;
        case 0x03: pushInt(0); break;
        case 0x04: pushInt(1); break;
        case 0x05: pushInt(2); break;
        case 0x06: pushInt(3); break;
        case 0x07: pushInt(4); break;
        case 0x08: pushInt(5); break;
        case 0x09: pushLong(0); break;
        case 0x0a: pushLong(1); break;
        case 0x0b:
        { int32_t bits = 0x3f000000; Value v; v.f = *reinterpret_cast<float *>(&bits); push(v, 1); break; }
        case 0x0c:
        { int32_t bits = 0x3f800000; Value v; v.f = *reinterpret_cast<float *>(&bits); push(v, 1); break; }
        case 0x0d:
        { int32_t bits = 0x40000000; Value v; v.f = *reinterpret_cast<float *>(&bits); push(v, 1); break; }
        case 0x0e: pushLong(0); break;
        case 0x0f: pushLong(0x3ff0000000000000LL); break;
        case 0x10: pushInt(static_cast<int8_t>(rb(c, pc))); break;
        case 0x11: pushInt(rb16(c, pc)); break;
        case 0x12:
        case 0x13:
        {
            uint16_t idx = (op == 0x12) ? rb(c, pc) : rbu16(c, pc);
            const CpEntry *e = cp.get(idx);
            if (!e) { okResult = false; done = true; break; }
            switch (e->tag)
            {
            case CONSTANT_STRING:
                pushRef(rt_->heap().newString(cp.getUtf8(e->nameIndex)));
                break;
            case CONSTANT_INTEGER:
                pushInt(e->intVal);
                break;
            case CONSTANT_FLOAT:
            { Value v; v.f = e->floatVal; push(v, 1); break; }
            case CONSTANT_CLASS:
                pushRef(rt_->heap().classObjFor(cp.getClassName(idx)));
                break;
            default:
                okResult = false; done = true; break;
            }
            break;
        }
        case 0x14:
        {
            uint16_t idx = rbu16(c, pc);
            const CpEntry *e = cp.get(idx);
            if (!e) { okResult = false; done = true; break; }
            if (e->tag == CONSTANT_LONG)
                pushLong(e->longVal);
            else if (e->tag == CONSTANT_DOUBLE)
            {
                double d = e->doubleVal; int64_t bits; std::memcpy(&bits, &d, 8); pushLong(bits);
            }
            else { okResult = false; done = true; }
            break;
        }

        case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
        {
            uint8_t idx = rb(c, pc);
            if (op == 0x16 || op == 0x18)
                pushLong(lv[idx].l);
            else
                push(lv[idx], 1);
            break;
        }
        case 0x1a: case 0x1b: case 0x1c: case 0x1d:
            push(lv[op - 0x1a], 1); break;
        case 0x1e: case 0x1f: case 0x20: case 0x21:
            pushLong(lv[op - 0x1e].l); break;
        case 0x22: case 0x23: case 0x24: case 0x25:
            push(lv[op - 0x22], 1); break;
        case 0x26: case 0x27: case 0x28: case 0x29:
            pushLong(lv[op - 0x26].l); break;
        case 0x2a: case 0x2b: case 0x2c: case 0x2d:
            push(lv[op - 0x2a], 1); break;

        case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a:
        {
            uint8_t idx = rb(c, pc);
            if (op == 0x37 || op == 0x39)
            {
                int64_t v = popLong();
                lv[idx].l = v; lv[idx + 1].l = 0;
            }
            else
            {
                lv[idx] = pop();
            }
            break;
        }
        case 0x3b: case 0x3c: case 0x3d: case 0x3e: lv[op - 0x3b] = pop(); break;
        case 0x3f: case 0x40: case 0x41: case 0x42:
        {
            int64_t v = popLong(); int i = op - 0x3f; lv[i].l = v; lv[i + 1].l = 0; break;
        }
        case 0x43: case 0x44: case 0x45: case 0x46: lv[op - 0x43] = pop(); break;
        case 0x47: case 0x48: case 0x49: case 0x4a:
        {
            int64_t v = popLong(); int i = op - 0x47; lv[i].l = v; lv[i + 1].l = 0; break;
        }
        case 0x4b: case 0x4c: case 0x4d: case 0x4e: lv[op - 0x4b] = pop(); break;

        case 0x50: case 0x52: // lastore, dastore : valeur sur 2 slots (catégorie 2)
        {
            Value v = Value::fromLong(popLong());
            int idx = popInt();
            Obj *arr = popRef();
            if (raiseIfBadArray(arr, idx, pc - 1)) break;
            arr->cells[idx] = v;
            break;
        }
        case 0x4f: case 0x51: case 0x53:
        {
            Value v = pop();
            int idx = popInt();
            Obj *arr = popRef();
            if (raiseIfBadArray(arr, idx, pc - 1)) break;
            arr->cells[idx] = v;
            break;
        }
        case 0x54: case 0x55: case 0x56:
        {
            Value v = pop();
            int idx = popInt();
            Obj *arr = popRef();
            if (raiseIfBadArray(arr, idx, pc - 1)) break;
            arr->cells[idx] = v;
            break;
        }

        case 0x2e:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (raiseIfBadArray(arr, idx, pc - 1)) break;
            push(arr->cells[idx], 1);
            break;
        }
        case 0x2f:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (raiseIfBadArray(arr, idx, pc - 1)) break;
            pushLong(arr->cells[idx].l);
            break;
        }
        case 0x30:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (raiseIfBadArray(arr, idx, pc - 1)) break;
            push(arr->cells[idx], 1);
            break;
        }
        case 0x31:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (raiseIfBadArray(arr, idx, pc - 1)) break;
            pushLong(arr->cells[idx].l);
            break;
        }
        case 0x32:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (raiseIfBadArray(arr, idx, pc - 1)) break;
            push(arr->cells[idx], 1);
            break;
        }
        case 0x33: case 0x34: case 0x35:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (raiseIfBadArray(arr, idx, pc - 1)) break;
            {
                int32_t v = arr->cells[idx].i;
                if (op == 0x33) v = static_cast<int8_t>(v);
                else if (op == 0x34) v = static_cast<uint16_t>(v);
                else v = static_cast<int16_t>(v);
                pushInt(v);
            }
            break;
        }

        case 0x57: if (sp > 0) sp--; break;
        case 0x58: { int k = (sp >= 1 && ct[sp - 1] == 2) ? 2 : 1; if (sp >= k) sp -= k; break; }
        case 0x59:
        {
            if (sp >= 1) { st[sp] = st[sp - 1]; ct[sp] = ct[sp - 1]; sp++; }
            break;
        }
        case 0x5a:
        {
            if (sp >= 2)
            {
                Value a = st[sp - 1], b = st[sp - 2];
                uint8_t ca = ct[sp - 1], cb = ct[sp - 2];
                st[sp - 2] = a; ct[sp - 2] = ca;
                st[sp - 1] = b; ct[sp - 1] = cb;
                st[sp] = a; ct[sp] = ca;
                sp++;
            }
            break;
        }
        case 0x5b:
        {
            if (sp >= 3)
            {
                Value a = st[sp - 1], b = st[sp - 2], c = st[sp - 3];
                uint8_t ca = ct[sp - 1], cb = ct[sp - 2], cc = ct[sp - 3];
                st[sp - 3] = a; ct[sp - 3] = ca;
                st[sp - 2] = c; ct[sp - 2] = cc;
                st[sp - 1] = b; ct[sp - 1] = cb;
                st[sp] = a; ct[sp] = ca;
                sp++;
            }
            break;
        }
        case 0x5c:
        {
            if (sp >= 2)
            {
                if (ct[sp - 2] == 2) { st[sp] = st[sp - 2]; ct[sp] = 2; ct[sp + 1] = 2; sp += 2; }
                else { st[sp] = st[sp - 2]; st[sp + 1] = st[sp - 1]; ct[sp] = ct[sp - 2]; ct[sp + 1] = ct[sp - 1]; sp += 2; }
            }
            break;
        }
        case 0x5f:
        {
            if (sp >= 2) { Value a = st[sp - 1]; st[sp - 1] = st[sp - 2]; st[sp - 2] = a; uint8_t ca = ct[sp - 1]; ct[sp - 1] = ct[sp - 2]; ct[sp - 2] = ca; }
            break;
        }

        case 0x60: { int b = popInt(); int a = popInt(); pushInt(a + b); break; }
        case 0x64: { int b = popInt(); int a = popInt(); pushInt(a - b); break; }
        case 0x68: { int b = popInt(); int a = popInt(); pushInt(a * b); break; }
        case 0x6c: { int b = popInt(); int a = popInt(); if (b == 0) { okResult = false; done = true; } else pushInt(a / b); break; }
        case 0x70: { int b = popInt(); int a = popInt(); if (b == 0) { okResult = false; done = true; } else pushInt(a % b); break; }
        case 0x74: { int a = popInt(); pushInt(-a); break; }
        case 0x78: { int b = popInt(); int a = popInt(); pushInt(a << (b & 31)); break; }
        case 0x7a: { int b = popInt(); int a = popInt(); pushInt(a >> (b & 31)); break; }
        case 0x7c: { int b = popInt(); int a = popInt(); pushInt(static_cast<int>(static_cast<uint32_t>(a) >> (b & 31))); break; }
        case 0x7e: { int b = popInt(); int a = popInt(); pushInt(a & b); break; }
        case 0x80: { int b = popInt(); int a = popInt(); pushInt(a | b); break; }
        case 0x82: { int b = popInt(); int a = popInt(); pushInt(a ^ b); break; }
        case 0x84:
        {
            uint8_t idx = rb(c, pc);
            int8_t cst = static_cast<int8_t>(rb(c, pc));
            lv[idx].i += cst;
            break;
        }

        case 0x85: { pushLong(popInt()); break; }
        case 0x86: { Value v; v.f = static_cast<float>(popInt()); push(v, 1); break; }
        case 0x87: { double d = popInt(); int64_t bits; std::memcpy(&bits, &d, 8); pushLong(bits); break; }
        case 0x88: { pushInt(static_cast<int32_t>(popLong())); break; }
        case 0x89: { Value v; v.f = static_cast<float>(popLong()); push(v, 1); break; }
        case 0x8a: { double d = static_cast<double>(popLong()); int64_t bits; std::memcpy(&bits, &d, 8); pushLong(bits); break; }
        case 0x8b: { int32_t a = popInt(); Value v; v.f = *reinterpret_cast<float *>(&a); pushInt(static_cast<int32_t>(v.f)); break; }
        case 0x8c: { int32_t a = popInt(); float v = *reinterpret_cast<float *>(&a); pushLong(static_cast<int64_t>(v)); break; }
        case 0x8d: { int32_t a = popInt(); float v = *reinterpret_cast<float *>(&a); double d = v; int64_t bits; std::memcpy(&bits, &d, 8); pushLong(bits); break; }
        case 0x8e: { int64_t a = popLong(); double d; std::memcpy(&d, &a, 8); pushInt(static_cast<int32_t>(d)); break; }
        case 0x8f: { int64_t a = popLong(); double d; std::memcpy(&d, &a, 8); pushLong(static_cast<int64_t>(d)); break; }
        case 0x90: { int64_t a = popLong(); double d; std::memcpy(&d, &a, 8); Value v; v.f = static_cast<float>(d); push(v, 1); break; }
        case 0x91: pushInt(static_cast<int8_t>(popInt())); break;
        case 0x92: pushInt(static_cast<uint16_t>(popInt())); break;
        case 0x93: pushInt(static_cast<int16_t>(popInt())); break;

        case 0x61: { int64_t b = popLong(); int64_t a = popLong(); pushLong(a + b); break; }
        case 0x65: { int64_t b = popLong(); int64_t a = popLong(); pushLong(a - b); break; }
        case 0x69: { int64_t b = popLong(); int64_t a = popLong(); pushLong(a * b); break; }
        case 0x6d: { int64_t b = popLong(); int64_t a = popLong(); if (b == 0) { okResult = false; done = true; } else pushLong(a / b); break; }
        case 0x71: { int64_t b = popLong(); int64_t a = popLong(); if (b == 0) { okResult = false; done = true; } else pushLong(a % b); break; }
        case 0x75: { pushLong(-popLong()); break; }
        case 0x79: { int b = popInt(); int64_t a = popLong(); pushLong(a << (b & 63)); break; }
        case 0x7b: { int b = popInt(); int64_t a = popLong(); pushLong(a >> (b & 63)); break; }
        case 0x7d: { int b = popInt(); int64_t a = popLong(); pushLong(static_cast<int64_t>(static_cast<uint64_t>(a) >> (b & 63))); break; }
        case 0x7f: { int64_t b = popLong(); int64_t a = popLong(); pushLong(a & b); break; }
        case 0x81: { int64_t b = popLong(); int64_t a = popLong(); pushLong(a | b); break; }
        case 0x83: { int64_t b = popLong(); int64_t a = popLong(); pushLong(a ^ b); break; }

        case 0x94: { int64_t b = popLong(); int64_t a = popLong(); pushInt(a < b ? -1 : (a > b ? 1 : 0)); break; }
        case 0x95: case 0x96:
        {
            int32_t bbits = popInt(); int32_t abits = popInt();
            float b = *reinterpret_cast<float *>(&bbits);
            float a = *reinterpret_cast<float *>(&abits);
            int r = (std::isnan(a) || std::isnan(b)) ? (op == 0x95 ? -1 : 1) : (a < b ? -1 : (a > b ? 1 : 0));
            pushInt(r);
            break;
        }
        case 0x97: case 0x98:
        {
            int64_t bb = popLong(); int64_t ab = popLong();
            double a, b; std::memcpy(&a, &ab, 8); std::memcpy(&b, &bb, 8);
            int r = (std::isnan(a) || std::isnan(b)) ? (op == 0x97 ? -1 : 1) : (a < b ? -1 : (a > b ? 1 : 0));
            pushInt(r);
            break;
        }

        case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e:
        {
            int16_t off = rb16(c, pc);
            int v = popInt();
            bool t;
            switch (op)
            {
            case 0x99: t = (v == 0); break; case 0x9a: t = (v != 0); break;
            case 0x9b: t = (v < 0); break; case 0x9c: t = (v >= 0); break;
            case 0x9d: t = (v > 0); break; default: t = (v <= 0); break;
            }
            if (t) pc = (pc - 3) + off;
            break;
        }
        case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4:
        {
            int16_t off = rb16(c, pc);
            int b = popInt(), a = popInt();
            bool t;
            switch (op)
            {
            case 0x9f: t = (a == b); break; case 0xa0: t = (a != b); break;
            case 0xa1: t = (a < b); break; case 0xa2: t = (a >= b); break;
            case 0xa3: t = (a > b); break; default: t = (a <= b); break;
            }
            if (t) pc = (pc - 3) + off;
            break;
        }
        case 0xa5: case 0xa6:
        {
            int16_t off = rb16(c, pc);
            Obj *b = popRef(); Obj *a = popRef();
            if ((op == 0xa5 ? (a == b) : (a != b))) pc = (pc - 3) + off;
            break;
        }
        case 0xc6: case 0xc7:
        {
            int16_t off = rb16(c, pc);
            Obj *a = popRef();
            if ((op == 0xc6 ? (a == nullptr) : (a != nullptr))) pc = (pc - 3) + off;
            break;
        }
        case 0xa7:
        {
            int16_t off = rb16(c, pc);
            pc = (pc - 3) + off;
            break;
        }
        case 0xc8:
        {
            int32_t off = rb32(c, pc);
            pc = (pc - 5) + off;
            break;
        }
        case 0xaa:
        {
            // Les offsets (default et table) sont relatifs à l'adresse de
            // L'OPCODE tableswitch lui-même (spec JVM), pas à `base` (le
            // début, après padding, des champs default/lo/hi/table). Utiliser
            // `base` ici faisait systématiquement atterrir 2-3 octets trop
            // loin (dans le padding/les champs eux-mêmes), exécutant des
            // octets arbitraires comme du bytecode -- observé concrètement :
            // atterrissage sur un `iconst_3` au lieu d'un `aload_0`, la
            // valeur 3 étant ensuite prise pour une référence d'objet par
            // le getfield suivant (segfault).
            int opcodePc = pc - 1;
            while ((pc & 3) != 0) pc++;
            int base = pc;
            int def = rb32(c, pc);
            int lo = rb32(c, pc);
            int hi = rb32(c, pc);
            int key = popInt();
            if (key < lo || key > hi)
            {
                pc = opcodePc + def;
            }
            else
            {
                // L'ancienne vérif ne bornait que le DÉBUT de la table (base+12),
                // jamais l'entrée réellement lue (base+12+(key-lo)*4) -- lecture
                // hors tableau possible dès qu'un tableswitch a plus de quelques
                // entrées (ex. dispatch UP/DOWN/LEFT/RIGHT/FIRE d'un jeu), avec
                // segfault à la clé. On borne l'accès réel ici.
                int64_t tOff = (int64_t)base + 12 + (int64_t)(key - lo) * 4;
                if (tOff < 0 || tOff + 4 > (int64_t)codeLen)
                {
                    pc = codeLen;
                }
                else
                {
                    const uint8_t *tb = c + tOff;
                    int32_t e = static_cast<int32_t>((tb[0] << 24) | (tb[1] << 16) | (tb[2] << 8) | tb[3]);
                    pc = opcodePc + e;
                }
            }
            break;
        }
        case 0xab:
        {
            // Même correction que tableswitch (0xaa) : les offsets sont
            // relatifs à l'adresse de l'opcode lookupswitch lui-même, pas à
            // `base` (début des champs default/npairs/table après padding).
            int opcodePc = pc - 1;
            while ((pc & 3) != 0) pc++;
            int base = pc;
            int def = rb32(c, pc);
            int npairs = rb32(c, pc);
            int key = popInt();
            int found = -1;
            // En-tête de lookupswitch = default + npairs = 8 octets (pas 12 :
            // contrairement à tableswitch, qui a 3 champs default/lo/hi avant
            // sa table, lookupswitch n'en a que 2). Utiliser +12 ici (copié
            // par erreur depuis tableswitch) désalignait chaque paire de 4
            // octets : on lisait le offset de la paire i comme si c'était sa
            // valeur de match, et la valeur de match de la paire i+1 comme
            // si c'était son offset -- la vraie valeur de match n'était donc
            // (presque) jamais comparée, et le `key` recherché ne correspondait
            // (presque) jamais à rien, forçant systématiquement la branche
            // `default`. Observé sur games/mortal_combat_new_b_240x320_173007.jar :
            // un dispatch d'état (`switch` sur un champ static, 23 paires)
            // dans `paint()` ne prenait jamais aucun cas réel, l'écran restant
            // noir en permanence alors qu'aucune erreur n'était jamais levée.
            for (int i = 0; i < npairs && (size_t)(base + 8 + (i + 1) * 8) <= (size_t)codeLen; i++)
            {
                const uint8_t *p = c + base + 8 + i * 8;
                int32_t cv = static_cast<int32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
                if (cv == key)
                {
                    int32_t e = static_cast<int32_t>((p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7]);
                    found = e;
                    break;
                }
            }
            pc = (found >= 0) ? opcodePc + found : opcodePc + def;
            break;
        }

        case 0xac: { resultVal = Value::fromInt(popInt()); done = true; break; }
        case 0xad: { resultVal = Value::fromLong(popLong()); done = true; break; }
        case 0xae: { resultVal = pop(); done = true; break; }
        case 0xaf: { resultVal = Value::fromLong(popLong()); done = true; break; }
        case 0xb0: { resultVal = pop(); done = true; break; }
        case 0xb1: { resultVal = Value(); done = true; break; }

        case 0xb2: case 0xb3:
        {
            uint16_t idx = rbu16(c, pc);
            ClassInfo *tc = nullptr;
            const MethodRecord *f = nullptr;
            std::string fn, fd; // seulement renseignés si echec (message d'erreur) ou pas encore en cache
            int cachedW = 1;
            if (idx < cls->fieldRefCache.size() && cls->fieldRefCache[idx].field)
            {
                tc = cls->fieldRefCache[idx].tc;
                f = cls->fieldRefCache[idx].field;
                cachedW = cls->fieldRefCache[idx].w;
            }
            else
            {
                auto fr = cp.getFieldRef(idx);
                if (fr.first.empty()) { okResult = false; done = true; break; }
                std::string classRef = fr.first;
                size_t colon = fr.second.find(':');
                fn = fr.second.substr(0, colon);
                fd = fr.second.substr(colon + 1);
                tc = rt_->classInfoOfName(classRef);
                if (!tc) tc = rt_->loadFromJar(classRef);
                if (!tc)
                {
                    fprintf(stderr, "JVM: get/putstatic: classe %s introuvable (0x%02x)\n", classRef.c_str(), op);
                    okResult = false; done = true; break;
                }
            }
            // Un accès à un champ static doit initialiser la classe cible
            // (<clinit>) au préalable si ce n'est pas déjà fait -- contexte
            // manquant ici jusqu'ici (seul invokestatic/new le faisaient),
            // alors qu'un getstatic peut très bien être le tout premier
            // accès à une classe (ex. lire un tableau statique alloué dans
            // son <clinit> avant même le premier appel de méthode dessus) :
            // observé sur games/mission.jar, un sastore vers un champ static
            // jamais alloué faute d'avoir lancé <clinit>. ensureInit() est
            // O(1) après la première fois (juste un bool), donc pas besoin
            // de la sauter sur un hit de cache.
            if (!tc->clinitDone && !ensureInit(tc))
            {
                okResult = false; done = true; break;
            }
            if (!f)
            {
                ClassInfo *owner = tc;
                f = owner->findField(fn, fd);
                while (!f && owner->super) { owner = owner->super; f = owner->findField(fn, fd); }
                if (!f)
                {
                    fprintf(stderr, "JVM: get/putstatic: champ %s.%s introuvable (0x%02x)\n",
                            tc->name.c_str(), fn.c_str(), op);
                    okResult = false; done = true; break;
                }
                if (idx >= cls->fieldRefCache.size())
                    cls->fieldRefCache.resize(cp.entries.size());
                cls->fieldRefCache[idx] = {tc, f, slotsOfDesc(f->desc)}; cachedW = cls->fieldRefCache[idx].w;
            }
            ClassInfo *owner = f->owner;
            if (op == 0xb2)
            {
                int w = cachedW;
                if (w == 2) pushLong(owner->statics[f->slot].l);
                else push(owner->statics[f->slot], 1);
            }
            else
            {
                int w = cachedW;
                Value v = (w == 2) ? Value::fromLong(popLong()) : pop();
                owner->statics[f->slot] = v;
            }
            break;
        }
        case 0xb4: case 0xb5:
        {
            uint16_t idx = rbu16(c, pc);
            // Cache de résolution (cf. ClassInfo::fieldRefCache) : évite de
            // reparser "nom:desc" et de refaire findFieldRecursive() --
            // recherche linéaire -- à chaque exécution du même
            // getfield/putfield. `tc` reste nullptr ici (pas d'ensureInit
            // pour un champ d'instance) ; validité expliquée dans runtime.h.
            const MethodRecord *ff = (idx < cls->fieldRefCache.size()) ? cls->fieldRefCache[idx].field : nullptr;
            std::string fn, fd;
            int w;
            if (ff)
            {
                w = cls->fieldRefCache[idx].w;
            }
            else
            {
                auto fr = cp.getFieldRef(idx);
                if (fr.first.empty()) { okResult = false; done = true; break; }
                size_t colon = fr.second.find(':');
                fn = fr.second.substr(0, colon);
                fd = fr.second.substr(colon + 1);
                w = slotsOfDesc(fd);
            }
            if (op == 0xb5)
            {
                Value v = (w == 2) ? Value::fromLong(popLong()) : pop();
                Obj *o = popRef();
                if (!o || o->kind != ObjKind::Instance) { okResult = false; done = true; break; }
                if (!ff)
                {
                    ff = o->cls->findFieldRecursive(fn, fd);
                    if (ff)
                    {
                        if (idx >= cls->fieldRefCache.size())
                            cls->fieldRefCache.resize(cp.entries.size());
                        cls->fieldRefCache[idx] = {nullptr, ff, w};
                    }
                }
                if (!ff)
                {
                    fprintf(stderr, "JVM: putfield: champ %s.%s introuvable (idx=%u) (0x%02x)\n",
                            o->cls ? o->cls->name.c_str() : "(null)", fn.c_str(), idx, op);
                    okResult = false; done = true; break;
                }
                o->cells[ff->slot] = v;
                break;
            }
            Obj *o = popRef();
            if (!o || o->kind != ObjKind::Instance) { okResult = false; done = true; break; }
            if (!ff)
            {
                ff = o->cls->findFieldRecursive(fn, fd);
                if (ff)
                {
                    if (idx >= cls->fieldRefCache.size())
                        cls->fieldRefCache.resize(cp.entries.size());
                    cls->fieldRefCache[idx] = {nullptr, ff, w};
                }
            }
            if (!ff)
            {
                fprintf(stderr, "JVM: getfield: champ %s.%s introuvable (idx=%u fd=%s) (0x%02x)\n",
                        o->cls ? o->cls->name.c_str() : "(null)", fn.c_str(), idx, fd.c_str(), op);
                okResult = false; done = true; break;
            }
            if (w == 2) pushLong(o->cells[ff->slot].l);
            else push(o->cells[ff->slot], 1);
            break;
        }

        case 0xb6: case 0xb7: case 0xb8: case 0xb9:
        {
            int opcodePc = pc - 1;
            uint16_t idx = rbu16(c, pc);
            if (op == 0xb9)
            {
                if (envDebug()) fprintf(stderr, "invokeinterface hit, pc before skip=%d\n", pc);
                rb(c, pc); rb(c, pc);
                if (envDebug()) fprintf(stderr, "invokeinterface pc after skip=%d\n", pc);
            }
            // Taille fixée une fois pour toutes (jamais de réallocation ensuite : `e`
            // reste valide pendant les appels imbriqués qui peuplent d'autres entrées).
            if (cls->methodRefCache.empty())
                cls->methodRefCache.resize(cp.entries.size());
            if (idx >= cls->methodRefCache.size()) { okResult = false; done = true; break; }
            ClassInfo::MethodCacheEntry &e = cls->methodRefCache[idx];
            if (!e.valid)
            {
                auto mr = cp.getMethodRef(idx);
                if (mr.first.empty()) { okResult = false; done = true; break; }
                ClassInfo *ctc = rt_->classInfoOfName(mr.first);
                if (!ctc && rt_->jar()) ctc = rt_->loadFromJar(mr.first);
                if (!ctc && op == 0xb8)
                {
                    if (envDebug())
                        fprintf(stderr, "invokestatic: tc NULL pour %s.%s (caller=%s.%s pc=%d)\n",
                                mr.first.c_str(), mr.second.c_str(), cls->name.c_str(), m->name.c_str(), pc);
                    // non mis en cache : la classe pourra être chargée plus tard
                    sp -= argSlots(mr.second.substr(mr.second.find(':') + 1));
                    if (!returnIsVoid(mr.second.substr(mr.second.find(':') + 1))) pushInt(0);
                    okResult = false; done = true; break;
                }
                size_t colon = mr.second.find(':');
                e.tc = ctc;
                e.name = mr.second.substr(0, colon);
                e.desc = mr.second.substr(colon + 1);
                e.nslots = argSlots(e.desc);
                e.rv = returnIsVoid(e.desc);
                size_t rp = e.desc.find(')');
                e.retType = (rp != std::string::npos && rp + 1 < e.desc.size()) ? e.desc[rp + 1] : 'V';
                e.valid = true;
            }
            ClassInfo *tc = e.tc;
            const std::string &mname = e.name;
            const std::string &mdesc = e.desc;
            int nslots = e.nslots;
            bool rv = e.rv;
            Value mres;
            bool ok = false;
            if (op == 0xb8)
            {
                if (!e.staticM)
                {
                    // Même résolution qu'invokeStatic : chaîne de super, première correspondance.
                    ClassInfo *owner = tc;
                    const MethodRecord *fm = nullptr;
                    while (owner && !(fm = owner->findMethod(mname, mdesc)))
                        owner = owner->super;
                    if (fm) e.staticM = fm;
                    else if (envDebug())
                        fprintf(stderr, "invokeStatic: méthode %s%s introuvable dans %s\n", mname.c_str(), mdesc.c_str(), tc->name.c_str());
                }
                if (e.staticM)
                {
                    ClassInfo *owner = e.staticM->owner;
                    ok = ensureInit(owner) && dispatch(owner, e.staticM, nullptr, &st[sp - nslots], nslots, mres);
                }
                sp -= nslots;
            }
            else
            {
                Obj *receiver = st[sp - nslots - 1].o;
                Value *argsPtr = st + (sp - nslots - 1);
                if (op == 0xb7)
                {
                    if (!e.staticM && tc)
                        e.staticM = tc->findMethodVirtual(mname, mdesc);
                    if (e.staticM)
                        ok = dispatch(e.staticM->owner, e.staticM, receiver, argsPtr, nslots + 1, mres);
                    else
                        ok = invokeSpecial(tc, mname, mdesc, receiver, argsPtr, nslots + 1, mres); // message d'erreur habituel
                }
                else
                {
                    ClassInfo *rc = receiver ? runtimeClassOf(rt_, receiver) : nullptr;
                    if (rc && rc == e.lastRecv)
                    {
                        ok = dispatch(e.lastM->owner, e.lastM, receiver, argsPtr, nslots + 1, mres);
                    }
                    else if (rc)
                    {
                        const MethodRecord *vm_ = rc->findMethodVirtual(mname, mdesc);
                        if (vm_ && ensureInit(rc))
                        {
                            e.lastRecv = rc;
                            e.lastM = vm_;
                            ok = dispatch(vm_->owner, vm_, receiver, argsPtr, nslots + 1, mres);
                        }
                        else if (!vm_)
                            ok = invokeVirtual(tc, mname, mdesc, receiver, argsPtr, nslots + 1, mres); // message d'erreur habituel
                    }
                    else
                        ok = invokeVirtual(tc, mname, mdesc, receiver, argsPtr, nslots + 1, mres); // récepteur null / classe inconnue
                }
                sp -= nslots + 1;
            }
            if (!rv)
            {
                char rt = e.retType;
                if (rt == 'J' || rt == 'D') pushLong(mres.l);
                else push(mres, 1);
            }
            if (!ok)
            {
                // Une méthode appelée peut avoir échoué parce qu'une
                // exception Java (athrow) s'est propagée sans être
                // rattrapée dans SA propre frame : on tente ici de la
                // rattraper dans la NÔTRE (table d'exceptions de la méthode
                // en cours, autour de ce site d'appel), exactement comme le
                // ferait un vrai déroulement de pile JVM. Sans ça, tout
                // `try { ... } catch (Exception e) { ... }` autour d'un
                // appel de méthode était invisible pour l'interpréteur :
                // n'importe quel échec (throw explicite ou natif manquant)
                // tuait la frame entière au lieu d'exécuter le bloc catch
                // (observé sur games/jump.jar : JumpCanvas.<init> fait
                // `throw new Exception(...)` sur les résolutions d'écran
                // non prévues, capturé par l'appelant pour retomber sur une
                // init par défaut -- sans rattrapage, le MIDlet entier
                // échouait à s'instancier).
                Obj *pending = pendingException_;
                pendingException_ = nullptr;
                int handlerPc;
                if (pending && findExceptionHandler(code, cp, opcodePc, pending, handlerPc))
                {
                    sp = 0;
                    pushRef(pending);
                    pc = handlerPc;
                    break;
                }
                if (pending)
                    pendingException_ = pending; // toujours pas rattrapée : continue de remonter
                if (envDebug())
                    fprintf(stderr, "invokeEchec %s.%s%s (caller=%s.%s pc=%d)\n",
                            tc ? tc->name.c_str() : "?", mname.c_str(), mdesc.c_str(),
                            cls->name.c_str(), m->name.c_str(), pc);
                okResult = false; done = true;
            }
            break;
        }

        case 0xbb:
        {
            uint16_t idx = rbu16(c, pc);
            std::string classRef = cp.getClassName(idx);
            ClassInfo *tc = rt_->classInfoOfName(classRef);
            if (!tc && rt_->jar()) tc = rt_->loadFromJar(classRef);
            if (!tc) { if (envDebug()) fprintf(stderr, "new: class %s introuvable\n", classRef.c_str()); okResult = false; done = true; break; }
            if (!ensureInit(tc)) { if (envDebug()) fprintf(stderr, "new: ensureInit echec pour %s\n", classRef.c_str()); okResult = false; done = true; break; }
            Obj *o = rt_->heap().newInstance(tc);
            if (!o) { rt_->reportOom(); if (envDebug()) fprintf(stderr, "new: OOM pour %s\n", classRef.c_str()); okResult = false; done = true; break; }
            pushRef(o);
            break;
        }
        case 0xbc:
        {
            uint8_t atype = rb(c, pc);
            int count = popInt();
            if (envDebug() && count < 0)
                fprintf(stderr, "newarray: count NEGATIF=%d atype=%d dans %s.%s\n",
                        count, atype, cls->name.c_str(), m->name.c_str());
            ObjKind k;
            switch (atype)
            {
            case 4: k = ObjKind::BoolArray; break;
            case 5: k = ObjKind::CharArray; break;
            case 6: k = ObjKind::FloatArray; break;
            case 7: k = ObjKind::DoubleArray; break;
            case 8: k = ObjKind::ByteArray; break;
            case 9: k = ObjKind::ShortArray; break;
            case 10: k = ObjKind::IntArray; break;
            case 11: k = ObjKind::LongArray; break;
            default: k = ObjKind::IntArray; break;
            }
            Obj *a = rt_->heap().newArray(k, count);
            if (!a) { rt_->reportOom(); okResult = false; done = true; break; }
            pushRef(a);
            break;
        }
        case 0xbd:
        {
            rbu16(c, pc);
            int count = popInt();
            Obj *a = rt_->heap().newArray(ObjKind::ObjArray, count);
            if (!a) { rt_->reportOom(); okResult = false; done = true; break; }
            pushRef(a);
            break;
        }
        case 0xc0: case 0xc1:
        {
            uint16_t idx = rbu16(c, pc);
            std::string classRef = cp.getClassName(idx);
            Obj *o = (sp >= 1) ? st[sp - 1].o : nullptr;
            bool is = false;
            if (o && !classRef.empty() && classRef[0] == '[')
            {
                // Cast/instanceof vers un type tableau (ex. "[I", "[[B") :
                // pas de ClassInfo pour ces descripteurs, on compare le ObjKind.
                size_t p = classRef.find_first_not_of('[');
                char base = (p != std::string::npos) ? classRef[p] : 'I';
                if (p >= 2)
                    is = (o->kind == ObjKind::ObjArray);
                else
                    is = (o->kind == leafArrayKind(base));
            }
            else
            {
                ClassInfo *tc = rt_->classInfoOfName(classRef);
                if (o && o->kind == ObjKind::Instance)
                {
                    ClassInfo *oc = o->cls;
                    while (oc) { if (!tc || oc == tc) { is = true; break; } oc = oc->super; }
                }
            }
            if (op == 0xc1)
                st[sp - 1].i = is ? 1 : 0;
            else if (o && !is)
            {
                if (envDebug())
                {
                    fprintf(stderr, "JVM: checkcast fail vers %s (objet kind=%d cls=%s len=%d ref=%p) in %s.%s pc=%d sp=%d\n",
                            classRef.c_str(), (int)o->kind,
                            o->cls ? o->cls->name.c_str() : "-",
                            o->arrayLen, (void *)o, cls->name.c_str(), m->name.c_str(), pc, sp);
                }
                okResult = false; done = true;
            }
            break;
        }
        case 0xbe:
        {
            Obj *a = popRef();
            if (!a)
            {
                if (raiseJava(rt_->classInfoOfName("java/lang/NullPointerException"), pc - 1))
                    break;
                okResult = false; done = true;
            }
            else pushInt(a->arrayLen);
            break;
        }
        case 0xbf:
        {
            int opcodePc = pc - 1;
            Obj *ex = popRef();
            int handlerPc;
            if (ex && findExceptionHandler(code, cp, opcodePc, ex, handlerPc))
            {
                sp = 0;
                pushRef(ex);
                pc = handlerPc;
                break;
            }
            if (envDebug())
                fprintf(stderr, "JVM: exception non rattrapée (athrow) ex=%s dans %s.%s pc=%d\n",
                        ex && ex->cls ? ex->cls->name.c_str() : "?", cls->name.c_str(), m->name.c_str(), opcodePc);
            pendingException_ = ex;
            okResult = false; done = true;
            break;
        }
        case 0xc2: case 0xc3:
        {
            // monitorenter/monitorexit : pas de moniteur réel, mais JVMS les
            // fait POINTER l'objectref de la pile d'opérandes. Un no-op sans
            // pop laissait une référence fantôme sur la pile, qui pouvait
            // pousser sp au-delà de maxStack dans les méthodes au budget de
            // pile exact (ex. GameThread.run avec stack=3 et un wait(long) :
            // le garde-fou d'overflow fixes then pc = codeLen, le run() était
            // tué silencieusement).
            if (sp >= 1) st[--sp].o = nullptr;
            break;
        }

        case 0xc4: // wide
        {
            uint8_t sub = rb(c, pc);
            uint16_t idx = rbu16(c, pc);
            if (sub == 0x15 || sub == 0x19) push(lv[idx], 1);
            else if (sub == 0x16 || sub == 0x18) pushLong(lv[idx].l);
            else if (sub == 0x36 || sub == 0x3a) lv[idx] = pop();
            else if (sub == 0x37 || sub == 0x39) { int64_t v = popLong(); lv[idx].l = v; lv[idx + 1].l = 0; }
            else if (sub == 0x84)
            {
                int16_t cst = rb16(c, pc);
                lv[idx].i += cst;
            }
            break;
        }
        case 0xc5:
        {
            uint16_t clsIdx = rbu16(c, pc);
            uint8_t dims = rb(c, pc);
            if (dims < 1 || dims > 32) { okResult = false; done = true; break; }
            std::vector<int32_t> sizes(dims);
            for (int i = dims - 1; i >= 0; i--)
                sizes[i] = popInt();
            std::string arrDesc = cp.getClassName(clsIdx); // ex: "[[I", "[[Ljava/lang/String;"
            size_t brackets = arrDesc.find_first_not_of('[');
            char baseChar = (brackets != std::string::npos) ? arrDesc[brackets] : 'I';
            Obj *a = buildMultiArray(rt_, leafArrayKind(baseChar), sizes.data(), 0, dims);
            if (!a) { rt_->reportOom(); okResult = false; done = true; break; }
            pushRef(a);
            break;
        }

        default:
            fprintf(stderr, "JVM: opcode non implémenté 0x%02X pc=%d dans %s.%s\n",
                    op, pc - 1, cls->name.c_str(), m->name.c_str());
            okResult = false;
            done = true;
            break;
        }
    }

    if (done && okResult)
        result = resultVal;
    if (!okResult && envDebug())
    {
        fprintf(stderr, "FRMFALSE %s.%s pc=%d codeLen=%d\n",
                cls->name.c_str(), m->name.c_str(), pc, codeLen);
        int fp = pc - 1;
        if (fp >= 0 && fp < codeLen)
        {
            fprintf(stderr, "  FAILBYTE[%d]=%02X next=[", fp, c[fp]);
            for (int i = fp + 1; i < fp + 6 && i < codeLen; i++)
                fprintf(stderr, " %02X", c[i]);
            fprintf(stderr, " ]\n");
        }
    }
    frameFree(mark);
    return okResult;
}

} // namespace jvm