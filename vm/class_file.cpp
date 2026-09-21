#include "class_file.h"

#include <cstring>
#include <functional>

namespace jvm
{

static bool checkMagic(const uint8_t *data, size_t len)
{
    if (len < 8)
        return false;
    return
        data[0] == 0xCA && data[1] == 0xFE &&
        data[2] == 0xBA && data[3] == 0xBE;
}

static uint16_t readU16(const uint8_t *p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
static uint32_t readU32(const uint8_t *p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; }

const CpEntry *ConstantPool::get(uint16_t index) const
{
    if (index == 0 || index >= entries.size())
        return nullptr;
    return &entries[index];
}

std::string ConstantPool::getUtf8(uint16_t index) const
{
    const CpEntry *e = get(index);
    if (!e || e->tag != CONSTANT_UTF8)
        return {};
    return e->utf8;
}

int32_t ConstantPool::getInt(uint16_t index) const
{
    const CpEntry *e = get(index);
    if (!e || e->tag != CONSTANT_INTEGER)
        return 0;
    return e->intVal;
}

int64_t ConstantPool::getLong(uint16_t index) const
{
    const CpEntry *e = get(index);
    if (!e || e->tag != CONSTANT_LONG)
        return 0;
    return e->longVal;
}

std::string ConstantPool::getClassName(uint16_t index) const
{
    const CpEntry *e = get(index);
    if (!e || e->tag != CONSTANT_CLASS)
        return {};
    return getUtf8(e->nameIndex);
}

std::pair<std::string, std::string> ConstantPool::getFieldRef(uint16_t index) const
{
    const CpEntry *e = get(index);
    if (!e || (e->tag != CONSTANT_FIELDREF))
        return {};
    const CpEntry *nt = get(e->nameAndTypeIndex);
    if (!nt || nt->tag != CONSTANT_NAMEANDTYPE)
        return {};
    return {getClassName(e->classIndex), getUtf8(nt->classIndex) + ":" + getUtf8(nt->nameAndTypeIndex)};
}

std::pair<std::string, std::string> ConstantPool::getMethodRef(uint16_t index) const
{
    const CpEntry *e = get(index);
    if (!e || (e->tag != CONSTANT_METHODREF && e->tag != CONSTANT_INTERFACE_METHODREF))
        return {};
    const CpEntry *nt = get(e->nameAndTypeIndex);
    if (!nt || nt->tag != CONSTANT_NAMEANDTYPE)
        return {};
    return {getClassName(e->classIndex), getUtf8(nt->classIndex) + ":" + getUtf8(nt->nameAndTypeIndex)};
}

std::pair<std::string, std::string> ConstantPool::getNameAndType(uint16_t index) const
{
    const CpEntry *e = get(index);
    if (!e || e->tag != CONSTANT_NAMEANDTYPE)
        return {};
    return {getUtf8(e->classIndex), getUtf8(e->nameAndTypeIndex)};
}

std::string ClassFile::thisClassName() const { return constantPool.getClassName(thisClass); }
std::string ClassFile::superClassName() const { return constantPool.getClassName(superClass); }

bool ClassFile::parseAttribute(const ConstantPool &cp, const uint8_t *data, size_t len, size_t &offset, AttributeInfo &out)
{
    if (offset + 6 > len)
        return false;
    uint16_t nameIdx = readU16(data + offset);
    uint32_t attrLen = readU32(data + offset + 2);
    offset += 6;
    if (offset + attrLen > len)
        return false;
    out.name = cp.getUtf8(nameIdx);
    out.data.assign(data + offset, data + offset + attrLen);
    offset += attrLen;
    return true;
}

bool ClassFile::parseCodeAttribute(const ConstantPool &cp, const uint8_t *data, size_t len, size_t &offset, CodeAttribute &out)
{
    if (offset + 8 > len)
        return false;
    out.maxStack = readU16(data + offset);
    out.maxLocals = readU16(data + offset + 2);
    uint32_t codeLen = readU32(data + offset + 4);
    offset += 8;
    if (offset + codeLen + 2 > len)
        return false;
    out.code.assign(data + offset, data + offset + codeLen);
    offset += codeLen;

    uint16_t excCount = readU16(data + offset);
    offset += 2;
    if (offset + excCount * 8 > len)
        return false;
    out.handlers.reserve(excCount);
    for (uint16_t i = 0; i < excCount; i++)
    {
        CodeAttribute::ExceptionHandler h;
        h.startPc = readU16(data + offset);
        h.endPc = readU16(data + offset + 2);
        h.handlerPc = readU16(data + offset + 4);
        h.catchType = readU16(data + offset + 6);
        out.handlers.push_back(h);
        offset += 8;
    }

    uint16_t attrCount = readU16(data + offset);
    offset += 2;
    out.attributes.reserve(attrCount);
    for (uint16_t i = 0; i < attrCount; i++)
    {
        AttributeInfo a;
        if (!parseAttribute(cp, data, len, offset, a))
            return false;
        out.attributes.push_back(std::move(a));
    }
    return true;
}

bool ClassFile::parse(const uint8_t *data, size_t len, ClassFile &out)
{
    if (!checkMagic(data, len))
        return false;
    size_t off = 0;
    out.magic = readU32(data);
    out.minorVersion = readU16(data + 4);
    out.majorVersion = readU16(data + 6);
    off = 8;

    if (out.majorVersion > 52)
        return false;

    uint16_t cpCount = readU16(data + off);
    off += 2;
    out.constantPool.entries.resize(cpCount);
    for (uint16_t i = 1; i < cpCount; i++)
    {
        if (off >= len)
            return false;
        CpEntry &e = out.constantPool.entries[i];
        e.tag = static_cast<CpTag>(data[off++]);
        switch (e.tag)
        {
        case CONSTANT_UTF8:
        {
            if (off + 2 > len)
                return false;
            uint16_t ulen = readU16(data + off);
            off += 2;
            if (off + ulen > len)
                return false;
            e.utf8.assign(reinterpret_cast<const char *>(data + off), ulen);
            off += ulen;
            break;
        }
        case CONSTANT_INTEGER:
        case CONSTANT_FLOAT:
            if (off + 4 > len)
                return false;
            e.intVal = static_cast<int32_t>(readU32(data + off));
            std::memcpy(&e.floatVal, &e.intVal, 4);
            off += 4;
            break;
        case CONSTANT_LONG:
        case CONSTANT_DOUBLE:
        {
            if (off + 8 > len)
                return false;
            uint64_t v = 0;
            for (int b = 0; b < 8; b++)
                v = (v << 8) | data[off + b];
            e.longVal = static_cast<int64_t>(v);
            std::memcpy(&e.doubleVal, &e.longVal, 8);
            off += 8;
            i++;
            if (i < cpCount)
                out.constantPool.entries[i].tag = CONSTANT_UTF8;
            break;
        }
        case CONSTANT_CLASS:
        case CONSTANT_STRING:
        case CONSTANT_METHODTYPE:
            if (off + 2 > len)
                return false;
            e.nameIndex = readU16(data + off);
            off += 2;
            break;
        case CONSTANT_FIELDREF:
        case CONSTANT_METHODREF:
        case CONSTANT_INTERFACE_METHODREF:
        case CONSTANT_NAMEANDTYPE:
            if (off + 4 > len)
                return false;
            e.classIndex = readU16(data + off);
            e.nameAndTypeIndex = readU16(data + off + 2);
            off += 4;
            break;
        case CONSTANT_METHODHANDLE:
            if (off + 3 > len)
                return false;
            e.refKind = data[off];
            e.refIndex = readU16(data + off + 1);
            off += 3;
            break;
        case CONSTANT_INVOKEDYNAMIC:
            if (off + 4 > len)
                return false;
            e.bootstrapMethodAttrIndex = readU16(data + off);
            e.nameAndTypeIndex = readU16(data + off + 2);
            off += 4;
            break;
        default:
            return false;
        }
    }

    if (off + 6 > len)
        return false;
    out.accessFlags = readU16(data + off);
    out.thisClass = readU16(data + off + 2);
    out.superClass = readU16(data + off + 4);
    off += 6;

    uint16_t ifaceCount = readU16(data + off);
    off += 2;
    if (off + ifaceCount * 2 > len)
        return false;
    out.interfaces.resize(ifaceCount);
    for (uint16_t i = 0; i < ifaceCount; i++)
        out.interfaces[i] = readU16(data + off + i * 2);
    off += ifaceCount * 2;

    uint16_t fieldCount = readU16(data + off);
    off += 2;
    out.fields.reserve(fieldCount);
    for (uint16_t i = 0; i < fieldCount; i++)
    {
        FieldInfo f;
        if (off + 6 > len)
            return false;
        f.accessFlags = readU16(data + off);
        f.nameIndex = readU16(data + off + 2);
        f.descIndex = readU16(data + off + 4);
        off += 6;
        uint16_t attrCount = readU16(data + off);
        off += 2;
        f.attributes.reserve(attrCount);
        for (uint16_t a = 0; a < attrCount; a++)
        {
            AttributeInfo attr;
            if (!parseAttribute(out.constantPool, data, len, off, attr))
                return false;
            f.attributes.push_back(std::move(attr));
        }
        out.fields.push_back(std::move(f));
    }

    uint16_t methodCount = readU16(data + off);
    off += 2;
    out.methods.reserve(methodCount);
    for (uint16_t i = 0; i < methodCount; i++)
    {
        MethodInfo m;
        if (off + 6 > len)
            return false;
        m.accessFlags = readU16(data + off);
        m.nameIndex = readU16(data + off + 2);
        m.descIndex = readU16(data + off + 4);
        off += 6;
        uint16_t attrCount = readU16(data + off);
        off += 2;
        m.attributes.reserve(attrCount);
        for (uint16_t a = 0; a < attrCount; a++)
        {
            size_t attrStart = off;
            AttributeInfo attr;
            if (!parseAttribute(out.constantPool, data, len, off, attr))
                return false;
            if (attr.name == "Code")
            {
                auto code = std::make_unique<CodeAttribute>();
                size_t inner = 0;
                if (!parseCodeAttribute(out.constantPool, attr.data.data(), attr.data.size(), inner, *code))
                    return false;
                m.codeAttr = std::move(code);
            }
            m.attributes.push_back(std::move(attr));
            (void)attrStart;
        }
        out.methods.push_back(std::move(m));
    }

    uint16_t attrCount = readU16(data + off);
    off += 2;
    out.attributes.reserve(attrCount);
    for (uint16_t i = 0; i < attrCount; i++)
    {
        AttributeInfo a;
        if (!parseAttribute(out.constantPool, data, len, off, a))
            return false;
        out.attributes.push_back(std::move(a));
    }

    return true;
}

FieldDesc parseFieldDesc(const char *&p)
{
    FieldDesc fd;
    while (*p == '[')
    {
        fd.arrayDim++;
        p++;
    }
    char c = *p++;
    fd.baseType = c;
    switch (c)
    {
    case 'L':
    {
        const char *start = p;
        while (*p && *p != ';')
            p++;
        fd.className.assign(start, p);
        if (*p == ';')
            p++;
        break;
    }
    case 'B': case 'C': case 'D': case 'F': case 'I': case 'J': case 'S': case 'Z':
        break;
    default:
        fd.baseType = 0;
        break;
    }
    return fd;
}

MethodDesc parseMethodDesc(const char *desc)
{
    MethodDesc md;
    const char *p = desc;
    if (*p != '(')
        return md;
    p++;
    while (*p && *p != ')')
        md.params.push_back(parseFieldDesc(p));
    if (*p == ')')
        p++;
    if (*p == 'V')
    {
        md.returnType.baseType = 'V';
        p++;
    }
    else
    {
        md.returnType = parseFieldDesc(p);
    }
    return md;
}

} // namespace jvm