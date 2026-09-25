// test_interpreter.cpp
// Tests du coeur de l'interpreteur bytecode (vm/interpreter.cpp), construits
// en assemblant du bytecode a la main (pas besoin d'un vrai .jar : un
// ClassInfo/ClassFile/MethodInfo minimal suffit -- cf. vm/runtime.h/
// class_file.h, tous des structs a champs publics).
//
// Priorite donnee aux DEUX regressions documentees dans CLAUDE.md ("JVM
// correctness pitfalls") comme deja corrigees une fois et a ne pas
// reintroduire :
//   - tableswitch/lookupswitch : offsets relatifs a l'opcode lui-meme, pas a
//     la base post-padding ; lookupswitch a un en-tete de 8 octets (pas 12,
//     copie par erreur depuis tableswitch).
//   - athrow / try-catch : deroulement de pile reel via la table
//     d'exceptions (findExceptionHandler), pas un abandon systematique.

#include "framework.h"
#include "../vm/interpreter.h"
#include "../vm/runtime.h"
#include "../vm/class_file.h"

#include <memory>
#include <vector>

using namespace jvm;

namespace
{
    void appendU32BE(std::vector<uint8_t> &v, int32_t x)
    {
        v.push_back(static_cast<uint8_t>((x >> 24) & 0xff));
        v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
        v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
        v.push_back(static_cast<uint8_t>(x & 0xff));
    }
    void patchU32BE(std::vector<uint8_t> &v, size_t pos, int32_t x)
    {
        v[pos] = static_cast<uint8_t>((x >> 24) & 0xff);
        v[pos + 1] = static_cast<uint8_t>((x >> 16) & 0xff);
        v[pos + 2] = static_cast<uint8_t>((x >> 8) & 0xff);
        v[pos + 3] = static_cast<uint8_t>(x & 0xff);
    }
    // sipush <val>; ireturn -- 4 octets, renvoie l'entier <val>.
    std::vector<uint8_t> retIntBlock(int16_t val)
    {
        return {0x11, static_cast<uint8_t>((val >> 8) & 0xff), static_cast<uint8_t>(val & 0xff), 0xac};
    }
    // iconst_m1; ireturn -- 2 octets, renvoie -1 (case "default").
    std::vector<uint8_t> retMinus1Block() { return {0x02, 0xac}; }

    // Construit un ClassInfo/MethodRecord minimal pour un bytecode donne et
    // invoque-le. `cf`/`ci`/`mr` doivent survivre a l'appelant (passes par
    // reference pour etre remplis en place, pas retournes par valeur, afin
    // que MethodRecord::mi -- qui pointe dans cf.methods -- reste valide).
    void wireMethod(ClassFile &cf, ClassInfo &ci, MethodRecord &mr,
                     std::vector<uint8_t> code, uint16_t maxStack, uint16_t maxLocals,
                     std::vector<CodeAttribute::ExceptionHandler> handlers = {})
    {
        MethodInfo mi;
        mi.codeAttr = std::make_unique<CodeAttribute>();
        mi.codeAttr->code = std::move(code);
        mi.codeAttr->maxStack = maxStack;
        mi.codeAttr->maxLocals = maxLocals;
        mi.codeAttr->handlers = std::move(handlers);
        cf.methods.push_back(std::move(mi));

        ci.name = "Test";
        ci.cf = &cf;
        mr.name = "m";
        mr.desc = "test";
        mr.mi = &cf.methods.back();
        mr.owner = &ci;
        mr.isStatic = true;
    }
} // namespace

// ---------------------------------------------------------------------
// Sanite de base : arithmetique + retour depuis une frame.
// ---------------------------------------------------------------------

TEST(interpreter_iadd_basic_arithmetic)
{
    // static int add(int a, int b) { return a + b; }
    std::vector<uint8_t> code = {0x1a, 0x1b, 0x60, 0xac}; // iload_0, iload_1, iadd, ireturn

    ClassFile cf;
    ClassInfo ci;
    MethodRecord mr;
    wireMethod(cf, ci, mr, code, /*maxStack*/ 2, /*maxLocals*/ 2);

    Runtime rt(64 * 1024);
    Interpreter interp(&rt);

    Value args[2] = {Value::fromInt(3), Value::fromInt(4)};
    Value result;
    ASSERT_TRUE(interp.invoke(&ci, &mr, nullptr, args, 2, result));
    ASSERT_EQ(result.i, 7);
}

// ---------------------------------------------------------------------
// tableswitch : offsets relatifs a l'opcode, cas hors [lo,hi] -> default.
// ---------------------------------------------------------------------

TEST(interpreter_tableswitch_dispatches_matching_case)
{
    // static int m(int key) { switch(key) { case 0: return 100; case 1: return 200;
    //                                        case 2: return 300; default: return -1; } }
    std::vector<uint8_t> code;
    code.push_back(0x1a); // iload_0 : pousse la cle
    size_t opcodeIdx = code.size();
    code.push_back(0xaa); // tableswitch
    while (code.size() % 4 != 0)
        code.push_back(0); // padding jusqu'au prochain multiple de 4

    size_t defPos = code.size();
    appendU32BE(code, 0); // default (patche plus bas)
    appendU32BE(code, 0); // low = 0
    appendU32BE(code, 2); // high = 2
    size_t off0Pos = code.size();
    appendU32BE(code, 0);
    size_t off1Pos = code.size();
    appendU32BE(code, 0);
    size_t off2Pos = code.size();
    appendU32BE(code, 0);

    size_t case0 = code.size();
    for (uint8_t b : retIntBlock(100))
        code.push_back(b);
    size_t case1 = code.size();
    for (uint8_t b : retIntBlock(200))
        code.push_back(b);
    size_t case2 = code.size();
    for (uint8_t b : retIntBlock(300))
        code.push_back(b);
    size_t caseDef = code.size();
    for (uint8_t b : retMinus1Block())
        code.push_back(b);

    int32_t opcodePc = static_cast<int32_t>(opcodeIdx);
    patchU32BE(code, off0Pos, static_cast<int32_t>(case0) - opcodePc);
    patchU32BE(code, off1Pos, static_cast<int32_t>(case1) - opcodePc);
    patchU32BE(code, off2Pos, static_cast<int32_t>(case2) - opcodePc);
    patchU32BE(code, defPos, static_cast<int32_t>(caseDef) - opcodePc);

    ClassFile cf;
    ClassInfo ci;
    MethodRecord mr;
    wireMethod(cf, ci, mr, code, /*maxStack*/ 1, /*maxLocals*/ 1);

    Runtime rt(64 * 1024);
    Interpreter interp(&rt);

    auto run = [&](int32_t key) {
        Value args[1] = {Value::fromInt(key)};
        Value result;
        bool ok = interp.invoke(&ci, &mr, nullptr, args, 1, result);
        ASSERT_TRUE(ok);
        return result.i;
    };

    ASSERT_EQ(run(0), 100);
    ASSERT_EQ(run(1), 200);
    ASSERT_EQ(run(2), 300);
    ASSERT_EQ(run(5), -1);  // hors [lo,hi] -> default
    ASSERT_EQ(run(-3), -1); // hors [lo,hi] (negatif) -> default
}

// ---------------------------------------------------------------------
// lookupswitch : paires (match,offset) non contigues, correctement
// alignees. Regression exacte du bug documente (mortal_combat_new_b...jar) :
// un decalage de 4 octets dans l'en-tete faisait lire l'offset comme le
// match et le match suivant comme l'offset, si bien qu'aucune paire ne
// correspondait jamais et que seul le "default" partait.
// ---------------------------------------------------------------------

TEST(interpreter_lookupswitch_dispatches_noncontiguous_keys)
{
    // static int m(int key) { switch(key) { case 10: return 111; case 20: return 222;
    //                                        case 30: return 333; default: return -1; } }
    std::vector<uint8_t> code;
    code.push_back(0x1a); // iload_0
    size_t opcodeIdx = code.size();
    code.push_back(0xab); // lookupswitch
    while (code.size() % 4 != 0)
        code.push_back(0);

    size_t defPos = code.size();
    appendU32BE(code, 0); // default (patche)
    appendU32BE(code, 3); // npairs = 3

    size_t pair0MatchPos = code.size();
    appendU32BE(code, 10);
    size_t pair0OffPos = code.size();
    appendU32BE(code, 0);
    size_t pair1MatchPos = code.size();
    appendU32BE(code, 20);
    size_t pair1OffPos = code.size();
    appendU32BE(code, 0);
    size_t pair2MatchPos = code.size();
    appendU32BE(code, 30);
    size_t pair2OffPos = code.size();
    appendU32BE(code, 0);
    (void)pair0MatchPos; (void)pair1MatchPos; (void)pair2MatchPos;

    size_t caseA = code.size();
    for (uint8_t b : retIntBlock(111))
        code.push_back(b);
    size_t caseB = code.size();
    for (uint8_t b : retIntBlock(222))
        code.push_back(b);
    size_t caseC = code.size();
    for (uint8_t b : retIntBlock(333))
        code.push_back(b);
    size_t caseDef = code.size();
    for (uint8_t b : retMinus1Block())
        code.push_back(b);

    int32_t opcodePc = static_cast<int32_t>(opcodeIdx);
    patchU32BE(code, pair0OffPos, static_cast<int32_t>(caseA) - opcodePc);
    patchU32BE(code, pair1OffPos, static_cast<int32_t>(caseB) - opcodePc);
    patchU32BE(code, pair2OffPos, static_cast<int32_t>(caseC) - opcodePc);
    patchU32BE(code, defPos, static_cast<int32_t>(caseDef) - opcodePc);

    ClassFile cf;
    ClassInfo ci;
    MethodRecord mr;
    wireMethod(cf, ci, mr, code, /*maxStack*/ 1, /*maxLocals*/ 1);

    Runtime rt(64 * 1024);
    Interpreter interp(&rt);

    auto run = [&](int32_t key) {
        Value args[1] = {Value::fromInt(key)};
        Value result;
        bool ok = interp.invoke(&ci, &mr, nullptr, args, 1, result);
        ASSERT_TRUE(ok);
        return result.i;
    };

    ASSERT_EQ(run(10), 111);
    ASSERT_EQ(run(20), 222); // la paire "du milieu" est le cas le plus sensible au bug de decalage
    ASSERT_EQ(run(30), 333);
    ASSERT_EQ(run(15), -1); // ne correspond a aucune paire -> default
}

// ---------------------------------------------------------------------
// athrow / try-catch : deroulement de pile reel via la table d'exceptions.
// ---------------------------------------------------------------------

namespace
{
    // Pool minimal : #1 Utf8(name) #2 Class(#1). Utilise comme catchType.
    void setSingleClassConstantPool(ClassFile &cf, const char *name)
    {
        cf.constantPool.entries.resize(3);
        cf.constantPool.entries[1].tag = CONSTANT_UTF8;
        cf.constantPool.entries[1].utf8 = name;
        cf.constantPool.entries[2].tag = CONSTANT_CLASS;
        cf.constantPool.entries[2].nameIndex = 1;
    }
} // namespace

TEST(interpreter_athrow_caught_by_matching_handler)
{
    // static int m(Object exc) { try { throw exc; } catch (Exception e) { return 42; } }
    std::vector<uint8_t> code = {
        0x2a,       // [0] aload_0 : pousse l'exception
        0xbf,       // [1] athrow
        0x57,       // [2] handler: pop (jette la ref, deja recuperee par le mecanisme de catch)
        0x10, 0x2a, // [3,4] bipush 42
        0xac,       // [5] ireturn
    };
    CodeAttribute::ExceptionHandler h{0, 2, 2, 2}; // startPc,endPc,handlerPc,catchType=#2

    ClassFile cf;
    setSingleClassConstantPool(cf, "Exception");
    ClassInfo ci;
    MethodRecord mr;
    wireMethod(cf, ci, mr, code, /*maxStack*/ 1, /*maxLocals*/ 1, {h});

    Runtime rt(64 * 1024);
    ClassInfo exBase;
    exBase.name = "Exception";
    ClassInfo exDerived;
    exDerived.name = "MyException";
    exDerived.super = &exBase; // le catch doit matcher via la chaine de super, pas juste le type exact
    Obj *exObj = rt.heap().newInstance(&exDerived);
    ASSERT_TRUE(exObj != nullptr);

    Interpreter interp(&rt);
    Value args[1] = {Value::fromRef(exObj)};
    Value result;
    ASSERT_TRUE(interp.invoke(&ci, &mr, nullptr, args, 1, result));
    ASSERT_EQ(result.i, 42);
}

TEST(interpreter_athrow_catch_all_matches_any_type)
{
    std::vector<uint8_t> code = {0x2a, 0xbf, 0x57, 0x10, 0x2a, 0xac};
    CodeAttribute::ExceptionHandler h{0, 2, 2, 0}; // catchType = 0 -> catch-all

    ClassFile cf; // pas besoin de pool : catchType=0 ne consulte pas cp.getClassName
    ClassInfo ci;
    MethodRecord mr;
    wireMethod(cf, ci, mr, code, 1, 1, {h});

    Runtime rt(64 * 1024);
    ClassInfo anyCls;
    anyCls.name = "WhateverType";
    Obj *exObj = rt.heap().newInstance(&anyCls);

    Interpreter interp(&rt);
    Value args[1] = {Value::fromRef(exObj)};
    Value result;
    ASSERT_TRUE(interp.invoke(&ci, &mr, nullptr, args, 1, result));
    ASSERT_EQ(result.i, 42);
}

TEST(interpreter_athrow_unmatched_type_propagates_as_failure)
{
    std::vector<uint8_t> code = {0x2a, 0xbf, 0x57, 0x10, 0x2a, 0xac};
    CodeAttribute::ExceptionHandler h{0, 2, 2, 2}; // catchType -> "Exception"

    ClassFile cf;
    setSingleClassConstantPool(cf, "Exception");
    ClassInfo ci;
    MethodRecord mr;
    wireMethod(cf, ci, mr, code, 1, 1, {h});

    Runtime rt(64 * 1024);
    ClassInfo unrelated;
    unrelated.name = "TotallyUnrelatedType"; // ni "Exception" ni un de ses sous-types
    Obj *exObj = rt.heap().newInstance(&unrelated);

    Interpreter interp(&rt);
    Value args[1] = {Value::fromRef(exObj)};
    Value result;
    // Sans handler correspondant, l'exception continue de se propager :
    // execBytecode echoue proprement (comportement equivalent a un
    // athrow non rattrape remontant jusqu'a l'appelant C++ du test).
    ASSERT_FALSE(interp.invoke(&ci, &mr, nullptr, args, 1, result));
}

TEST(interpreter_athrow_outside_try_range_not_caught)
{
    // Le handler ne couvre que [0,2) ; l'athrow est en pc=2 (hors plage) ->
    // ne doit pas etre rattrape meme si le type correspond.
    std::vector<uint8_t> code = {
        0x00,       // [0] nop (en dehors, juste pour decaler)
        0x00,       // [1] nop
        0x2a,       // [2] aload_0
        0xbf,       // [3] athrow (pc=3, hors [0,2))
        0x57,       // [4] handler (jamais atteint)
        0x10, 0x2a, // [5,6] bipush 42
        0xac,       // [7] ireturn
    };
    CodeAttribute::ExceptionHandler h{0, 2, 4, 2};

    ClassFile cf;
    setSingleClassConstantPool(cf, "Exception");
    ClassInfo ci;
    MethodRecord mr;
    wireMethod(cf, ci, mr, code, 1, 1, {h});

    Runtime rt(64 * 1024);
    ClassInfo exCls;
    exCls.name = "Exception";
    Obj *exObj = rt.heap().newInstance(&exCls);

    Interpreter interp(&rt);
    Value args[1] = {Value::fromRef(exObj)};
    Value result;
    ASSERT_FALSE(interp.invoke(&ci, &mr, nullptr, args, 1, result));
}

// ---------------------------------------------------------------------
// Pile : dup2 sur un long (motif `f = now(); bg = (int)(f - last)` des
// boucles de jeu : invokestatic currentTimeMillis; dup2; lstore f; lload
// last; lsub; l2i).
// ---------------------------------------------------------------------

TEST(interpreter_dup2_on_long_then_lsub_l2i)
{
    // static int m(long now, long last) { long f = now; return (int)(f - last); }
    // lload_0 ; dup2 ; lstore_2 ; lload 4 ; lsub ; l2i ; ireturn  (le dup2 laisse `now` sur la pile)
    std::vector<uint8_t> code = {0x1e, 0x5c, 0x41, 0x16, 0x04, 0x65, 0x88, 0xac};
    ClassFile cf;
    ClassInfo ci;
    MethodRecord mr;
    wireMethod(cf, ci, mr, code, /*maxStack*/ 6, /*maxLocals*/ 6);
    Runtime rt(64 * 1024);
    Interpreter interp(&rt);
    Value args[6];
    args[0] = Value::fromLong(1000);
    args[4] = Value::fromLong(940);
    Value result;
    // Pile après lload_0/dup2/lstore_2 : [now] ; puis lload 4 ; lsub -> now - last
    ASSERT_TRUE(interp.invoke(&ci, &mr, nullptr, args, 6, result));
    ASSERT_EQ(result.i, 60);
}
