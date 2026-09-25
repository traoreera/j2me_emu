#pragma once

#include "core/class_file.h"
#include "hal/jar_reader.h"

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace jvm
{

// ---------------------------------------------------------------------
// Value : slot de 64 bits (couvre int, float, référence, long, double)
// ---------------------------------------------------------------------
struct Obj;

struct Value
{
    union
    {
        int32_t i;
        float f;
        Obj *o;
        int64_t l;
        uint64_t u;
    };
    Value() : u(0) {}
    static Value fromInt(int32_t v) { Value r; r.i = v; return r; }
    static Value fromFloat(float v) { Value r; r.f = v; return r; }
    static Value fromLong(int64_t v) { Value r; r.l = v; return r; }
    static Value fromRef(Obj *v) { Value r; r.o = v; return r; }
};

enum class ObjKind : uint8_t
{
    Null = 0,
    Instance,
    String,
    Class,
    ByteArray,
    ShortArray,
    IntArray,
    LongArray,
    FloatArray,
    DoubleArray,
    CharArray,
    BoolArray,
    ObjArray,
};

struct ClassInfo;

struct Obj
{
    ObjKind kind = ObjKind::Null;
    ClassInfo *cls = nullptr;   // Instance / Class
    int32_t cellCount = 0;      // nombre de slots Value
    Value *cells = nullptr;     // champs d'instance / données de tableau
    int32_t arrayLen = 0;
    std::string str;            // payload String
};

// ---------------------------------------------------------------------
// Runtime : classes chargées, statics, heap
// ---------------------------------------------------------------------

struct MethodRecord
{
    std::string name;
    std::string desc;
    const MethodInfo *mi = nullptr;  // nullptr pour les natives synthétiques
    int slot = 0;                    // champ instance: cell offset absolu; champ static: index statics
    bool isField = false;
    bool isStatic = false;
    ClassInfo *owner = nullptr;      // classe déclarante (résolution natives virtuelles)
};

class Runtime;

// Info de classe au runtime
struct ClassInfo
{
    std::string name;
    ClassFile *cf = nullptr;                 // nullptr = classe native synthétique
    Runtime *rt = nullptr;
    ClassInfo *super = nullptr;
    std::vector<ClassInfo *> interfaces;
    std::vector<MethodRecord> methods;
    std::vector<MethodRecord> fields;        // champs d'instance (ordre de déclaration)
    int instanceCells = 0;                   // nombre de slots d'instance (toute hiérarchie)
    std::vector<Value> statics;              // slots statiques (ordre de déclaration, sans double-compte long)
    bool isNativeClass = false;

    const MethodRecord *findMethod(const std::string &nm, const std::string &ds) const;
    const MethodRecord *findMethodVirtual(const std::string &nm, const std::string &ds) const;
    // `ds` (descripteur de type, ex. "I", "[B", "Ljava/lang/String;") est
    // optionnel mais fortement recommandé : le bytecode obfusqué réutilise
    // couramment le même nom de champ pour plusieurs types distincts dans
    // une même classe (name-mangling agressif pour minimiser la taille du
    // .class) -- sans désambiguïsation par type, ces champs s'aliasent tous
    // sur le même slot mémoire (observé : un champ Image et un champ
    // InputStream tous deux nommés "a" sur la même classe, corrompant l'un
    // l'autre). `ds` vide = comportement historique (premier match par nom).
    const MethodRecord *findField(const std::string &nm, const std::string &ds = "") const;
    const MethodRecord *findFieldRecursive(const std::string &nm, const std::string &ds = "") const;
    int findStaticIndex(const std::string &nm) const;
    const MethodRecord *findClinit() const;
    bool clinitDone = false;

    // Cache de résolution "index du pool de constantes -> champ résolu",
    // pour getfield/putfield/getstatic/putstatic (interpreter.cpp). Rempli
    // paresseusement, jamais par le class loader -- l'index est toujours
    // relatif à cf->constantPool, LE POOL DE CETTE CLASSE (celle qui
    // contient le bytecode exécuté), donc valide pour toute la durée de vie
    // du programme une fois résolu une première fois. Sans ce cache,
    // chaque exécution du même getfield/putfield reparse la chaîne
    // "nom:desc" du Fieldref ET refait une recherche linéaire dans
    // ClassInfo::fields/statics -- mesuré (gprof) comme le point chaud
    // dominant du VM : ~50% du temps CPU total sur games/gangstar_2 (~4.7M
    // appels de ClassInfo::findField en 40 frames), le genre de jeu dont le
    // bytecode par-instance/par-frame fait énormément de lectures/écritures
    // de champs. `tc` (classe référencée par le Fieldref, nécessaire pour
    // ensureInit() sur getstatic/putstatic) n'est renseigné que pour ces
    // deux opcodes ; `field->owner` donne la classe qui déclare réellement
    // le champ (peut différer de `tc` pour un champ static hérité). Pour
    // getfield/putfield (champs d'instance, sans ensureInit), `tc` reste
    // nullptr et n'est pas utilisé.
    struct FieldCacheEntry
    {
        ClassInfo *tc = nullptr;
        const MethodRecord *field = nullptr;
        int w = 1; // slots du champ (2 pour long/double), pré-calculé : évite de rescanner le descripteur
    };
    std::vector<FieldCacheEntry> fieldRefCache;

    // Cache de résolution des invoke* (même principe que fieldRefCache, indexé
    // par l'index du Methodref dans le pool de cette classe) : évite, à CHAQUE
    // appel, de reparser "classe/nom:desc", de rechercher la classe par son nom
    // (table de hachage de chaînes) et de rechercher la méthode par
    // (nom, descripteur) -- comparaison de chaînes dans un scan linéaire de
    // ClassInfo::methods, remontant la chaîne de super. Mesuré (gprof) sur
    // games/gangstar_rio : ClassInfo::findMethod 26 % du CPU + getMethodRef/
    // classInfoOfName/hash ~8 %, 5,2 M appels pendant le chargement. Pour
    // invokestatic/invokespecial la cible est fixe (`staticM`) ; pour
    // invokevirtual/interface elle dépend de la classe réelle du récepteur :
    // cache monomorphe (dernier récepteur -> dernière méthode).
    struct MethodCacheEntry
    {
        bool valid = false;
        ClassInfo *tc = nullptr;          // classe référencée par le Methodref
        std::string name, desc;
        int nslots = 0;                   // slots d'arguments (hors récepteur)
        bool rv = true;                   // retour void ?
        char retType = 'V';
        const MethodRecord *staticM = nullptr; // invokestatic/special : méthode résolue
        ClassInfo *lastRecv = nullptr;         // invokevirtual : dernier type de récepteur
        const MethodRecord *lastM = nullptr;   //                 et méthode correspondante
    };
    std::vector<MethodCacheEntry> methodRefCache;
};

class Heap
{
public:
    // 224 KB correspond au budget visé sur RP2040 (264 KB de RAM totale).
    // Sur PC, le confort de dev prime : sans GC (bump allocator, reset()
    // seul point de récupération), des jeux qui allouent beaucoup d'objets
    // courts (ex. génération procédurale) épuisent vite 224 KB. On monte à
    // 512 KB pour le dev PC ; repasser à 224 * 1024 (ou moins) lors du
    // portage RP2040 -- même logique que JAR_READER_INDEX_IN_RAM.
    static constexpr size_t kDefaultPoolSize = 512 * 1024;
    // maxSize = plafond de capacité TOTALE (0 = illimité, comportement dev PC).
    // Un plafond < poolSize est relevé à poolSize. Au-delà : OOM, jamais d'écriture hors segment.
    explicit Heap(size_t poolSize = kDefaultPoolSize, size_t maxSize = 0);
    ~Heap();

    Heap(const Heap &) = delete;
    Heap &operator=(const Heap &) = delete;

    Obj *allocObj(ObjKind kind, int32_t cells);
    Obj *newString(const std::string &s);
    Obj *newStringCat(Obj *a, Obj *b);
    // Chaîne INTERNÉE : le même contenu renvoie toujours le même Obj (littéraux
    // `ldc`, String.intern()). Indispensable : le bytecode compare couramment des
    // littéraux par identité (`if_acmpeq`), ce qui suppose l'interning JVM.
    Obj *internString(const std::string &s);
    Obj *newArray(ObjKind kind, int32_t len);
    Obj *newInstance(ClassInfo *ci);
    Obj *classObjFor(const std::string &name); // cherche dans le cache des Class Obj
    size_t used() const { return usedTotal_; }
    size_t capacity() const { return capTotal_; }
    size_t maximumCapacity() const { return maxCap_; }
    bool outOfMemory() const { return oom_; }
    void reset();

private:
    // Segments de mémoire : le bump allocator défile dans chaque segment et
    // pousse un NOUVEAU segment quand besoin (auto-grow). Les objets ne sont
    // jamais déplacés (un Obj embarque un std::string → non relocalisable par
    // memcpy), donc chaque segment reste physique et immuable. Cette souplesse
    // sert le dev PC : des jeux qui allouent beaucoup (texte, typewriter,
    // ressources) finissaient par mourir en "heap epuisee" sans GC. Sur RP2040
    // on peut brider via JME_HEAP_MAX ou revenir à un pool unique figé.
    std::vector<uint8_t *> segs_;
    std::vector<size_t> segCaps_;
    size_t initCap_ = 0;    // taille du premier segment (JME_HEAP)
    size_t maxCap_ = 0;     // plafond de capacité totale (JME_HEAP_MAX), 0 = aucun
    size_t capTotal_ = 0;   // capacité totale allouée (init + segments étendus)
    size_t usedTotal_ = 0;  // octets consommés au total
    size_t off_ = 0;        // offset courant dans le dernier segment
    bool oom_ = false;
    std::unordered_map<std::string, Obj *> classCache_;
    std::unordered_map<std::string, Obj *> internTable_;
    std::vector<Obj *> strings_; // garde les strings (bump allocator, jamais libérés)
};

class Runtime
{
public:
    Runtime(size_t heapSize = Heap::kDefaultPoolSize, size_t heapMax = 0)
        : heap_(heapSize, heapMax) {}

    Heap &heap() { return heap_; }
    const Heap &heap() const { return heap_; }

    // Charge une classe depuis le JAR (nom interne "com/foo/Game")
    ClassInfo *loadFromJar(const std::string &internalName);
    // Enregistre une classe native synthétique (API MIDP/CLDC)
    ClassInfo *registerNativeClass(const std::string &internalName,
                                   const std::string &superName,
                                   const std::vector<std::pair<std::string, std::string>> &methodSig,
                                   const std::vector<std::pair<std::string, std::string>> &fieldSig);

    // Résout une classe (chargée ou native), nullptr si absente
    ClassInfo *resolveClass(const std::string &internalName);
    ClassInfo *classInfoOfName(const std::string &internalName);

    // Trouve la classe native API correspondant au nom interne (sans la charger)
    ClassInfo *lookupNativeClass(const std::string &internalName);

    // Fournit la source .jar pour le chargement paresseux des classes
    void setJar(jme::JarReader *jar) { jar_ = jar; }
    jme::JarReader *jar() { return jar_; }

    void reportOom();

private:
    Heap heap_;
    jme::JarReader *jar_ = nullptr;
    std::unordered_map<std::string, std::unique_ptr<ClassFile>> jarClasses_;
    std::unordered_map<std::string, std::unique_ptr<ClassInfo>> classes_;

    ClassInfo *buildFromClassFile(ClassFile *cf);
    void linkSuper(ClassInfo *ci);
};

} // namespace jvm