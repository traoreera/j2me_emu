// test_class_file.cpp
// Tests du parseur .class (vm/class_file.*) : parsing de descripteurs de
// type (pur, sans I/O), accès au pool de constantes construit directement en
// mémoire (sans passer par le binaire .class), et un round-trip complet de
// ClassFile::parse() sur un .class minimal construit à la main (une méthode
// statique triviale avec un attribut Code).

#include "framework.h"
#include "core/class_file.h"

#include <cstring>
#include <vector>

using namespace jvm;

// ---------------------------------------------------------------------
// parseFieldDesc / parseMethodDesc (fonctions pures)
// ---------------------------------------------------------------------

TEST(parse_field_desc_primitive)
{
    const char *s = "I";
    const char *p = s;
    FieldDesc fd = parseFieldDesc(p);
    ASSERT_EQ(fd.baseType, 'I');
    ASSERT_EQ(fd.arrayDim, 0);
}

TEST(parse_field_desc_object)
{
    const char *s = "Ljava/lang/String;";
    const char *p = s;
    FieldDesc fd = parseFieldDesc(p);
    ASSERT_EQ(fd.baseType, 'L');
    ASSERT_EQ(fd.className, std::string("java/lang/String"));
    ASSERT_EQ(fd.arrayDim, 0);
    ASSERT_EQ(*p, '\0'); // le pointeur doit avoir consomme tout le descripteur
}

TEST(parse_field_desc_array_of_object)
{
    const char *s = "[[Ljavax/microedition/lcdui/game/Layer;";
    const char *p = s;
    FieldDesc fd = parseFieldDesc(p);
    ASSERT_EQ(fd.baseType, 'L');
    ASSERT_EQ(fd.arrayDim, 2);
    ASSERT_EQ(fd.className, std::string("javax/microedition/lcdui/game/Layer"));
}

TEST(parse_method_desc_multiple_args)
{
    MethodDesc md = parseMethodDesc("(ILjava/lang/String;[BZ)J");
    ASSERT_EQ(md.params.size(), (size_t)4);
    ASSERT_EQ(md.params[0].baseType, 'I');
    ASSERT_EQ(md.params[1].baseType, 'L');
    ASSERT_EQ(md.params[1].className, std::string("java/lang/String"));
    ASSERT_EQ(md.params[2].baseType, 'B');
    ASSERT_EQ(md.params[2].arrayDim, 1);
    ASSERT_EQ(md.params[3].baseType, 'Z');
    ASSERT_EQ(md.returnType.baseType, 'J');
}

TEST(parse_method_desc_void_no_args)
{
    MethodDesc md = parseMethodDesc("()V");
    ASSERT_EQ(md.params.size(), (size_t)0);
    ASSERT_EQ(md.returnType.baseType, 'V');
}

// ---------------------------------------------------------------------
// ConstantPool : construit directement en mémoire (pas besoin de bytes
// .class pour tester la logique d'accès elle-meme).
// ---------------------------------------------------------------------

TEST(constant_pool_get_utf8_and_class_name)
{
    ConstantPool cp;
    cp.entries.resize(3);
    cp.entries[1].tag = CONSTANT_UTF8;
    cp.entries[1].utf8 = "com/example/Foo";
    cp.entries[2].tag = CONSTANT_CLASS;
    cp.entries[2].nameIndex = 1;

    ASSERT_EQ(cp.getUtf8(1), std::string("com/example/Foo"));
    ASSERT_EQ(cp.getClassName(2), std::string("com/example/Foo"));
}

TEST(constant_pool_get_utf8_wrong_tag_returns_empty)
{
    ConstantPool cp;
    cp.entries.resize(2);
    cp.entries[1].tag = CONSTANT_INTEGER;
    cp.entries[1].intVal = 42;

    ASSERT_EQ(cp.getUtf8(1), std::string(""));
}

TEST(constant_pool_get_method_ref_resolves_class_name_desc)
{
    // pool: 1=Utf8"Foo" 2=Class(1) 3=Utf8"bar" 4=Utf8"(I)V"
    //       5=NameAndType(classIndex=3 [nom], nameAndTypeIndex=4 [desc])
    //       6=Methodref(classIndex=2, nameAndTypeIndex=5)
    ConstantPool cp;
    cp.entries.resize(7);
    cp.entries[1].tag = CONSTANT_UTF8; cp.entries[1].utf8 = "Foo";
    cp.entries[2].tag = CONSTANT_CLASS; cp.entries[2].nameIndex = 1;
    cp.entries[3].tag = CONSTANT_UTF8; cp.entries[3].utf8 = "bar";
    cp.entries[4].tag = CONSTANT_UTF8; cp.entries[4].utf8 = "(I)V";
    cp.entries[5].tag = CONSTANT_NAMEANDTYPE;
    cp.entries[5].classIndex = 3;        // nom (champ reutilise)
    cp.entries[5].nameAndTypeIndex = 4;  // descripteur (champ reutilise)
    cp.entries[6].tag = CONSTANT_METHODREF;
    cp.entries[6].classIndex = 2;
    cp.entries[6].nameAndTypeIndex = 5;

    auto ref = cp.getMethodRef(6);
    ASSERT_EQ(ref.first, std::string("Foo"));
    ASSERT_EQ(ref.second, std::string("bar:(I)V"));
}

// ---------------------------------------------------------------------
// ClassFile::parse() : round-trip complet sur un .class minimal construit
// a la main -- equivalent de :
//   class TestClass extends java/lang/Object {
//       static int add(int a, int b) { return a + b; }
//   }
// ---------------------------------------------------------------------

namespace
{
    void putU8(std::vector<uint8_t> &v, uint8_t x) { v.push_back(x); }
    void putU16(std::vector<uint8_t> &v, uint16_t x)
    {
        v.push_back(static_cast<uint8_t>(x >> 8));
        v.push_back(static_cast<uint8_t>(x));
    }
    void putU32(std::vector<uint8_t> &v, uint32_t x)
    {
        v.push_back(static_cast<uint8_t>(x >> 24));
        v.push_back(static_cast<uint8_t>(x >> 16));
        v.push_back(static_cast<uint8_t>(x >> 8));
        v.push_back(static_cast<uint8_t>(x));
    }
    void putUtf8Entry(std::vector<uint8_t> &v, const char *s)
    {
        putU8(v, CONSTANT_UTF8);
        uint16_t len = static_cast<uint16_t>(std::strlen(s));
        putU16(v, len);
        for (uint16_t i = 0; i < len; i++)
            v.push_back(static_cast<uint8_t>(s[i]));
    }
    void putClassEntry(std::vector<uint8_t> &v, uint16_t nameIdx)
    {
        putU8(v, CONSTANT_CLASS);
        putU16(v, nameIdx);
    }

    std::vector<uint8_t> buildMinimalClass()
    {
        std::vector<uint8_t> v;
        putU32(v, 0xCAFEBABE);
        putU16(v, 0);  // minor
        putU16(v, 49); // major (<=52 exige par ClassFile::parse)

        // constant_pool_count = 8 (entrees 1..7 utilisees)
        putU16(v, 8);
        putUtf8Entry(v, "TestClass");        // #1
        putClassEntry(v, 1);                 // #2 this_class
        putUtf8Entry(v, "java/lang/Object");  // #3
        putClassEntry(v, 3);                 // #4 super_class
        putUtf8Entry(v, "add");              // #5 method name
        putUtf8Entry(v, "(II)I");            // #6 method desc
        putUtf8Entry(v, "Code");             // #7 attribute name "Code"

        putU16(v, 0x0021); // access_flags: ACC_PUBLIC|ACC_SUPER
        putU16(v, 2);      // this_class
        putU16(v, 4);      // super_class
        putU16(v, 0);      // interfaces_count
        putU16(v, 0);      // fields_count

        putU16(v, 1); // methods_count
        putU16(v, 0x0009); // access_flags: ACC_PUBLIC|ACC_STATIC
        putU16(v, 5);      // name_index -> "add"
        putU16(v, 6);      // descriptor_index -> "(II)I"
        putU16(v, 1);      // attributes_count

        // Code attribute: iload_0; iload_1; iadd; ireturn
        std::vector<uint8_t> code = {0x1a, 0x1b, 0x60, 0xac};
        std::vector<uint8_t> codeAttrBody;
        putU16(codeAttrBody, 2); // max_stack
        putU16(codeAttrBody, 2); // max_locals
        putU32(codeAttrBody, static_cast<uint32_t>(code.size()));
        for (uint8_t b : code)
            codeAttrBody.push_back(b);
        putU16(codeAttrBody, 0); // exception_table_length
        putU16(codeAttrBody, 0); // attributes_count

        putU16(v, 7); // attribute_name_index -> "Code"
        putU32(v, static_cast<uint32_t>(codeAttrBody.size()));
        for (uint8_t b : codeAttrBody)
            v.push_back(b);

        putU16(v, 0); // class attributes_count
        return v;
    }
} // namespace

TEST(class_file_parse_minimal_class_roundtrip)
{
    std::vector<uint8_t> bytes = buildMinimalClass();
    ClassFile cf;
    ASSERT_TRUE(ClassFile::parse(bytes.data(), bytes.size(), cf));

    ASSERT_EQ(cf.thisClassName(), std::string("TestClass"));
    ASSERT_EQ(cf.superClassName(), std::string("java/lang/Object"));
    ASSERT_EQ(cf.methods.size(), (size_t)1);

    const MethodInfo &m = cf.methods[0];
    ASSERT_EQ(m.name(cf.constantPool), std::string("add"));
    ASSERT_EQ(m.desc(cf.constantPool), std::string("(II)I"));
    ASSERT_TRUE(m.isStatic());
    ASSERT_TRUE(m.code() != nullptr);
    ASSERT_EQ(m.code()->maxStack, (uint16_t)2);
    ASSERT_EQ(m.code()->maxLocals, (uint16_t)2);
    ASSERT_EQ(m.code()->code.size(), (size_t)4);
    ASSERT_EQ((int)m.code()->code[2], 0x60); // iadd
}

TEST(class_file_parse_rejects_bad_magic)
{
    std::vector<uint8_t> bytes = buildMinimalClass();
    bytes[0] = 0x00; // corrompt le magic CAFEBABE
    ClassFile cf;
    ASSERT_FALSE(ClassFile::parse(bytes.data(), bytes.size(), cf));
}

TEST(class_file_parse_rejects_truncated_buffer)
{
    std::vector<uint8_t> bytes = buildMinimalClass();
    bytes.resize(bytes.size() / 2); // coupe en plein milieu
    ClassFile cf;
    ASSERT_FALSE(ClassFile::parse(bytes.data(), bytes.size(), cf));
}
