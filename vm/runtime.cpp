#include "runtime.h"
#include "../hal/jar_reader.h"

#include <cstdio>

namespace jvm
{

// ---------------------------------------------------------------------
// Heap
// ---------------------------------------------------------------------

Heap::Heap(size_t poolSize)
{
    initCap_ = poolSize ? poolSize : kDefaultPoolSize;
    auto *seg = new uint8_t[initCap_];
    segs_.push_back(seg);
    segCaps_.push_back(initCap_);
    capTotal_ = initCap_;
}

Heap::~Heap()
{
    for (uint8_t *s : segs_)
        delete[] s;
}

void Heap::reset()
{
    usedTotal_ = 0;
    off_ = 0;
    for (size_t i = 1; i < segs_.size(); i++)
    {
        delete[] segs_[i];
    }
    segs_.resize(1);
    segCaps_.resize(1);
    capTotal_ = initCap_;
    classCache_.clear();
    strings_.clear();
    oom_ = false;
}

Obj *Heap::allocObj(ObjKind kind, int32_t cells)
{
    cells = (cells > 0) ? cells : 0;
    size_t need = sizeof(Obj) + cells * sizeof(Value);
    size_t freeInCur = segCaps_.back() - off_;
    if (need > freeInCur)
    {
        // Auto-grow : nouveau segment assez grand (double au moins).
        size_t growTo = std::max(capTotal_ * 2, initCap_);
        growTo = std::max(growTo, usedTotal_ + need);
        size_t segSize = growTo - capTotal_;
        uint8_t *seg = nullptr;
        try
        {
            seg = new uint8_t[segSize];
        }
        catch (...)
        {
            seg = nullptr;
        }
        if (!seg)
        {
            if (getenv("JME_DEBUG"))
                fprintf(stderr, "allocObj OOM: kind=%d cells=%d need=%zu used=%zu cap=%zu\n",
                        (int)kind, cells, need, usedTotal_, capTotal_);
            oom_ = true;
            return nullptr;
        }
        segs_.push_back(seg);
        segCaps_.push_back(segSize);
        capTotal_ += segSize;
        off_ = 0;
    }
    Obj *o = reinterpret_cast<Obj *>(segs_.back() + off_);
    off_ += need;
    usedTotal_ += need;
    o->kind = kind;
    o->cls = nullptr;
    o->cellCount = cells;
    o->cells = (cells > 0) ? reinterpret_cast<Value *>(o + 1) : nullptr;
    o->arrayLen = 0;
    ::new (static_cast<void *>(&o->str)) std::string();
    o->str.clear();
    if (cells > 0)
    {
        for (int32_t i = 0; i < cells; i++)
            o->cells[i] = Value();
    }
    return o;
}

Obj *Heap::newString(const std::string &s)
{
    Obj *o = allocObj(ObjKind::String, 0);
    if (!o)
        return nullptr;
    o->str = s;
    strings_.push_back(o);
    return o;
}

Obj *Heap::newStringCat(Obj *a, Obj *b)
{
    if (!a) a = newString("");
    if (!b) b = newString("");
    return newString(a->str + b->str);
}

Obj *Heap::newArray(ObjKind kind, int32_t len)
{
    if (len < 0)
        return nullptr;
    Obj *o = allocObj(kind, len);
    if (!o)
        return nullptr;
    o->arrayLen = len;
    return o;
}

Obj *Heap::newInstance(ClassInfo *ci)
{
    Obj *o = allocObj(ObjKind::Instance, ci->instanceCells);
    if (!o)
        return nullptr;
    o->cls = ci;
    o->arrayLen = 0;
    return o;
}

Obj *Heap::classObjFor(const std::string &name)
{
    auto it = classCache_.find(name);
    if (it != classCache_.end())
        return it->second;
    Obj *o = allocObj(ObjKind::Class, 0);
    if (!o)
        return nullptr;
    o->str = name;
    classCache_[name] = o;
    return o;
}

// ---------------------------------------------------------------------
// ClassInfo helpers
// ---------------------------------------------------------------------

static int sizeOfSlot(const std::string &desc)
{
    if (desc.size() == 1)
    {
        char c = desc[0];
        if (c == 'J' || c == 'D')
            return 2;
        return 1;
    }
    // tableaux ou objets
    return 1;
}

const MethodRecord *ClassInfo::findMethod(const std::string &nm, const std::string &ds) const
{
    for (const auto &m : methods)
        if (m.name == nm && m.desc == ds)
            return &m;
    return nullptr;
}

const MethodRecord *ClassInfo::findMethodVirtual(const std::string &nm, const std::string &ds) const
{
    const ClassInfo *c = this;
    while (c)
    {
        const MethodRecord *m = c->findMethod(nm, ds);
        if (m)
            return m;
        c = c->super;
    }
    return nullptr;
}

const MethodRecord *ClassInfo::findClinit() const
{
    return findMethod("<clinit>", "()V");
}

const MethodRecord *ClassInfo::findField(const std::string &nm, const std::string &ds) const
{
    if (ds.empty())
    {
        for (const auto &f : fields)
            if (f.name == nm)
                return &f;
        return nullptr;
    }
    for (const auto &f : fields)
        if (f.name == nm && f.desc == ds)
            return &f;
    return nullptr;
}

const MethodRecord *ClassInfo::findFieldRecursive(const std::string &nm, const std::string &ds) const
{
    const ClassInfo *c = this;
    while (c)
    {
        const MethodRecord *f = c->findField(nm, ds);
        if (f)
            return f;
        c = c->super;
    }
    return nullptr;
}

int ClassInfo::findStaticIndex(const std::string &nm) const
{
    for (size_t i = 0; i < fields.size(); i++)
        if (fields[i].name == nm)
            return i;
    return -1;
}

// ---------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------

ClassInfo *Runtime::buildFromClassFile(ClassFile *cf)
{
    auto ci = std::make_unique<ClassInfo>();
    ci->name = cf->thisClassName();
    ci->cf = cf;
    ci->rt = this;

    int staticIdx = 0;
    for (const auto &f : cf->fields)
    {
        MethodRecord r;
        r.name = f.name(cf->constantPool);
        r.desc = f.desc(cf->constantPool);
        r.mi = nullptr;
        r.isField = true;
        r.isStatic = f.isStatic();
        r.owner = ci.get();
        if (f.isStatic())
        {
            r.slot = staticIdx++;
            ci->statics.push_back(Value());
        }
        else
        {
            r.slot = 0; // rempli par linkSuper
        }
        ci->fields.push_back(r);
    }

    for (const auto &m : cf->methods)
    {
        MethodRecord r;
        r.name = m.name(cf->constantPool);
        r.desc = m.desc(cf->constantPool);
        r.mi = &m;
        r.isField = false;
        r.owner = ci.get();
        ci->methods.push_back(r);
    }

    ClassInfo *out = ci.get();
    classes_[ci->name] = std::move(ci);
    linkSuper(out);
    return out;
}

void Runtime::linkSuper(ClassInfo *ci)
{
    if (!ci->cf)
        return;

    int base = 0;
    if (ci->cf->superClass != 0 && ci->cf->superClassName() != ci->name)
    {
        std::string superName = ci->cf->superClassName();
        ClassInfo *s = resolveClass(superName);
        if (!s)
            s = lookupNativeClass(superName);
        if (!s && jar_)
            s = loadFromJar(superName);
        if (s)
        {
            ci->super = s;
            base = s->instanceCells;
        }
    }
    ci->instanceCells = base;
    for (auto &f : ci->fields)
    {
        if (f.isStatic)
            continue;
        f.slot = ci->instanceCells;
        ci->instanceCells += sizeOfSlot(f.desc);
    }
}

ClassInfo *Runtime::resolveClass(const std::string &internalName)
{
    auto it = classes_.find(internalName);
    if (it != classes_.end())
        return it->second.get();
    return nullptr;
}

ClassInfo *Runtime::classInfoOfName(const std::string &internalName)
{
    return resolveClass(internalName);
}

ClassInfo *Runtime::lookupNativeClass(const std::string &internalName)
{
    auto it = classes_.find(internalName);
    if (it != classes_.end() && it->second->isNativeClass)
        return it->second.get();
    return nullptr;
}

ClassInfo *Runtime::loadFromJar(const std::string &internalName)
{
    auto it = classes_.find(internalName);
    if (it != classes_.end())
        return it->second.get();

    if (!jar_)
        return nullptr;

    static uint8_t buf[256 * 1024];
    size_t n = jar_->extractClass(internalName, buf, sizeof(buf));
    if (n == 0)
        return nullptr;

    auto cf = std::make_unique<ClassFile>();
    if (!ClassFile::parse(buf, n, *cf))
        return nullptr;

    ClassFile *raw = cf.get();
    jarClasses_[cf->thisClassName()] = std::move(cf);
    return buildFromClassFile(raw);
}

ClassInfo *Runtime::registerNativeClass(
    const std::string &internalName,
    const std::string &superName,
    const std::vector<std::pair<std::string, std::string>> &methodSig,
    const std::vector<std::pair<std::string, std::string>> &fieldSig)
{
    auto ci = std::make_unique<ClassInfo>();
    ci->name = internalName;
    ci->cf = nullptr;
    ci->rt = this;
    ci->isNativeClass = true;

    for (const auto &f : fieldSig)
    {
        MethodRecord r;
        r.name = f.first;
        r.desc = f.second;
        r.isField = true;
        r.isStatic = false;
        r.slot = 0; // rempli ci-dessous
        r.owner = ci.get();
        ci->fields.push_back(r);
    }

    for (const auto &m : methodSig)
    {
        MethodRecord r;
        r.name = m.first;
        r.desc = m.second;
        r.mi = nullptr;
        r.isField = false;
        r.owner = ci.get();
        ci->methods.push_back(r);
    }

    int base = 0;
    if (!superName.empty())
    {
        ClassInfo *s = resolveClass(superName);
        if (!s)
            s = lookupNativeClass(superName);
        ci->super = s;
        if (s)
            base = s->instanceCells;
    }
    ci->instanceCells = base;
    for (auto &f : ci->fields)
    {
        f.slot = ci->instanceCells;
        ci->instanceCells += sizeOfSlot(f.desc);
        // Les champs de classe native déclarés ici (fieldSig) sont toujours
        // marqués isStatic=false et indexés en cellules d'INSTANCE (voir
        // plus haut) -- correct pour l'état caché type GC_FULLSCREEN/GC_GFX,
        // couleur de Graphics, etc. Mais si le bytecode du jeu accède à ce
        // champ via getstatic/putstatic (ex. le vrai java/lang/System.out,
        // static par nature), l'interpréteur indexe owner->statics[f.slot]
        // -- vecteur resté vide faute d'être jamais rempli ici, d'où un
        // plantage (vector::operator[] hors bornes). On garde `statics`
        // assez grand pour couvrir n'importe quel slot de champ natif : la
        // même valeur reste alors lisible/écrivable via getstatic ET
        // getfield, au prix de quelques cellules perdues (classes natives
        // ont très peu de champs).
        if (ci->statics.size() <= static_cast<size_t>(f.slot))
            ci->statics.resize(f.slot + 1);
    }

    ClassInfo *out = ci.get();
    classes_[internalName] = std::move(ci);
    return out;
}

void Runtime::reportOom()
{
    fprintf(stderr, "JVM: heap epuisee (%zu/%zu octets)\n", heap_.used(), heap_.capacity());
}

} // namespace jvm