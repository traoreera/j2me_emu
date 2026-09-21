#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <memory>
#include <span>

namespace jvm
{

// Forward declarations
struct ClassFile;
struct ConstantPool;
struct MethodInfo;
struct FieldInfo;
struct AttributeInfo;
struct CodeAttribute;

// --- Constant Pool Tags ---
enum CpTag : uint8_t
{
    CONSTANT_UTF8 = 1,
    CONSTANT_INTEGER = 3,
    CONSTANT_FLOAT = 4,
    CONSTANT_LONG = 5,
    CONSTANT_DOUBLE = 6,
    CONSTANT_CLASS = 7,
    CONSTANT_STRING = 8,
    CONSTANT_FIELDREF = 9,
    CONSTANT_METHODREF = 10,
    CONSTANT_INTERFACE_METHODREF = 11,
    CONSTANT_NAMEANDTYPE = 12,
    CONSTANT_METHODHANDLE = 15,
    CONSTANT_METHODTYPE = 16,
    CONSTANT_INVOKEDYNAMIC = 18,
};

// --- Access Flags ---
enum AccessFlags : uint16_t
{
    ACC_PUBLIC       = 0x0001,
    ACC_PRIVATE      = 0x0002,
    ACC_PROTECTED    = 0x0004,
    ACC_STATIC       = 0x0008,
    ACC_FINAL        = 0x0010,
    ACC_SYNCHRONIZED = 0x0020,
    ACC_VOLATILE     = 0x0040,
    ACC_TRANSIENT    = 0x0080,
    ACC_NATIVE       = 0x0100,
    ACC_INTERFACE    = 0x0200,
    ACC_ABSTRACT     = 0x0400,
    ACC_STRICT       = 0x0800,
    ACC_SYNTHETIC    = 0x1000,
    ACC_ANNOTATION   = 0x2000,
    ACC_ENUM         = 0x4000,
};

// --- Constant Pool Entry ---
struct CpEntry
{
    CpTag tag = CONSTANT_UTF8;
    // Union-like storage for different entry types
    // UTF8
    std::string utf8;
    // Integer/Float
    int32_t intVal = 0;
    float floatVal = 0.0f;
    // Long/Double
    int64_t longVal = 0;
    double doubleVal = 0.0;
    // Class / String / MethodType
    uint16_t nameIndex = 0;
    // Fieldref / Methodref / InterfaceMethodref / NameAndType
    uint16_t classIndex = 0;
    uint16_t nameAndTypeIndex = 0;
    // MethodHandle
    uint8_t refKind = 0;
    uint16_t refIndex = 0;
    // InvokeDynamic
    uint16_t bootstrapMethodAttrIndex = 0;
};

// --- Constant Pool ---
struct ConstantPool
{
    std::vector<CpEntry> entries; // 1-based indexing (entries[0] unused)

    const CpEntry *get(uint16_t index) const;
    std::string getUtf8(uint16_t index) const;
    int32_t getInt(uint16_t index) const;
    int64_t getLong(uint16_t index) const;
    std::string getClassName(uint16_t index) const;        // CONSTANT_Class -> UTF8
    std::pair<std::string, std::string> getFieldRef(uint16_t index) const; // (class, name:desc)
    std::pair<std::string, std::string> getMethodRef(uint16_t index) const; // (class, name:desc)
    std::pair<std::string, std::string> getNameAndType(uint16_t index) const; // (name, desc)
};

// --- Attribute Info ---
struct AttributeInfo
{
    std::string name;
    std::vector<uint8_t> data;
};

// --- Code Attribute ---
struct CodeAttribute
{
    uint16_t maxStack = 0;
    uint16_t maxLocals = 0;
    std::vector<uint8_t> code; // bytecode
    struct ExceptionHandler
    {
        uint16_t startPc = 0;
        uint16_t endPc = 0;
        uint16_t handlerPc = 0;
        uint16_t catchType = 0; // constant pool index
    };
    std::vector<ExceptionHandler> handlers;
    std::vector<AttributeInfo> attributes;
};

// --- LineNumberTable Attribute ---
struct LineNumberTableAttribute
{
    struct Entry { uint16_t startPc; uint16_t lineNumber; };
    std::vector<Entry> entries;
};

// --- LocalVariableTable Attribute ---
struct LocalVariableTableAttribute
{
    struct Entry { uint16_t startPc; uint16_t length; uint16_t nameIndex; uint16_t descIndex; uint16_t index; };
    std::vector<Entry> entries;
};

// --- Field Info ---
struct FieldInfo
{
    uint16_t accessFlags = 0;
    uint16_t nameIndex = 0;
    uint16_t descIndex = 0;
    std::vector<AttributeInfo> attributes;

    std::string name(const ConstantPool &cp) const { return cp.getUtf8(nameIndex); }
    std::string desc(const ConstantPool &cp) const { return cp.getUtf8(descIndex); }
    bool isStatic() const { return accessFlags & ACC_STATIC; }
    bool isFinal() const { return accessFlags & ACC_FINAL; }
};

// --- Method Info ---
struct MethodInfo
{
    uint16_t accessFlags = 0;
    uint16_t nameIndex = 0;
    uint16_t descIndex = 0;
    std::vector<AttributeInfo> attributes;
    // Parsed attributes (cached)
    std::unique_ptr<CodeAttribute> codeAttr;

    std::string name(const ConstantPool &cp) const { return cp.getUtf8(nameIndex); }
    std::string desc(const ConstantPool &cp) const { return cp.getUtf8(descIndex); }
    bool isStatic() const { return accessFlags & ACC_STATIC; }
    bool isNative() const { return accessFlags & ACC_NATIVE; }
    bool isAbstract() const { return accessFlags & ACC_ABSTRACT; }

    const CodeAttribute *code() const { return codeAttr.get(); }
};

// --- Class File ---
struct ClassFile
{
    uint32_t magic = 0;
    uint16_t minorVersion = 0;
    uint16_t majorVersion = 0;
    ConstantPool constantPool;
    uint16_t accessFlags = 0;
    uint16_t thisClass = 0;
    uint16_t superClass = 0;
    std::vector<uint16_t> interfaces;
    std::vector<FieldInfo> fields;
    std::vector<MethodInfo> methods;
    std::vector<AttributeInfo> attributes;

    std::string thisClassName() const;
    std::string superClassName() const;

    // Parse from byte buffer (owned by caller, must outlive ClassFile)
    static bool parse(const uint8_t *data, size_t len, ClassFile &out);
    static bool parseAttribute(const ConstantPool &cp, const uint8_t *data, size_t len, size_t &offset, AttributeInfo &out);
    static bool parseCodeAttribute(const ConstantPool &cp, const uint8_t *data, size_t len, size_t &offset, CodeAttribute &out);
};

// --- Descriptor parsing ---
struct FieldDesc
{
    // Base type: 'B','C','D','F','I','J','S','Z', 'L' (object), '[' (array)
    char baseType = 0;
    std::string className; // for 'L' type: "java/lang/String"
    int arrayDim = 0;
};

struct MethodDesc
{
    std::vector<FieldDesc> params;
    FieldDesc returnType;
};

FieldDesc parseFieldDesc(const char *&p);
MethodDesc parseMethodDesc(const char *desc);

// --- Class Loader Interface ---
class ClassLoader
{
public:
    virtual ~ClassLoader() = default;
    virtual bool loadClass(const char *name, ClassFile &out) = 0;
    virtual const ClassFile *findLoadedClass(const char *name) = 0;
    virtual void addLoadedClass(std::unique_ptr<ClassFile> cf) = 0;
};

} // namespace jvm