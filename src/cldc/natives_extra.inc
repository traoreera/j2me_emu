// natives_extra.inc -- compléments CLDC 1.1 (java.lang / java.util), inclus dans natives.cpp
// à l'intérieur de son namespace anonyme : réutilise argRef/argInt/.../vecData/htTable/keyEq.
//
// Convention d'arguments (cf. Interpreter::dispatch) : les natives reçoivent les SLOTS bruts de
// la pile. Un long/double occupe 2 slots (valeur dans le 1er) : `pow(DD)` lit args[0] et args[2].
// Pour une méthode d'instance, args[0] = this.

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
double argDouble(NativeContext *ctx, int i)
{
    double d = 0;
    if (i >= 0 && i < ctx->nargs)
        std::memcpy(&d, &ctx->args[i].l, 8);
    return d;
}
void setFloatResult(NativeContext *ctx, float v)
{
    if (ctx->result)
        *ctx->result = Value::fromFloat(v);
}
void setDoubleResult(NativeContext *ctx, double v)
{
    int64_t b;
    std::memcpy(&b, &v, 8);
    setLongResult(ctx, b);
}
Obj *newStr(NativeContext *ctx, const std::string &s) { return ctx->rt->heap().newString(s); }
// Instance d'une classe native/JAR dont le champ 0 (cells[0]) porte la valeur.
Obj *newBoxed(NativeContext *ctx, const char *cls, Value v)
{
    ClassInfo *c = clsOf(ctx, cls);
    Obj *o = c ? ctx->rt->heap().newInstance(c) : nullptr;
    if (o && o->cells)
        o->cells[0] = v;
    return o;
}
std::string fmtDouble(double d)
{
    // Format Java : "3.0", "1.5E10" ... approximé (pas de repr. la plus courte garantie).
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    char b[40];
    snprintf(b, sizeof b, "%.15g", d);
    std::string s = b;
    if (s.find_first_of(".eEn") == std::string::npos)
        s += ".0";
    return s;
}
std::string fmtFloat(float f)
{
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f > 0 ? "Infinity" : "-Infinity";
    char b[40];
    snprintf(b, sizeof b, "%.7g", static_cast<double>(f));
    std::string s = b;
    if (s.find_first_of(".eEn") == std::string::npos)
        s += ".0";
    return s;
}

// ---------------------------------------------------------------------------------------------
// java.lang.String
// ---------------------------------------------------------------------------------------------
void x_String_hashCode(NativeContext *ctx)
{
    int32_t h = 0;
    for (unsigned char c : strOf(ctx->thisObj))
        h = static_cast<int32_t>(static_cast<uint32_t>(h) * 31u + c);
    setIntResult(ctx, h);
}
void x_String_getBytes(NativeContext *ctx)
{
    std::string enc8;
    const std::string &s0 = strOf(ctx->thisObj);
    if (ctx->nargs > 1 && encIsUtf8(argRef(ctx, 1))) enc8 = latin1ToUtf8(s0);
    const std::string &s = enc8.empty() && !s0.empty() ? s0 : (enc8.empty() ? s0 : enc8);
    Obj *o = ctx->rt->heap().newArray(ObjKind::ByteArray, static_cast<int32_t>(s.size()));
    if (o)
        for (size_t i = 0; i < s.size(); i++)
            o->cells[i].u = static_cast<unsigned char>(s[i]);
    setRefResult(ctx, o);
}
void x_String_lastIndexOfChar(NativeContext *ctx)
{
    const std::string &s = strOf(ctx->thisObj);
    size_t p = s.rfind(static_cast<char>(argInt(ctx, 1)));
    setIntResult(ctx, p == std::string::npos ? -1 : static_cast<int32_t>(p));
}
void x_String_lastIndexOfCharFrom(NativeContext *ctx)
{
    const std::string &s = strOf(ctx->thisObj);
    int from = argInt(ctx, 2);
    if (from < 0) { setIntResult(ctx, -1); return; }
    size_t p = s.rfind(static_cast<char>(argInt(ctx, 1)), static_cast<size_t>(from));
    setIntResult(ctx, p == std::string::npos ? -1 : static_cast<int32_t>(p));
}
void x_String_lastIndexOfStr(NativeContext *ctx)
{
    size_t p = strOf(ctx->thisObj).rfind(strOf(argRef(ctx, 1)));
    setIntResult(ctx, p == std::string::npos ? -1 : static_cast<int32_t>(p));
}
void x_String_replace(NativeContext *ctx)
{
    std::string s = strOf(ctx->thisObj);
    char a = static_cast<char>(argInt(ctx, 1)), b = static_cast<char>(argInt(ctx, 2));
    for (char &c : s)
        if (c == a) c = b;
    setRefResult(ctx, newStr(ctx, s));
}
void x_String_startsWithFrom(NativeContext *ctx)
{
    const std::string &s = strOf(ctx->thisObj), &p = strOf(argRef(ctx, 1));
    int off = argInt(ctx, 2);
    setIntResult(ctx, (off >= 0 && (size_t)off + p.size() <= s.size() && s.compare(off, p.size(), p) == 0) ? 1 : 0);
}
void x_String_toString(NativeContext *ctx) { setRefResult(ctx, ctx->thisObj); }
void x_String_isEmpty(NativeContext *ctx) { setIntResult(ctx, strOf(ctx->thisObj).empty() ? 1 : 0); }
void x_String_compareToIgnoreCase(NativeContext *ctx)
{
    std::string a = strOf(ctx->thisObj), b = strOf(argRef(ctx, 1));
    for (char &c : a) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (char &c : b) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    int r = a.compare(b);
    setIntResult(ctx, r < 0 ? -1 : (r > 0 ? 1 : 0));
}
void x_String_initStr(NativeContext *ctx) // new String(String) / new String(StringBuffer)
{
    Obj *self = argRef(ctx, 0), *src = argRef(ctx, 1);
    if (!self) return;
    self->kind = ObjKind::String;
    if (src && src->kind == ObjKind::String)
        self->str = src->str;
    else
        self->str = sbStr(src); // StringBuffer
}
void x_String_initChars(NativeContext *ctx) // new String(char[])
{
    Obj *self = argRef(ctx, 0), *a = argRef(ctx, 1);
    if (!self) return;
    self->kind = ObjKind::String;
    self->str.clear();
    if (a && a->cells)
        for (int i = 0; i < a->arrayLen; i++)
            self->str += static_cast<char>(a->cells[i].u & 0xFF);
}
void x_String_valueOfChar(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, std::string(1, static_cast<char>(argInt(ctx, 0))))); }
void x_String_valueOfLong(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, itos(argLong(ctx, 0)))); }
void x_String_valueOfBool(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, argInt(ctx, 0) ? "true" : "false")); }
void x_String_valueOfFloat(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, fmtFloat(argFloat(ctx, 0)))); }
void x_String_valueOfDouble(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, fmtDouble(argDouble(ctx, 0)))); }
void x_String_valueOfChars(NativeContext *ctx)
{
    Obj *a = argRef(ctx, 0);
    std::string s;
    if (a && a->cells)
        for (int i = 0; i < a->arrayLen; i++)
            s += static_cast<char>(a->cells[i].u & 0xFF);
    setRefResult(ctx, newStr(ctx, s));
}
// String.valueOf(Object) : "null", la chaîne elle-même, ou toString() virtuel (bytecode ou natif).
void x_String_valueOfObject(NativeContext *ctx)
{
    Obj *o = argRef(ctx, 0);
    if (!o) { setRefResult(ctx, newStr(ctx, "null")); return; }
    if (o->kind == ObjKind::String) { setRefResult(ctx, o); return; }
    if (o->kind == ObjKind::Instance && o->cls && ctx->interp)
    {
        Value a[1] = {Value::fromRef(o)}, r;
        if (ctx->interp->invokeVirtual(o->cls, "toString", "()Ljava/lang/String;", o, a, 1, r) && r.o)
        {
            setRefResult(ctx, r.o);
            return;
        }
    }
    setRefResult(ctx, newStr(ctx, o->cls ? o->cls->name : "Object"));
}

// ---------------------------------------------------------------------------------------------
// java.lang.StringBuffer (cells[0] = String courante)
// ---------------------------------------------------------------------------------------------
void x_SB_insertStr(NativeContext *ctx)
{
    std::string s = sbStr(ctx->thisObj);
    int at = argInt(ctx, 1);
    if (at < 0) at = 0;
    if ((size_t)at > s.size()) at = static_cast<int>(s.size());
    Obj *o = argRef(ctx, 2);
    s.insert(at, o && o->kind == ObjKind::String ? o->str : std::string("null"));
    n_SB_setStr(ctx, s);
}
void x_SB_insertChar(NativeContext *ctx)
{
    std::string s = sbStr(ctx->thisObj);
    int at = argInt(ctx, 1);
    if (at < 0) at = 0;
    if ((size_t)at > s.size()) at = static_cast<int>(s.size());
    s.insert(s.begin() + at, static_cast<char>(argInt(ctx, 2)));
    n_SB_setStr(ctx, s);
}
void x_SB_insertInt(NativeContext *ctx)
{
    std::string s = sbStr(ctx->thisObj);
    int at = argInt(ctx, 1);
    if (at < 0) at = 0;
    if ((size_t)at > s.size()) at = static_cast<int>(s.size());
    s.insert(at, itos(argInt(ctx, 2)));
    n_SB_setStr(ctx, s);
}
void x_SB_reverse(NativeContext *ctx)
{
    std::string s = sbStr(ctx->thisObj);
    std::reverse(s.begin(), s.end());
    n_SB_setStr(ctx, s);
}
void x_SB_deleteCharAt(NativeContext *ctx)
{
    std::string s = sbStr(ctx->thisObj);
    int at = argInt(ctx, 1);
    if (at >= 0 && (size_t)at < s.size())
        s.erase(at, 1);
    n_SB_setStr(ctx, s);
}
void x_SB_capacity(NativeContext *ctx) { setIntResult(ctx, static_cast<int32_t>(sbStr(ctx->thisObj).size()) + 16); }
void x_SB_ensureCapacity(NativeContext *) {}
void x_SB_appendCharsRange(NativeContext *ctx) // append(char[], int off, int len)
{
    Obj *a = argRef(ctx, 1);
    int off = argInt(ctx, 2), len = argInt(ctx, 3);
    std::string s = sbStr(ctx->thisObj);
    if (a && a->cells)
        for (int i = off; i < off + len && i < a->arrayLen; i++)
            if (i >= 0) s += static_cast<char>(a->cells[i].u & 0xFF);
    n_SB_setStr(ctx, s);
}
void x_SB_indexOf(NativeContext *ctx)
{
    size_t p = sbStr(ctx->thisObj).find(strOf(argRef(ctx, 1)));
    setIntResult(ctx, p == std::string::npos ? -1 : static_cast<int32_t>(p));
}
void x_SB_substring(NativeContext *ctx)
{
    std::string s = sbStr(ctx->thisObj);
    int a = argInt(ctx, 1), b = ctx->nargs > 2 ? argInt(ctx, 2) : static_cast<int>(s.size());
    if (a < 0) a = 0;
    if ((size_t)b > s.size()) b = static_cast<int>(s.size());
    setRefResult(ctx, newStr(ctx, a < b ? s.substr(a, b - a) : std::string()));
}

// ---------------------------------------------------------------------------------------------
// java.lang.Math
// ---------------------------------------------------------------------------------------------
#define X_MATH1(NAME, EXPR) \
    void x_Math_##NAME(NativeContext *ctx) { double x = argDouble(ctx, 0); setDoubleResult(ctx, EXPR); }
X_MATH1(sin, std::sin(x))
X_MATH1(cos, std::cos(x))
X_MATH1(tan, std::tan(x))
X_MATH1(asin, std::asin(x))
X_MATH1(acos, std::acos(x))
X_MATH1(atan, std::atan(x))
X_MATH1(exp, std::exp(x))
X_MATH1(log, std::log(x))
X_MATH1(toRadians, x / 180.0 * 3.141592653589793)
X_MATH1(toDegrees, x * 180.0 / 3.141592653589793)
X_MATH1(absD, std::fabs(x))
#undef X_MATH1
void x_Math_atan2(NativeContext *ctx) { setDoubleResult(ctx, std::atan2(argDouble(ctx, 0), argDouble(ctx, 2))); }
void x_Math_pow(NativeContext *ctx) { setDoubleResult(ctx, std::pow(argDouble(ctx, 0), argDouble(ctx, 2))); }
void x_Math_absF(NativeContext *ctx) { setFloatResult(ctx, std::fabs(argFloat(ctx, 0))); }
void x_Math_minF(NativeContext *ctx) { setFloatResult(ctx, std::fmin(argFloat(ctx, 0), argFloat(ctx, 1))); }
void x_Math_maxF(NativeContext *ctx) { setFloatResult(ctx, std::fmax(argFloat(ctx, 0), argFloat(ctx, 1))); }
void x_Math_minD(NativeContext *ctx) { setDoubleResult(ctx, std::fmin(argDouble(ctx, 0), argDouble(ctx, 2))); }
void x_Math_maxD(NativeContext *ctx) { setDoubleResult(ctx, std::fmax(argDouble(ctx, 0), argDouble(ctx, 2))); }
void x_Math_roundF(NativeContext *ctx)
{
    float f = argFloat(ctx, 0);
    setIntResult(ctx, std::isnan(f) ? 0 : static_cast<int32_t>(std::floor(f + 0.5f)));
}
void x_Math_roundD(NativeContext *ctx)
{
    double d = argDouble(ctx, 0);
    setLongResult(ctx, std::isnan(d) ? 0 : static_cast<int64_t>(std::floor(d + 0.5)));
}

// ---------------------------------------------------------------------------------------------
// java.lang.Integer / Long / Short / Byte / Character / Float / Double / Boolean
// ---------------------------------------------------------------------------------------------
std::string radixStr(uint64_t v, int radix)
{
    if (v == 0) return "0";
    std::string s;
    while (v) { s += "0123456789abcdefghijklmnopqrstuvwxyz"[v % radix]; v /= radix; }
    std::reverse(s.begin(), s.end());
    return s;
}
void x_Int_parseIntRadix(NativeContext *ctx)
{
    int64_t v;
    int r = argInt(ctx, 1);
    if (!parseJavaInt(strOf(argRef(ctx, 0)), (r < 2 || r > 36) ? 10 : r, INT32_MIN, INT32_MAX, v))
    {
        throwJava(ctx, "java/lang/NumberFormatException");
        return;
    }
    setIntResult(ctx, static_cast<int32_t>(v));
}
void x_Int_toHex(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, radixStr(static_cast<uint32_t>(argInt(ctx, 0)), 16))); }
void x_Int_toBin(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, radixStr(static_cast<uint32_t>(argInt(ctx, 0)), 2))); }
void x_Int_toOct(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, radixStr(static_cast<uint32_t>(argInt(ctx, 0)), 8))); }
void x_Int_toStringRadix(NativeContext *ctx)
{
    int v = argInt(ctx, 0), r = argInt(ctx, 1);
    if (r < 2 || r > 36) r = 10;
    setRefResult(ctx, newStr(ctx, (v < 0 ? "-" : "") + radixStr(v < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(v)) : static_cast<uint64_t>(v), r)));
}
void x_Int_valueOfStr(NativeContext *ctx)
{
    int64_t v;
    if (!parseJavaInt(strOf(argRef(ctx, 0)), 10, INT32_MIN, INT32_MAX, v))
    {
        throwJava(ctx, "java/lang/NumberFormatException");
        return;
    }
    setRefResult(ctx, newBoxed(ctx, "java/lang/Integer", Value::fromInt(static_cast<int32_t>(v))));
}

// Long : cells[0].l
int64_t boxL(Obj *o) { return (o && o->cells) ? o->cells[0].l : 0; }
void x_Long_init(NativeContext *ctx) { if (ctx->thisObj && ctx->thisObj->cells) ctx->thisObj->cells[0] = Value::fromLong(argLong(ctx, 1)); }
void x_Long_longValue(NativeContext *ctx) { setLongResult(ctx, boxL(ctx->thisObj)); }
void x_Long_intValue(NativeContext *ctx) { setIntResult(ctx, static_cast<int32_t>(boxL(ctx->thisObj))); }
void x_Long_toString(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, itos(boxL(ctx->thisObj)))); }
void x_Long_toStringS(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, itos(argLong(ctx, 0)))); }
void x_Long_parse(NativeContext *ctx)
{
    int64_t v;
    if (!parseJavaInt(strOf(argRef(ctx, 0)), 10, INT64_MIN / 2, INT64_MAX / 2, v)) { throwJava(ctx, "java/lang/NumberFormatException"); return; }
    setLongResult(ctx, v);
}
void x_Long_valueOfL(NativeContext *ctx) { setRefResult(ctx, newBoxed(ctx, "java/lang/Long", Value::fromLong(argLong(ctx, 0)))); }
void x_Long_valueOfS(NativeContext *ctx)
{
    int64_t v;
    if (!parseJavaInt(strOf(argRef(ctx, 0)), 10, INT64_MIN / 2, INT64_MAX / 2, v)) { throwJava(ctx, "java/lang/NumberFormatException"); return; }
    setRefResult(ctx, newBoxed(ctx, "java/lang/Long", Value::fromLong(v)));
}
void x_Long_hashCode(NativeContext *ctx) { int64_t v = boxL(ctx->thisObj); setIntResult(ctx, static_cast<int32_t>(v ^ (static_cast<uint64_t>(v) >> 32))); }
void x_Long_equals(NativeContext *ctx)
{
    Obj *k = argRef(ctx, 1);
    setIntResult(ctx, (k && k->kind == ObjKind::Instance && ctx->thisObj && k->cls == ctx->thisObj->cls && boxL(k) == boxL(ctx->thisObj)) ? 1 : 0);
}

// Short / Byte : cells[0].i (valeur déjà réduite à son type par le bytecode appelant)
void x_Small_init(NativeContext *ctx) { if (ctx->thisObj && ctx->thisObj->cells) ctx->thisObj->cells[0] = Value::fromInt(argInt(ctx, 1)); }
void x_Small_value(NativeContext *ctx) { setIntResult(ctx, ctx->thisObj && ctx->thisObj->cells ? ctx->thisObj->cells[0].i : 0); }
void x_Small_toString(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, itos(ctx->thisObj && ctx->thisObj->cells ? ctx->thisObj->cells[0].i : 0))); }
void x_Short_parse(NativeContext *ctx)
{
    int64_t v;
    if (!parseJavaInt(strOf(argRef(ctx, 0)), 10, -32768, 32767, v)) { throwJava(ctx, "java/lang/NumberFormatException"); return; }
    setIntResult(ctx, static_cast<int32_t>(v));
}
void x_Byte_parse(NativeContext *ctx)
{
    int64_t v;
    if (!parseJavaInt(strOf(argRef(ctx, 0)), 10, -128, 127, v)) { throwJava(ctx, "java/lang/NumberFormatException"); return; }
    setIntResult(ctx, static_cast<int32_t>(v));
}

// Character : cells[0].i
void x_Chr_init(NativeContext *ctx) { if (ctx->thisObj && ctx->thisObj->cells) ctx->thisObj->cells[0] = Value::fromInt(argInt(ctx, 1) & 0xFFFF); }
void x_Chr_charValue(NativeContext *ctx) { setIntResult(ctx, ctx->thisObj && ctx->thisObj->cells ? ctx->thisObj->cells[0].i : 0); }
void x_Chr_toString(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, std::string(1, static_cast<char>(ctx->thisObj && ctx->thisObj->cells ? ctx->thisObj->cells[0].i : 0)))); }
void x_Chr_toStringS(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, std::string(1, static_cast<char>(argInt(ctx, 0))))); }
void x_Chr_isDigit(NativeContext *ctx) { int c = argInt(ctx, 0); setIntResult(ctx, (c >= '0' && c <= '9') ? 1 : 0); }
void x_Chr_isLetter(NativeContext *ctx) { int c = argInt(ctx, 0); setIntResult(ctx, ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0) ? 1 : 0); }
void x_Chr_isLetterOrDigit(NativeContext *ctx) { int c = argInt(ctx, 0); setIntResult(ctx, ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0) ? 1 : 0); }
void x_Chr_isUpper(NativeContext *ctx) { int c = argInt(ctx, 0); setIntResult(ctx, (c >= 'A' && c <= 'Z') ? 1 : 0); }
void x_Chr_isLower(NativeContext *ctx) { int c = argInt(ctx, 0); setIntResult(ctx, (c >= 'a' && c <= 'z') ? 1 : 0); }
void x_Chr_isSpace(NativeContext *ctx) { int c = argInt(ctx, 0); setIntResult(ctx, (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') ? 1 : 0); }
void x_Chr_toUpper(NativeContext *ctx) { int c = argInt(ctx, 0); setIntResult(ctx, (c >= 'a' && c <= 'z') ? c - 32 : c); }
void x_Chr_toLower(NativeContext *ctx) { int c = argInt(ctx, 0); setIntResult(ctx, (c >= 'A' && c <= 'Z') ? c + 32 : c); }
void x_Chr_digit(NativeContext *ctx)
{
    int c = argInt(ctx, 0), r = argInt(ctx, 1), v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    setIntResult(ctx, (v >= 0 && v < r) ? v : -1);
}
void x_Chr_forDigit(NativeContext *ctx)
{
    int d = argInt(ctx, 0), r = argInt(ctx, 1);
    setIntResult(ctx, (d >= 0 && d < r && r <= 36) ? "0123456789abcdefghijklmnopqrstuvwxyz"[d] : 0);
}
void x_Chr_valueOf(NativeContext *ctx) { setRefResult(ctx, newBoxed(ctx, "java/lang/Character", Value::fromInt(argInt(ctx, 0) & 0xFFFF))); }
void x_Chr_hashCode(NativeContext *ctx) { setIntResult(ctx, ctx->thisObj && ctx->thisObj->cells ? ctx->thisObj->cells[0].i : 0); }
void x_Chr_equals(NativeContext *ctx)
{
    Obj *k = argRef(ctx, 1);
    setIntResult(ctx, (k && k->kind == ObjKind::Instance && ctx->thisObj && k->cls == ctx->thisObj->cls && k->cells[0].i == ctx->thisObj->cells[0].i) ? 1 : 0);
}

// Float : cells[0].f ; Double : cells[0].l (bits)
float boxF(Obj *o) { return (o && o->cells) ? o->cells[0].f : 0.f; }
double boxD(Obj *o) { double d = 0; if (o && o->cells) std::memcpy(&d, &o->cells[0].l, 8); return d; }
void x_Float_init(NativeContext *ctx) { if (ctx->thisObj && ctx->thisObj->cells) ctx->thisObj->cells[0] = Value::fromFloat(argFloat(ctx, 1)); }
void x_Float_floatValue(NativeContext *ctx) { setFloatResult(ctx, boxF(ctx->thisObj)); }
void x_Float_doubleValue(NativeContext *ctx) { setDoubleResult(ctx, boxF(ctx->thisObj)); }
void x_Float_intValue(NativeContext *ctx) { setIntResult(ctx, static_cast<int32_t>(boxF(ctx->thisObj))); }
void x_Float_toString(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, fmtFloat(boxF(ctx->thisObj)))); }
void x_Float_toStringS(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, fmtFloat(argFloat(ctx, 0)))); }
bool parseJavaDouble(const std::string &t0, double &out)
{
    std::string t = t0;
    while (!t.empty() && isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    size_t b = t.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return false;
    t = t.substr(b);
    char *end = nullptr;
    out = strtod(t.c_str(), &end);
    return end && *end == 0;
}
void x_Float_parse(NativeContext *ctx)
{
    double d;
    if (!parseJavaDouble(strOf(argRef(ctx, 0)), d)) { throwJava(ctx, "java/lang/NumberFormatException"); return; }
    setFloatResult(ctx, static_cast<float>(d));
}
void x_Float_valueOfS(NativeContext *ctx)
{
    double d;
    if (!parseJavaDouble(strOf(argRef(ctx, 0)), d)) { throwJava(ctx, "java/lang/NumberFormatException"); return; }
    setRefResult(ctx, newBoxed(ctx, "java/lang/Float", Value::fromFloat(static_cast<float>(d))));
}
void x_Float_valueOfF(NativeContext *ctx) { setRefResult(ctx, newBoxed(ctx, "java/lang/Float", Value::fromFloat(argFloat(ctx, 0)))); }
void x_Float_isNaN(NativeContext *ctx) { setIntResult(ctx, std::isnan(argFloat(ctx, 0)) ? 1 : 0); }
void x_Float_isNaNi(NativeContext *ctx) { setIntResult(ctx, std::isnan(boxF(ctx->thisObj)) ? 1 : 0); }
void x_Float_isInf(NativeContext *ctx) { setIntResult(ctx, std::isinf(argFloat(ctx, 0)) ? 1 : 0); }
void x_Float_toBits(NativeContext *ctx) { float f = argFloat(ctx, 0); int32_t b; std::memcpy(&b, &f, 4); setIntResult(ctx, b); }
void x_Float_fromBits(NativeContext *ctx) { int32_t b = argInt(ctx, 0); float f; std::memcpy(&f, &b, 4); setFloatResult(ctx, f); }
void x_Double_init(NativeContext *ctx) { if (ctx->thisObj && ctx->thisObj->cells) ctx->thisObj->cells[0] = Value::fromLong(ctx->args[1].l); }
void x_Double_doubleValue(NativeContext *ctx) { setDoubleResult(ctx, boxD(ctx->thisObj)); }
void x_Double_floatValue(NativeContext *ctx) { setFloatResult(ctx, static_cast<float>(boxD(ctx->thisObj))); }
void x_Double_intValue(NativeContext *ctx) { setIntResult(ctx, static_cast<int32_t>(boxD(ctx->thisObj))); }
void x_Double_toString(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, fmtDouble(boxD(ctx->thisObj)))); }
void x_Double_toStringS(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, fmtDouble(argDouble(ctx, 0)))); }
void x_Double_parse(NativeContext *ctx)
{
    double d;
    if (!parseJavaDouble(strOf(argRef(ctx, 0)), d)) { throwJava(ctx, "java/lang/NumberFormatException"); return; }
    setDoubleResult(ctx, d);
}
void x_Double_valueOfS(NativeContext *ctx)
{
    double d;
    if (!parseJavaDouble(strOf(argRef(ctx, 0)), d)) { throwJava(ctx, "java/lang/NumberFormatException"); return; }
    int64_t b; std::memcpy(&b, &d, 8);
    setRefResult(ctx, newBoxed(ctx, "java/lang/Double", Value::fromLong(b)));
}
void x_Double_valueOfD(NativeContext *ctx) { setRefResult(ctx, newBoxed(ctx, "java/lang/Double", Value::fromLong(ctx->args[0].l))); }
void x_Double_isNaN(NativeContext *ctx) { setIntResult(ctx, std::isnan(argDouble(ctx, 0)) ? 1 : 0); }
void x_Double_isNaNi(NativeContext *ctx) { setIntResult(ctx, std::isnan(boxD(ctx->thisObj)) ? 1 : 0); }
void x_Double_isInf(NativeContext *ctx) { setIntResult(ctx, std::isinf(argDouble(ctx, 0)) ? 1 : 0); }
void x_Double_toBits(NativeContext *ctx) { setLongResult(ctx, ctx->args[0].l); }
void x_Double_fromBits(NativeContext *ctx) { setLongResult(ctx, ctx->args[0].l); }

// ---------------------------------------------------------------------------------------------
// java.util : Enumeration (instantané), Vector / Hashtable / Stack compléments
// ---------------------------------------------------------------------------------------------
// ArrayEnumeration : cells[0] = ObjArray instantané, cells[1] = position.
Obj *makeEnumeration(NativeContext *ctx, const std::vector<Obj *> &items)
{
    ClassInfo *c = clsOf(ctx, "java/util/ArrayEnumeration");
    Obj *e = c ? ctx->rt->heap().newInstance(c) : nullptr;
    Obj *arr = ctx->rt->heap().newArray(ObjKind::ObjArray, static_cast<int32_t>(items.size()));
    if (!e || !arr) return nullptr;
    for (size_t i = 0; i < items.size(); i++)
        arr->cells[i] = Value::fromRef(items[i]);
    e->cells[0] = Value::fromRef(arr);
    e->cells[1] = Value::fromInt(0);
    return e;
}
void x_Enum_hasMore(NativeContext *ctx)
{
    Obj *e = ctx->thisObj;
    setIntResult(ctx, (e && e->cells && e->cells[0].o && e->cells[1].i < e->cells[0].o->arrayLen) ? 1 : 0);
}
void x_Enum_next(NativeContext *ctx)
{
    Obj *e = ctx->thisObj;
    if (e && e->cells && e->cells[0].o && e->cells[1].i < e->cells[0].o->arrayLen)
    {
        setRefResult(ctx, e->cells[0].o->cells[e->cells[1].i].o);
        e->cells[1] = Value::fromInt(e->cells[1].i + 1);
        return;
    }
    throwJava(ctx, "java/util/NoSuchElementException");
}
void x_Vec_elements(NativeContext *ctx)
{
    std::vector<Obj *> v;
    Obj *d = vecData(ctx);
    for (int i = 0, n = vecCount(ctx); d && i < n; i++)
        v.push_back(d->cells[i].o);
    setRefResult(ctx, makeEnumeration(ctx, v));
}
void x_Vec_copyInto(NativeContext *ctx)
{
    Obj *dst = argRef(ctx, 1), *d = vecData(ctx);
    for (int i = 0, n = vecCount(ctx); d && dst && dst->cells && i < n && i < dst->arrayLen; i++)
        dst->cells[i] = d->cells[i];
}
void x_Vec_setSize(NativeContext *ctx)
{
    int n = argInt(ctx, 1), old = vecCount(ctx);
    if (n < 0) return;
    vecEnsureCapacity(ctx, n);
    Obj *d = vecData(ctx);
    for (int i = n; d && i < old; i++)
        d->cells[i] = Value::fromRef(nullptr);
    vecSetCount(ctx, n);
}
void x_Vec_capacity(NativeContext *ctx) { Obj *d = vecData(ctx); setIntResult(ctx, d ? d->arrayLen : 0); }
void x_Vec_indexOfFrom(NativeContext *ctx)
{
    Obj *val = argRef(ctx, 1), *d = vecData(ctx);
    for (int i = argInt(ctx, 2) < 0 ? 0 : argInt(ctx, 2), n = vecCount(ctx); d && i < n; i++)
        if (keyEq(d->cells[i].o, val)) { setIntResult(ctx, i); return; }
    setIntResult(ctx, -1);
}
void x_Vec_lastIndexOf(NativeContext *ctx)
{
    Obj *val = argRef(ctx, 1), *d = vecData(ctx);
    for (int i = vecCount(ctx) - 1; d && i >= 0; i--)
        if (keyEq(d->cells[i].o, val)) { setIntResult(ctx, i); return; }
    setIntResult(ctx, -1);
}
void x_Vec_toString(NativeContext *ctx)
{
    std::string s = "[";
    Obj *d = vecData(ctx);
    for (int i = 0, n = vecCount(ctx); d && i < n; i++)
    {
        if (i) s += ", ";
        Obj *o = d->cells[i].o;
        if (o && o->kind == ObjKind::String) s += o->str;
        else if (o && o->kind == ObjKind::Instance && o->cls && o->cls->name == "java/lang/Integer") s += itos(o->cells[0].i);
        else s += o ? "obj" : "null";
    }
    setRefResult(ctx, newStr(ctx, s + "]"));
}
// Stack extends Vector : push/pop/peek/empty/search
void x_Stack_push(NativeContext *ctx)
{
    n_Vec_addElement(ctx);
    setRefResult(ctx, argRef(ctx, 1));
}
void x_Stack_pop(NativeContext *ctx)
{
    int n = vecCount(ctx);
    Obj *d = vecData(ctx);
    if (!d || n <= 0) { throwJava(ctx, "java/util/EmptyStackException"); return; }
    Obj *top = d->cells[n - 1].o;
    d->cells[n - 1] = Value::fromRef(nullptr);
    vecSetCount(ctx, n - 1);
    setRefResult(ctx, top);
}
void x_Stack_peek(NativeContext *ctx)
{
    int n = vecCount(ctx);
    Obj *d = vecData(ctx);
    if (!d || n <= 0) { throwJava(ctx, "java/util/EmptyStackException"); return; }
    setRefResult(ctx, d->cells[n - 1].o);
}
void x_Stack_empty(NativeContext *ctx) { setIntResult(ctx, vecCount(ctx) == 0 ? 1 : 0); }
void x_Stack_search(NativeContext *ctx)
{
    Obj *val = argRef(ctx, 1), *d = vecData(ctx);
    for (int i = vecCount(ctx) - 1; d && i >= 0; i--)
        if (keyEq(d->cells[i].o, val)) { setIntResult(ctx, vecCount(ctx) - i); return; }
    setIntResult(ctx, -1);
}
void x_HT_keys(NativeContext *ctx)
{
    std::vector<Obj *> v;
    Obj *t = htTable(ctx);
    for (int i = 0, n = htCount(ctx); t && i < n; i++)
        v.push_back(t->cells[i * 2].o);
    setRefResult(ctx, makeEnumeration(ctx, v));
}
void x_HT_elements(NativeContext *ctx)
{
    std::vector<Obj *> v;
    Obj *t = htTable(ctx);
    for (int i = 0, n = htCount(ctx); t && i < n; i++)
        v.push_back(t->cells[i * 2 + 1].o);
    setRefResult(ctx, makeEnumeration(ctx, v));
}
void x_HT_contains(NativeContext *ctx)
{
    Obj *val = argRef(ctx, 1), *t = htTable(ctx);
    for (int i = 0, n = htCount(ctx); t && i < n; i++)
        if (keyEq(t->cells[i * 2 + 1].o, val)) { setIntResult(ctx, 1); return; }
    setIntResult(ctx, 0);
}
void x_HT_initCap(NativeContext *ctx) { n_HT_init(ctx); }

// javax.microedition.io.Connector : aucun réseau/fichier émulé -> ConnectionNotFoundException (les jeux
// bien écrits la rattrapent et continuent hors-ligne).
void x_Connector_open(NativeContext *ctx) { throwJava(ctx, "javax/microedition/io/ConnectionNotFoundException"); }

// ---------------------------------------------------------------------------------------------
// java.util.Date / Calendar / TimeZone (heure locale = horloge murale + horloge virtuelle du jeu)
// ---------------------------------------------------------------------------------------------
int64_t wallMillis()
{
    static const int64_t base = static_cast<int64_t>(time(nullptr)) * 1000;
    return base + virtualMillis();
}
void x_Date_init0(NativeContext *ctx) { if (ctx->thisObj && ctx->thisObj->cells) ctx->thisObj->cells[0] = Value::fromLong(wallMillis()); }
void x_Date_initL(NativeContext *ctx) { if (ctx->thisObj && ctx->thisObj->cells) ctx->thisObj->cells[0] = Value::fromLong(argLong(ctx, 1)); }
void x_Date_getTime(NativeContext *ctx) { setLongResult(ctx, boxL(ctx->thisObj)); }
void x_Date_setTime(NativeContext *ctx) { if (ctx->thisObj && ctx->thisObj->cells) ctx->thisObj->cells[0] = Value::fromLong(argLong(ctx, 1)); }
void x_Date_toString(NativeContext *ctx)
{
    time_t t = static_cast<time_t>(boxL(ctx->thisObj) / 1000);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[64];
    strftime(b, sizeof b, "%a %b %d %H:%M:%S %Y", &tmv);
    setRefResult(ctx, newStr(ctx, b));
}
void x_Cal_getInstance(NativeContext *ctx)
{
    setRefResult(ctx, newBoxed(ctx, "java/util/Calendar", Value::fromLong(wallMillis())));
}
void x_Cal_get(NativeContext *ctx)
{
    time_t t = static_cast<time_t>(boxL(ctx->thisObj) / 1000);
    struct tm tmv;
    localtime_r(&t, &tmv);
    int64_t ms = boxL(ctx->thisObj) % 1000;
    switch (argInt(ctx, 1))
    {
    case 1: setIntResult(ctx, tmv.tm_year + 1900); break;   // YEAR
    case 2: setIntResult(ctx, tmv.tm_mon); break;           // MONTH (0..11)
    case 5: setIntResult(ctx, tmv.tm_mday); break;          // DAY_OF_MONTH
    case 7: setIntResult(ctx, tmv.tm_wday + 1); break;      // DAY_OF_WEEK (dimanche = 1)
    case 9: setIntResult(ctx, tmv.tm_hour >= 12 ? 1 : 0); break; // AM_PM
    case 10: setIntResult(ctx, tmv.tm_hour % 12); break;    // HOUR
    case 11: setIntResult(ctx, tmv.tm_hour); break;         // HOUR_OF_DAY
    case 12: setIntResult(ctx, tmv.tm_min); break;          // MINUTE
    case 13: setIntResult(ctx, tmv.tm_sec); break;          // SECOND
    case 14: setIntResult(ctx, static_cast<int32_t>(ms)); break; // MILLISECOND
    case 6: setIntResult(ctx, tmv.tm_yday + 1); break;      // DAY_OF_YEAR
    default: setIntResult(ctx, 0); break;
    }
}
void x_Cal_getTime(NativeContext *ctx) { setRefResult(ctx, newBoxed(ctx, "java/util/Date", Value::fromLong(boxL(ctx->thisObj)))); }
void x_Cal_setTime(NativeContext *ctx) { if (ctx->thisObj && ctx->thisObj->cells) ctx->thisObj->cells[0] = Value::fromLong(boxL(argRef(ctx, 1))); }
void x_TZ_getDefault(NativeContext *ctx) { setRefResult(ctx, newBoxed(ctx, "java/util/TimeZone", Value::fromInt(0))); }
void x_TZ_getID(NativeContext *ctx) { setRefResult(ctx, newStr(ctx, "GMT")); }

// ---------------------------------------------------------------------------------------------
// java.io.PrintStream (System.out / System.err -> stdout)
// ---------------------------------------------------------------------------------------------
std::string psText(NativeContext *ctx, Obj *o)
{
    if (!o) return "null";
    if (o->kind == ObjKind::String) return o->str;
    if (o->kind == ObjKind::Instance && o->cls && ctx->interp)
    {
        Value a[1] = {Value::fromRef(o)}, r;
        if (ctx->interp->invokeVirtual(o->cls, "toString", "()Ljava/lang/String;", o, a, 1, r) && r.o && r.o->kind == ObjKind::String)
            return r.o->str;
    }
    return o->cls ? o->cls->name : "Object";
}
void x_PS_printlnObj(NativeContext *ctx) { printf("%s\n", psText(ctx, argRef(ctx, 1)).c_str()); }
void x_PS_printObj(NativeContext *ctx) { printf("%s", psText(ctx, argRef(ctx, 1)).c_str()); }
void x_PS_printlnChar(NativeContext *ctx) { printf("%c\n", static_cast<char>(argInt(ctx, 1))); }
void x_PS_printChar(NativeContext *ctx) { printf("%c", static_cast<char>(argInt(ctx, 1))); }
void x_PS_printlnLong(NativeContext *ctx) { printf("%lld\n", static_cast<long long>(argLong(ctx, 1))); }
void x_PS_printLong(NativeContext *ctx) { printf("%lld", static_cast<long long>(argLong(ctx, 1))); }
void x_PS_printlnBool(NativeContext *ctx) { printf("%s\n", argInt(ctx, 1) ? "true" : "false"); }
void x_PS_printBool(NativeContext *ctx) { printf("%s", argInt(ctx, 1) ? "true" : "false"); }
void x_PS_printlnChars(NativeContext *ctx)
{
    Obj *a = argRef(ctx, 1);
    std::string s;
    if (a && a->cells) for (int i = 0; i < a->arrayLen; i++) s += static_cast<char>(a->cells[i].u & 0xFF);
    printf("%s\n", s.c_str());
}

// ---------------------------------------------------------------------------------------------
// Enregistrement des natives (les classes + signatures sont déclarées dans midp::init, regClass)
// ---------------------------------------------------------------------------------------------
void registerExtraNatives()
{
#define RN(k, f) registerNative(k, f)
    // String
    RN("java/lang/String.hashCode:()I", x_String_hashCode);
    RN("java/lang/String.getBytes:()[B", x_String_getBytes);
    RN("java/lang/String.getBytes:(Ljava/lang/String;)[B", x_String_getBytes);
    RN("java/lang/String.lastIndexOf:(I)I", x_String_lastIndexOfChar);
    RN("java/lang/String.lastIndexOf:(II)I", x_String_lastIndexOfCharFrom);
    RN("java/lang/String.lastIndexOf:(Ljava/lang/String;)I", x_String_lastIndexOfStr);
    RN("java/lang/String.replace:(CC)Ljava/lang/String;", x_String_replace);
    RN("java/lang/String.startsWith:(Ljava/lang/String;I)Z", x_String_startsWithFrom);
    RN("java/lang/String.toString:()Ljava/lang/String;", x_String_toString);
    RN("java/lang/String.isEmpty:()Z", x_String_isEmpty);
    RN("java/lang/String.compareToIgnoreCase:(Ljava/lang/String;)I", x_String_compareToIgnoreCase);
    RN("java/lang/String.<init>:(Ljava/lang/String;)V", x_String_initStr);
    RN("java/lang/String.<init>:(Ljava/lang/StringBuffer;)V", x_String_initStr);
    RN("java/lang/String.<init>:([C)V", x_String_initChars);
    RN("java/lang/String.valueOf:(C)Ljava/lang/String;", x_String_valueOfChar);
    RN("java/lang/String.valueOf:(J)Ljava/lang/String;", x_String_valueOfLong);
    RN("java/lang/String.valueOf:(Z)Ljava/lang/String;", x_String_valueOfBool);
    RN("java/lang/String.valueOf:(F)Ljava/lang/String;", x_String_valueOfFloat);
    RN("java/lang/String.valueOf:(D)Ljava/lang/String;", x_String_valueOfDouble);
    RN("java/lang/String.valueOf:([C)Ljava/lang/String;", x_String_valueOfChars);
    RN("java/lang/String.copyValueOf:([C)Ljava/lang/String;", x_String_valueOfChars);
    RN("java/lang/String.valueOf:(Ljava/lang/Object;)Ljava/lang/String;", x_String_valueOfObject);
    // StringBuffer
    RN("java/lang/StringBuffer.insert:(ILjava/lang/String;)Ljava/lang/StringBuffer;", x_SB_insertStr);
    RN("java/lang/StringBuffer.insert:(IC)Ljava/lang/StringBuffer;", x_SB_insertChar);
    RN("java/lang/StringBuffer.insert:(II)Ljava/lang/StringBuffer;", x_SB_insertInt);
    RN("java/lang/StringBuffer.reverse:()Ljava/lang/StringBuffer;", x_SB_reverse);
    RN("java/lang/StringBuffer.deleteCharAt:(I)Ljava/lang/StringBuffer;", x_SB_deleteCharAt);
    RN("java/lang/StringBuffer.capacity:()I", x_SB_capacity);
    RN("java/lang/StringBuffer.ensureCapacity:(I)V", x_SB_ensureCapacity);
    RN("java/lang/StringBuffer.append:([CII)Ljava/lang/StringBuffer;", x_SB_appendCharsRange);
    RN("java/lang/StringBuffer.indexOf:(Ljava/lang/String;)I", x_SB_indexOf);
    RN("java/lang/StringBuffer.substring:(II)Ljava/lang/String;", x_SB_substring);
    RN("java/lang/StringBuffer.substring:(I)Ljava/lang/String;", x_SB_substring);
    // Math (pow existait avec un bug d'index : le 2e double est en args[2])
    RN("java/lang/Math.pow:(DD)D", x_Math_pow);
    RN("java/lang/Math.sin:(D)D", x_Math_sin);
    RN("java/lang/Math.cos:(D)D", x_Math_cos);
    RN("java/lang/Math.tan:(D)D", x_Math_tan);
    RN("java/lang/Math.asin:(D)D", x_Math_asin);
    RN("java/lang/Math.acos:(D)D", x_Math_acos);
    RN("java/lang/Math.atan:(D)D", x_Math_atan);
    RN("java/lang/Math.atan2:(DD)D", x_Math_atan2);
    RN("java/lang/Math.exp:(D)D", x_Math_exp);
    RN("java/lang/Math.log:(D)D", x_Math_log);
    RN("java/lang/Math.toRadians:(D)D", x_Math_toRadians);
    RN("java/lang/Math.toDegrees:(D)D", x_Math_toDegrees);
    RN("java/lang/Math.abs:(D)D", x_Math_absD);
    RN("java/lang/Math.abs:(F)F", x_Math_absF);
    RN("java/lang/Math.min:(FF)F", x_Math_minF);
    RN("java/lang/Math.max:(FF)F", x_Math_maxF);
    RN("java/lang/Math.min:(DD)D", x_Math_minD);
    RN("java/lang/Math.max:(DD)D", x_Math_maxD);
    RN("java/lang/Math.round:(F)I", x_Math_roundF);
    RN("java/lang/Math.round:(D)J", x_Math_roundD);
    // Integer
    RN("java/lang/Integer.parseInt:(Ljava/lang/String;I)I", x_Int_parseIntRadix);
    RN("java/lang/Integer.toHexString:(I)Ljava/lang/String;", x_Int_toHex);
    RN("java/lang/Integer.toBinaryString:(I)Ljava/lang/String;", x_Int_toBin);
    RN("java/lang/Integer.toOctalString:(I)Ljava/lang/String;", x_Int_toOct);
    RN("java/lang/Integer.toString:(II)Ljava/lang/String;", x_Int_toStringRadix);
    RN("java/lang/Integer.valueOf:(Ljava/lang/String;)Ljava/lang/Integer;", x_Int_valueOfStr);
    // Long
    RN("java/lang/Long.<init>:(J)V", x_Long_init);
    RN("java/lang/Long.longValue:()J", x_Long_longValue);
    RN("java/lang/Long.intValue:()I", x_Long_intValue);
    RN("java/lang/Long.toString:()Ljava/lang/String;", x_Long_toString);
    RN("java/lang/Long.toString:(J)Ljava/lang/String;", x_Long_toStringS);
    RN("java/lang/Long.parseLong:(Ljava/lang/String;)J", x_Long_parse);
    RN("java/lang/Long.valueOf:(J)Ljava/lang/Long;", x_Long_valueOfL);
    RN("java/lang/Long.valueOf:(Ljava/lang/String;)Ljava/lang/Long;", x_Long_valueOfS);
    RN("java/lang/Long.hashCode:()I", x_Long_hashCode);
    RN("java/lang/Long.equals:(Ljava/lang/Object;)Z", x_Long_equals);
    // Short / Byte
    RN("java/lang/Short.<init>:(S)V", x_Small_init);
    RN("java/lang/Short.shortValue:()S", x_Small_value);
    RN("java/lang/Short.intValue:()I", x_Small_value);
    RN("java/lang/Short.toString:()Ljava/lang/String;", x_Small_toString);
    RN("java/lang/Short.parseShort:(Ljava/lang/String;)S", x_Short_parse);
    RN("java/lang/Byte.<init>:(B)V", x_Small_init);
    RN("java/lang/Byte.byteValue:()B", x_Small_value);
    RN("java/lang/Byte.intValue:()I", x_Small_value);
    RN("java/lang/Byte.toString:()Ljava/lang/String;", x_Small_toString);
    RN("java/lang/Byte.parseByte:(Ljava/lang/String;)B", x_Byte_parse);
    // Character
    RN("java/lang/Character.<init>:(C)V", x_Chr_init);
    RN("java/lang/Character.charValue:()C", x_Chr_charValue);
    RN("java/lang/Character.toString:()Ljava/lang/String;", x_Chr_toString);
    RN("java/lang/Character.toString:(C)Ljava/lang/String;", x_Chr_toStringS);
    RN("java/lang/Character.isDigit:(C)Z", x_Chr_isDigit);
    RN("java/lang/Character.isLetter:(C)Z", x_Chr_isLetter);
    RN("java/lang/Character.isLetterOrDigit:(C)Z", x_Chr_isLetterOrDigit);
    RN("java/lang/Character.isUpperCase:(C)Z", x_Chr_isUpper);
    RN("java/lang/Character.isLowerCase:(C)Z", x_Chr_isLower);
    RN("java/lang/Character.isSpace:(C)Z", x_Chr_isSpace);
    RN("java/lang/Character.isWhitespace:(C)Z", x_Chr_isSpace);
    RN("java/lang/Character.toUpperCase:(C)C", x_Chr_toUpper);
    RN("java/lang/Character.toLowerCase:(C)C", x_Chr_toLower);
    RN("java/lang/Character.digit:(CI)I", x_Chr_digit);
    RN("java/lang/Character.forDigit:(II)C", x_Chr_forDigit);
    RN("java/lang/Character.valueOf:(C)Ljava/lang/Character;", x_Chr_valueOf);
    RN("java/lang/Character.hashCode:()I", x_Chr_hashCode);
    RN("java/lang/Character.equals:(Ljava/lang/Object;)Z", x_Chr_equals);
    // Float / Double
    RN("java/lang/Float.<init>:(F)V", x_Float_init);
    RN("java/lang/Float.floatValue:()F", x_Float_floatValue);
    RN("java/lang/Float.doubleValue:()D", x_Float_doubleValue);
    RN("java/lang/Float.intValue:()I", x_Float_intValue);
    RN("java/lang/Float.toString:()Ljava/lang/String;", x_Float_toString);
    RN("java/lang/Float.toString:(F)Ljava/lang/String;", x_Float_toStringS);
    RN("java/lang/Float.parseFloat:(Ljava/lang/String;)F", x_Float_parse);
    RN("java/lang/Float.valueOf:(Ljava/lang/String;)Ljava/lang/Float;", x_Float_valueOfS);
    RN("java/lang/Float.valueOf:(F)Ljava/lang/Float;", x_Float_valueOfF);
    RN("java/lang/Float.isNaN:(F)Z", x_Float_isNaN);
    RN("java/lang/Float.isNaN:()Z", x_Float_isNaNi);
    RN("java/lang/Float.isInfinite:(F)Z", x_Float_isInf);
    RN("java/lang/Float.floatToIntBits:(F)I", x_Float_toBits);
    RN("java/lang/Float.intBitsToFloat:(I)F", x_Float_fromBits);
    RN("java/lang/Double.<init>:(D)V", x_Double_init);
    RN("java/lang/Double.doubleValue:()D", x_Double_doubleValue);
    RN("java/lang/Double.floatValue:()F", x_Double_floatValue);
    RN("java/lang/Double.intValue:()I", x_Double_intValue);
    RN("java/lang/Double.toString:()Ljava/lang/String;", x_Double_toString);
    RN("java/lang/Double.toString:(D)Ljava/lang/String;", x_Double_toStringS);
    RN("java/lang/Double.parseDouble:(Ljava/lang/String;)D", x_Double_parse);
    RN("java/lang/Double.valueOf:(Ljava/lang/String;)Ljava/lang/Double;", x_Double_valueOfS);
    RN("java/lang/Double.valueOf:(D)Ljava/lang/Double;", x_Double_valueOfD);
    RN("java/lang/Double.isNaN:(D)Z", x_Double_isNaN);
    RN("java/lang/Double.isNaN:()Z", x_Double_isNaNi);
    RN("java/lang/Double.isInfinite:(D)Z", x_Double_isInf);
    RN("java/lang/Double.doubleToLongBits:(D)J", x_Double_toBits);
    RN("java/lang/Double.longBitsToDouble:(J)D", x_Double_fromBits);
    RN("javax/microedition/io/Connector.open:(Ljava/lang/String;)Ljavax/microedition/io/Connection;", x_Connector_open);
    RN("javax/microedition/io/Connector.open:(Ljava/lang/String;I)Ljavax/microedition/io/Connection;", x_Connector_open);
    RN("javax/microedition/io/Connector.open:(Ljava/lang/String;IZ)Ljavax/microedition/io/Connection;", x_Connector_open);
    RN("javax/microedition/io/Connector.openInputStream:(Ljava/lang/String;)Ljava/io/InputStream;", x_Connector_open);
    RN("javax/microedition/io/Connector.openDataInputStream:(Ljava/lang/String;)Ljava/io/DataInputStream;", x_Connector_open);
    // PrintStream
    RN("java/io/PrintStream.println:(Ljava/lang/Object;)V", x_PS_printlnObj);
    RN("java/io/PrintStream.print:(Ljava/lang/Object;)V", x_PS_printObj);
    RN("java/io/PrintStream.println:(C)V", x_PS_printlnChar);
    RN("java/io/PrintStream.print:(C)V", x_PS_printChar);
    RN("java/io/PrintStream.println:(J)V", x_PS_printlnLong);
    RN("java/io/PrintStream.print:(J)V", x_PS_printLong);
    RN("java/io/PrintStream.println:(Z)V", x_PS_printlnBool);
    RN("java/io/PrintStream.print:(Z)V", x_PS_printBool);
    RN("java/io/PrintStream.println:([C)V", x_PS_printlnChars);
    // java.util
    RN("java/util/ArrayEnumeration.hasMoreElements:()Z", x_Enum_hasMore);
    RN("java/util/ArrayEnumeration.nextElement:()Ljava/lang/Object;", x_Enum_next);
    RN("java/util/Vector.elements:()Ljava/util/Enumeration;", x_Vec_elements);
    RN("java/util/Vector.trimToSize:()V", x_SB_ensureCapacity);
    RN("java/util/Vector.ensureCapacity:(I)V", x_SB_ensureCapacity);
    RN("java/util/Vector.copyInto:([Ljava/lang/Object;)V", x_Vec_copyInto);
    RN("java/util/Vector.setSize:(I)V", x_Vec_setSize);
    RN("java/util/Vector.capacity:()I", x_Vec_capacity);
    RN("java/util/Vector.indexOf:(Ljava/lang/Object;I)I", x_Vec_indexOfFrom);
    RN("java/util/Vector.lastIndexOf:(Ljava/lang/Object;)I", x_Vec_lastIndexOf);
    RN("java/util/Vector.toString:()Ljava/lang/String;", x_Vec_toString);
    RN("java/util/Stack.<init>:()V", n_Vec_init0);
    RN("java/util/Stack.push:(Ljava/lang/Object;)Ljava/lang/Object;", x_Stack_push);
    RN("java/util/Stack.pop:()Ljava/lang/Object;", x_Stack_pop);
    RN("java/util/Stack.peek:()Ljava/lang/Object;", x_Stack_peek);
    RN("java/util/Stack.empty:()Z", x_Stack_empty);
    RN("java/util/Stack.search:(Ljava/lang/Object;)I", x_Stack_search);
    RN("java/util/Hashtable.keys:()Ljava/util/Enumeration;", x_HT_keys);
    RN("java/util/Hashtable.elements:()Ljava/util/Enumeration;", x_HT_elements);
    RN("java/util/Hashtable.contains:(Ljava/lang/Object;)Z", x_HT_contains);
    RN("java/util/Hashtable.<init>:(I)V", x_HT_initCap);
    RN("java/util/Date.<init>:()V", x_Date_init0);
    RN("java/util/Date.<init>:(J)V", x_Date_initL);
    RN("java/util/Date.getTime:()J", x_Date_getTime);
    RN("java/util/Date.setTime:(J)V", x_Date_setTime);
    RN("java/util/Date.toString:()Ljava/lang/String;", x_Date_toString);
    RN("java/util/Calendar.getInstance:()Ljava/util/Calendar;", x_Cal_getInstance);
    RN("java/util/Calendar.get:(I)I", x_Cal_get);
    RN("java/util/Calendar.getTime:()Ljava/util/Date;", x_Cal_getTime);
    RN("java/util/Calendar.setTime:(Ljava/util/Date;)V", x_Cal_setTime);
    RN("java/util/Calendar.getTimeInMillis:()J", x_Date_getTime);
    RN("java/util/Calendar.setTimeInMillis:(J)V", x_Date_setTime);
    RN("java/util/TimeZone.getDefault:()Ljava/util/TimeZone;", x_TZ_getDefault);
    RN("java/util/TimeZone.getID:()Ljava/lang/String;", x_TZ_getID);
#undef RN
}
