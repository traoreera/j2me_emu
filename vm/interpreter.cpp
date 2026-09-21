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
inline uint8_t rb(const uint8_t *c, int &pc) { return c[pc++]; }
inline int16_t rb16(const uint8_t *c, int &pc) { int v = (c[pc] << 8) | c[pc + 1]; pc += 2; return static_cast<int16_t>(v); }
inline uint16_t rbu16(const uint8_t *c, int &pc) { int v = (c[pc] << 8) | c[pc + 1]; pc += 2; return static_cast<uint16_t>(v); }
inline int32_t rb32(const uint8_t *c, int &pc) { int v = (c[pc] << 24) | (c[pc + 1] << 16) | (c[pc + 2] << 8) | c[pc + 3]; pc += 4; return v; }

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
        if (getenv("JME_TRACE"))
        {
            const std::string &mn = m->name;
            if (mn.find("reateImage") != std::string::npos || mn.find("etResource") != std::string::npos ||
                mn.find("ByteArray") != std::string::npos || mn.find("decode") != std::string::npos ||
                mn.find("drawRGB") != std::string::npos || mn.find("load") != std::string::npos)
                fprintf(stderr, "NATIVE %s.%s:%s\n", cls->name.c_str(), m->name.c_str(), m->desc.c_str());
        }
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
                return false;
            return dispatch(c, m, nullptr, args, nargs, result);
        }
        c = c->super;
    }
    return false;
}

bool Interpreter::invokeSpecial(ClassInfo *declClass, const std::string &name, const std::string &desc,
                                Obj *thisObj, Value *args, int nargs, Value &result)
{
    ClassInfo *c = declClass ? declClass : thisObj ? runtimeClassOf(rt_, thisObj) : nullptr;
    const MethodRecord *m = c ? c->findMethod(name, desc) : nullptr;
    if (!m && c && c->super)
        m = c->super->findMethod(name, desc);
    if (!m && thisObj)
    {
        ClassInfo *rc = runtimeClassOf(rt_, thisObj);
        m = rc ? rc->findMethodVirtual(name, desc) : nullptr;
    }
    if (!m)
    {
        fprintf(stderr, "JVM: invokeSpecial: méthode %s%s introuvable dans %s\n",
                name.c_str(), desc.c_str(),
                c ? c->name.c_str() : "(null)");
        return false;
    }
    return dispatch(m->owner, m, thisObj, args, nargs, result);
}

bool Interpreter::invokeVirtual(ClassInfo *declClass, const std::string &name, const std::string &desc,
                                Obj *thisObj, Value *args, int nargs, Value &result)
{
    if (!thisObj)
    {
        if (getenv("JME_DEBUG"))
            fprintf(stderr, "invokeVirtual: thisObj NULL pour %s%s\n", name.c_str(), desc.c_str());
        return false;
    }
    (void)declClass;
    ClassInfo *rc = runtimeClassOf(rt_, thisObj);
    if (!rc)
    {
        if (getenv("JME_DEBUG"))
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
        if (getenv("JME_DEBUG"))
            fprintf(stderr, "invokeVirtual: ensureInit echec sur %s (appel %s%s)\n",
                    rc->name.c_str(), name.c_str(), desc.c_str());
        return false;
    }
    if (getenv("JME_DEBUG") && name == "setFullScreenMode")
        fprintf(stderr, "invokeVirtual: dispatch %s.%s%s receiver=%s cls=%s mi=%d\n",
                m->owner->name.c_str(), m->name.c_str(), m->desc.c_str(),
                thisObj && thisObj->cls ? thisObj->cls->name.c_str() : "?", rc->name.c_str(),
                m->mi ? 1 : 0);
    bool dr = dispatch(m->owner, m, thisObj, args, nargs, result);
    if (getenv("JME_DEBUG") && name == "setFullScreenMode")
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

    while (!done && pc >= 0 && pc < codeLen)
    {
        if (instrBudget_ >= 0 && --instrBudget_ < 0)
        {
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

        case 0x4f: case 0x50: case 0x51: case 0x52: case 0x53:
        {
            Value v = pop();
            int idx = popInt();
            Obj *arr = popRef();
            if (!arr || idx < 0 || idx >= arr->arrayLen) { okResult = false; done = true; }
            else arr->cells[idx] = v;
            break;
        }
        case 0x54: case 0x55: case 0x56:
        {
            Value v = pop();
            int idx = popInt();
            Obj *arr = popRef();
            if (!arr || idx < 0 || idx >= arr->arrayLen) { okResult = false; done = true; }
            else arr->cells[idx] = v;
            break;
        }

        case 0x2e:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (!arr || idx < 0 || idx >= arr->arrayLen) { okResult = false; done = true; }
            else push(arr->cells[idx], 1);
            break;
        }
        case 0x2f:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (!arr || idx < 0 || idx >= arr->arrayLen) { okResult = false; done = true; }
            else pushLong(arr->cells[idx].l);
            break;
        }
        case 0x30:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (!arr || idx < 0 || idx >= arr->arrayLen) { okResult = false; done = true; }
            else push(arr->cells[idx], 1);
            break;
        }
        case 0x31:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (!arr || idx < 0 || idx >= arr->arrayLen) { okResult = false; done = true; }
            else pushLong(arr->cells[idx].l);
            break;
        }
        case 0x32:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (!arr || idx < 0 || idx >= arr->arrayLen) { okResult = false; done = true; }
            else push(arr->cells[idx], 1);
            break;
        }
        case 0x33: case 0x34: case 0x35:
        {
            int idx = popInt(); Obj *arr = popRef();
            if (!arr || idx < 0 || idx >= arr->arrayLen) { okResult = false; done = true; }
            else
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
            if (sp >= 2) { Value a = st[sp - 1], b = st[sp - 2]; uint8_t ca = ct[sp - 1], cb = ct[sp - 2]; st[sp - 1] = b; ct[sp - 1] = cb; st[sp] = a; ct[sp] = ca; sp++; }
            break;
        }
        case 0x5b:
        {
            if (sp >= 3) { Value a = st[sp - 1]; uint8_t ca = ct[sp - 1]; st[sp - 1] = st[sp - 2]; ct[sp - 1] = ct[sp - 2]; st[sp - 2] = st[sp - 3]; ct[sp - 2] = ct[sp - 3]; st[sp] = a; ct[sp] = ca; sp++; }
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
        case 0x79: { int64_t b = popLong(); int64_t a = popLong(); pushLong(a << (b & 63)); break; }
        case 0x7b: { int64_t b = popLong(); int64_t a = popLong(); pushLong(a >> (b & 63)); break; }
        case 0x7d: { int64_t b = popLong(); int64_t a = popLong(); pushLong(static_cast<int64_t>(static_cast<uint64_t>(a) >> (b & 63))); break; }
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
            while ((pc & 3) != 0) pc++;
            int base = pc;
            int def = rb32(c, pc);
            int lo = rb32(c, pc);
            int hi = rb32(c, pc);
            int key = popInt();
            if (key < lo || key > hi)
            {
                pc = base + def;
            }
            else
            {
                const uint8_t *tb = c + base + 12 + (key - lo) * 4;
                if ((size_t)(base + 12) < (size_t)codeLen)
                {
                    int32_t e = static_cast<int32_t>((tb[0] << 24) | (tb[1] << 16) | (tb[2] << 8) | tb[3]);
                    pc = base + e;
                }
                else
                {
                    pc = codeLen;
                }
            }
            break;
        }
        case 0xab:
        {
            while ((pc & 3) != 0) pc++;
            int base = pc;
            int def = rb32(c, pc);
            int npairs = rb32(c, pc);
            int key = popInt();
            int found = -1;
            for (int i = 0; i < npairs && (size_t)(base + 12 + (i + 1) * 8) <= (size_t)codeLen; i++)
            {
                const uint8_t *p = c + base + 12 + i * 8;
                int32_t cv = static_cast<int32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
                if (cv == key)
                {
                    int32_t e = static_cast<int32_t>((p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7]);
                    found = e;
                    break;
                }
            }
            pc = (found >= 0) ? base + found : base + def;
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
            auto fr = cp.getFieldRef(idx);
            if (fr.first.empty()) { okResult = false; done = true; break; }
            std::string classRef = fr.first;
            size_t colon = fr.second.find(':');
            std::string fn = fr.second.substr(0, colon);
            std::string fd = fr.second.substr(colon + 1);
            ClassInfo *tc = rt_->classInfoOfName(classRef);
            if (!tc) tc = rt_->loadFromJar(classRef);
            if (!tc)
            {
                fprintf(stderr, "JVM: get/putstatic: classe %s introuvable (0x%02x)\n", classRef.c_str(), op);
                okResult = false; done = true; break;
            }
            if (op == 0xb2)
            {
                ClassInfo *owner = tc;
                const MethodRecord *f = owner->findField(fn);
                while (!f && owner->super) { owner = owner->super; f = owner->findField(fn); }
                if (!f)
                {
                    fprintf(stderr, "JVM: get/putstatic: champ %s.%s introuvable (0x%02x)\n",
                            tc->name.c_str(), fn.c_str(), op);
                    okResult = false; done = true; break;
                }
                int w = slotsOfDesc(fd);
                if (w == 2) pushLong(owner->statics[f->slot].l);
                else push(owner->statics[f->slot], 1);
            }
            else
            {
                int w = slotsOfDesc(fd);
                Value v = (w == 2) ? Value::fromLong(popLong()) : pop();
                ClassInfo *owner = tc;
                const MethodRecord *f = owner->findField(fn);
                while (!f && owner->super) { owner = owner->super; f = owner->findField(fn); }
                if (!f)
                {
                    fprintf(stderr, "JVM: get/putstatic: champ %s.%s introuvable (0x%02x)\n",
                            tc->name.c_str(), fn.c_str(), op);
                    okResult = false; done = true; break;
                }
                owner->statics[f->slot] = v;
            }
            break;
        }
        case 0xb4: case 0xb5:
        {
            uint16_t idx = rbu16(c, pc);
            auto fr = cp.getFieldRef(idx);
            if (fr.first.empty()) { okResult = false; done = true; break; }
            size_t colon = fr.second.find(':');
            std::string fn = fr.second.substr(0, colon);
            std::string fd = fr.second.substr(colon + 1);
            int w = slotsOfDesc(fd);
            if (op == 0xb5)
            {
                Value v = (w == 2) ? Value::fromLong(popLong()) : pop();
                Obj *o = popRef();
                if (!o || o->kind != ObjKind::Instance) { okResult = false; done = true; break; }
                const MethodRecord *ff = o->cls->findFieldRecursive(fn);
                if (!ff)
                {
                    fprintf(stderr, "JVM: putfield: champ %s.%s introuvable (idx=%u owner=%s fd=%s) (0x%02x)\n",
                            o->cls ? o->cls->name.c_str() : "(null)", fn.c_str(), idx,
                            fr.first.c_str(), fd.c_str(), op);
                    okResult = false; done = true; break;
                }
                o->cells[ff->slot] = v;
                break;
            }
            Obj *o = popRef();
            if (!o || o->kind != ObjKind::Instance) { okResult = false; done = true; break; }
            const MethodRecord *ff = o->cls->findFieldRecursive(fn);
            if (!ff)
            {
                fprintf(stderr, "JVM: getfield: champ %s.%s introuvable (idx=%u owner=%s fd=%s) (0x%02x)\n",
                        o->cls ? o->cls->name.c_str() : "(null)", fn.c_str(), idx,
                        fr.first.c_str(), fd.c_str(), op);
                okResult = false; done = true; break;
            }
            if (w == 2) pushLong(o->cells[ff->slot].l);
            else push(o->cells[ff->slot], 1);
            break;
        }

        case 0xb6: case 0xb7: case 0xb8: case 0xb9:
        {
            uint16_t idx = rbu16(c, pc);
            auto mr = cp.getMethodRef(idx);
            if (mr.first.empty()) { okResult = false; done = true; break; }
            std::string classRef = mr.first;
            ClassInfo *tc = rt_->classInfoOfName(classRef);
            if (!tc && rt_->jar()) tc = rt_->loadFromJar(classRef);
            size_t colon = mr.second.find(':');
            std::string mname = mr.second.substr(0, colon);
            std::string mdesc = mr.second.substr(colon + 1);
            int nslots = argSlots(mdesc);
            bool rv = returnIsVoid(mdesc);
            Value mres;
            bool ok = false;
            if (op == 0xb8)
            {
                if (tc) ok = invokeStatic(tc, mname, mdesc, nslots, &st[sp - nslots], mres);
                sp -= nslots;
            }
            else
            {
                Obj *receiver = st[sp - nslots - 1].o;
                Value *argsPtr = st + (sp - nslots - 1);
                if (op == 0xb7)
                    ok = invokeSpecial(tc, mname, mdesc, receiver, argsPtr, nslots + 1, mres);
                else
                    ok = invokeVirtual(tc, mname, mdesc, receiver, argsPtr, nslots + 1, mres);
                sp -= nslots + 1;
            }
            if (!rv)
            {
                size_t rp = mdesc.find(')');
                char rt = (rp != std::string::npos && rp + 1 < mdesc.size()) ? mdesc[rp + 1] : 'V';
                if (rt == 'J' || rt == 'D') pushLong(mres.l);
                else push(mres, 1);
            }
            if (!ok && getenv("JME_DEBUG"))
                fprintf(stderr, "invokeEchec %s.%s%s (caller=%s.%s pc=%d)\n",
                        tc ? tc->name.c_str() : "?", mname.c_str(), mdesc.c_str(),
                        cls->name.c_str(), m->name.c_str(), pc);
            if (!ok) { okResult = false; done = true; }
            break;
        }

        case 0xbb:
        {
            uint16_t idx = rbu16(c, pc);
            std::string classRef = cp.getClassName(idx);
            ClassInfo *tc = rt_->classInfoOfName(classRef);
            if (!tc && rt_->jar()) tc = rt_->loadFromJar(classRef);
            if (!tc) { okResult = false; done = true; break; }
            if (!ensureInit(tc)) { okResult = false; done = true; break; }
            Obj *o = rt_->heap().newInstance(tc);
            if (!o) { rt_->reportOom(); okResult = false; done = true; break; }
            pushRef(o);
            break;
        }
        case 0xbc:
        {
            uint8_t atype = rb(c, pc);
            int count = popInt();
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
            ClassInfo *tc = rt_->classInfoOfName(classRef);
            Obj *o = (sp >= 1) ? st[sp - 1].o : nullptr;
            bool is = false;
            if (o && o->kind == ObjKind::Instance)
            {
                ClassInfo *oc = o->cls;
                while (oc) { if (!tc || oc == tc) { is = true; break; } oc = oc->super; }
            }
            if (op == 0xc1)
                st[sp - 1].i = is ? 1 : 0;
            else if (o && !is)
            {
                fprintf(stderr, "JVM: checkcast fail vers %s\n", classRef.c_str());
                okResult = false; done = true;
            }
            break;
        }
        case 0xbe:
        {
            Obj *a = popRef();
            if (!a) { okResult = false; done = true; break; }
            pushInt(a->arrayLen);
            break;
        }
        case 0xbf:
        {
            Obj *ex = popRef();
            fprintf(stderr, "JVM: exception non gérée (athrow), ex=%p\n", (void *)ex);
            okResult = false; done = true;
            break;
        }
        case 0xc2: case 0xc3: break;

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
            rbu16(c, pc);
            uint8_t dims = rb(c, pc);
            (void)dims;
            // implémentation simple : un seul tableau de taille = count total du dernier dim
            int total = 1;
            for (int i = 0; i < dims; i++)
                total *= popInt();
            Obj *a = rt_->heap().newArray(ObjKind::ObjArray, total);
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
    frameFree(mark);
    return okResult;
}

} // namespace jvm