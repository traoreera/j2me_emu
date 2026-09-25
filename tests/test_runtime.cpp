// test_runtime.cpp
// Tests de vm/runtime.* : l'allocateur bump (Heap) et la resolution de
// champs/methodes de ClassInfo -- en particulier la desambiguisation de
// champs par (nom, descripteur), documentee dans CLAUDE.md comme piege deja
// corrige une fois (obfuscation qui reutilise un meme nom de champ pour
// plusieurs types sur une meme classe).

#include "framework.h"
#include "../vm/runtime.h"

using namespace jvm;

// ---------------------------------------------------------------------
// Heap
// ---------------------------------------------------------------------

TEST(heap_alloc_obj_zeroes_cells)
{
    Heap heap(4096);
    Obj *o = heap.allocObj(ObjKind::Instance, 3);
    ASSERT_TRUE(o != nullptr);
    ASSERT_EQ(o->cellCount, 3);
    for (int i = 0; i < 3; i++)
        ASSERT_EQ(o->cells[i].i, 0);
}

TEST(heap_new_string_roundtrip)
{
    Heap heap(4096);
    Obj *s = heap.newString("hello");
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ((int)s->kind, (int)ObjKind::String);
    ASSERT_EQ(s->str, std::string("hello"));
}

TEST(heap_new_string_cat_concatenates)
{
    Heap heap(4096);
    Obj *a = heap.newString("foo");
    Obj *b = heap.newString("bar");
    Obj *c = heap.newStringCat(a, b);
    ASSERT_EQ(c->str, std::string("foobar"));
}

TEST(heap_new_array_sets_length_and_cells)
{
    Heap heap(4096);
    Obj *arr = heap.newArray(ObjKind::IntArray, 5);
    ASSERT_TRUE(arr != nullptr);
    ASSERT_EQ(arr->arrayLen, 5);
    ASSERT_EQ(arr->cellCount, 5);
}

TEST(heap_new_array_negative_length_fails)
{
    Heap heap(4096);
    Obj *arr = heap.newArray(ObjKind::IntArray, -1);
    ASSERT_TRUE(arr == nullptr);
}

TEST(heap_new_instance_uses_class_instance_cells)
{
    Heap heap(4096);
    ClassInfo ci;
    ci.name = "Foo";
    ci.instanceCells = 4;
    Obj *o = heap.newInstance(&ci);
    ASSERT_TRUE(o != nullptr);
    ASSERT_EQ(o->cls, &ci);
    ASSERT_EQ(o->cellCount, 4);
}

TEST(heap_reset_reclaims_used_space)
{
    Heap heap(4096);
    heap.newString("some content to consume bump space");
    ASSERT_TRUE(heap.used() > 0);
    heap.reset();
    ASSERT_EQ(heap.used(), (size_t)0);
    ASSERT_EQ(heap.capacity(), (size_t)4096);
    // Le heap doit rester utilisable apres reset().
    Obj *s = heap.newString("again");
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(s->str, std::string("again"));
}

TEST(heap_auto_grows_beyond_initial_segment)
{
    Heap heap(64); // pool minuscule -> force un auto-grow rapide
    bool grew = false;
    for (int i = 0; i < 50 && !grew; i++)
    {
        Obj *o = heap.allocObj(ObjKind::Instance, 8);
        ASSERT_TRUE(o != nullptr); // ne doit jamais echouer : auto-grow, pas d'OOM ici
        if (heap.capacity() > 64)
            grew = true;
    }
    ASSERT_TRUE(grew);
}

TEST(heap_max_caps_growth_and_reports_oom)
{
    Heap heap(256, 1024);
    ASSERT_EQ((size_t)1024, heap.maximumCapacity());
    int ok = 0;
    Obj *o = nullptr;
    for (int i = 0; i < 1000; i++)
    {
        o = heap.allocObj(ObjKind::Instance, 4);
        if (!o) break;
        ok++;
    }
    ASSERT_TRUE(o == nullptr);
    ASSERT_TRUE(heap.outOfMemory());
    ASSERT_TRUE(ok > 0);
    ASSERT_TRUE(heap.capacity() <= 1024);
    heap.reset();
    ASSERT_EQ((size_t)256, heap.capacity());
    ASSERT_FALSE(heap.outOfMemory());
    ASSERT_TRUE(heap.allocObj(ObjKind::Instance, 4) != nullptr);
}

TEST(heap_max_below_initial_is_raised_to_initial)
{
    Heap heap(512, 100);
    ASSERT_EQ((size_t)512, heap.maximumCapacity());
}

// ---------------------------------------------------------------------
// ClassInfo::findField -- desambiguisation par descripteur
// ---------------------------------------------------------------------

TEST(find_field_disambiguates_same_name_by_descriptor)
{
    ClassInfo ci;
    ci.name = "Obfuscated";

    MethodRecord fInt;
    fInt.name = "a";
    fInt.desc = "I";
    fInt.isField = true;
    fInt.slot = 0;
    ci.fields.push_back(fInt);

    MethodRecord fStr;
    fStr.name = "a";
    fStr.desc = "Ljava/lang/String;";
    fStr.isField = true;
    fStr.slot = 1;
    ci.fields.push_back(fStr);

    const MethodRecord *gotInt = ci.findField("a", "I");
    const MethodRecord *gotStr = ci.findField("a", "Ljava/lang/String;");
    ASSERT_TRUE(gotInt != nullptr);
    ASSERT_TRUE(gotStr != nullptr);
    ASSERT_NE(gotInt, gotStr);
    ASSERT_EQ(gotInt->slot, 0);
    ASSERT_EQ(gotStr->slot, 1);
}

TEST(find_field_empty_descriptor_returns_first_name_match)
{
    ClassInfo ci;
    ci.name = "Obfuscated";
    MethodRecord fInt;
    fInt.name = "a";
    fInt.desc = "I";
    fInt.slot = 0;
    ci.fields.push_back(fInt);
    MethodRecord fStr;
    fStr.name = "a";
    fStr.desc = "Ljava/lang/String;";
    fStr.slot = 1;
    ci.fields.push_back(fStr);

    const MethodRecord *got = ci.findField("a"); // pas de descripteur -> premier match
    ASSERT_TRUE(got != nullptr);
    ASSERT_EQ(got->slot, 0);
}

TEST(find_field_missing_returns_null)
{
    ClassInfo ci;
    ci.name = "Empty";
    ASSERT_TRUE(ci.findField("nope", "I") == nullptr);
}

// ---------------------------------------------------------------------
// ClassInfo::findMethodVirtual -- remontee de la chaine de super
// ---------------------------------------------------------------------

TEST(find_method_virtual_walks_superclass_chain)
{
    ClassInfo base;
    base.name = "Base";
    MethodRecord foo;
    foo.name = "foo";
    foo.desc = "()V";
    foo.owner = &base;
    base.methods.push_back(foo);

    ClassInfo derived;
    derived.name = "Derived";
    derived.super = &base;

    const MethodRecord *found = derived.findMethodVirtual("foo", "()V");
    ASSERT_TRUE(found != nullptr);
    ASSERT_EQ(found->owner, &base);
}

TEST(find_method_virtual_prefers_own_override_over_super)
{
    ClassInfo base;
    base.name = "Base";
    MethodRecord baseFoo;
    baseFoo.name = "foo";
    baseFoo.desc = "()V";
    baseFoo.owner = &base;
    base.methods.push_back(baseFoo);

    ClassInfo derived;
    derived.name = "Derived";
    derived.super = &base;
    MethodRecord derivedFoo;
    derivedFoo.name = "foo";
    derivedFoo.desc = "()V";
    derivedFoo.owner = &derived;
    derived.methods.push_back(derivedFoo);

    const MethodRecord *found = derived.findMethodVirtual("foo", "()V");
    ASSERT_TRUE(found != nullptr);
    ASSERT_EQ(found->owner, &derived);
}

TEST(find_method_virtual_missing_returns_null)
{
    ClassInfo base;
    base.name = "Base";
    ClassInfo derived;
    derived.name = "Derived";
    derived.super = &base;

    ASSERT_TRUE(derived.findMethodVirtual("nope", "()V") == nullptr);
}
