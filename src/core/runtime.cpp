#include "core/debug.h"
#include "core/runtime.h"
#include "hal/jar_reader.h"

#include <cstdio>
#include <cstdlib>

namespace jvm
{

    // ---------------------------------------------------------------------
    // Heap
    // ---------------------------------------------------------------------

    Heap::Heap(size_t poolSize, size_t maxSize)
    {
        initCap_ = poolSize ? poolSize : kDefaultPoolSize;
        maxCap_ = (maxSize && maxSize < initCap_) ? initCap_ : maxSize;
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
        segUsed_.clear();
        capTotal_ = initCap_;
        classCache_.clear();
        internTable_.clear();
        freeList_ = nullptr;
        oom_ = false;
    }

    // (Ré)initialise l'en-tête d'un objet dont l'ADRESSE est déjà choisie -- fraîchement bump-alloué en
    // fin de segment, ou récupéré (en tout ou partie) d'un bloc libre par `allocFromFreeList`. Dans ce
    // second cas les octets peuvent porter les restes DÉTRUITS d'un ancien `std::string` (le destructeur a
    // déjà tourné pendant le balayage, cf. `collectGarbage`) : le placement-new ci-dessous reconstruit un
    // `std::string` valide par-dessus, exactement comme sur de la mémoire fraîche.
    void Heap::initObj(Obj *o, ObjKind kind, int32_t cells)
    {
        o->kind = kind;
        o->marked = false;
        o->cls = nullptr;
        o->cellCount = cells;
        o->cells = (cells > 0) ? reinterpret_cast<Value *>(o + 1) : nullptr;
        o->arrayLen = 0;
        o->freeNext = nullptr;
        ::new (static_cast<void *>(&o->str)) std::string();
        if (cells > 0)
            for (int32_t i = 0; i < cells; i++)
                o->cells[i] = Value();
    }

    // Premier ajustement : parcourt la liste des blocs libres (reconstruite à chaque `collectGarbage()`),
    // prend le premier utilisable. Un bloc ne convient QUE si la découpe laisse soit RIEN (ajustement exact),
    // soit un reliquat >= sizeof(Obj) (assez grand pour former lui-même un en-tête `Obj` valide) : céder un
    // reliquat plus petit "en plus" à l'objet alloué romprait l'invariant taille-déclarée == taille-occupée
    // (`sizeof(Obj) + cellCount*sizeof(Value)`) dont dépend le parcours du tas par en-têtes (GC) -- l'objet
    // se retrouverait alors plus PETIT que l'espace réellement occupé, et le prochain parcours retomberait au
    // milieu de son reliquat non comptabilisé au lieu du prochain en-tête réel (repéré avec ASAN : lecture
    // d'un `std::string` composé d'octets de bourrage -> SIGSEGV dans le hachage de la table d'internement).
    // Un bloc dont le reliquat serait trop petit est donc simplement IGNORÉ (recherche du suivant) : il reste
    // inchangé dans la liste, toujours valide, juste temporairement inutilisable (fragmentation interne
    // bénigne -- un futur GC peut le fusionner avec un voisin mort).
    Obj *Heap::allocFromFreeList(size_t need)
    {
        Obj *prev = nullptr;
        for (Obj *f = freeList_; f; prev = f, f = f->freeNext)
        {
            size_t avail = static_cast<size_t>(f->arrayLen);
            size_t remainder = (avail >= need) ? avail - need : 0;
            if (avail < need || (remainder != 0 && remainder < sizeof(Obj)))
                continue;
            if (getenv("JME_GC_DEBUG"))
            {
                bool ok = false;
                for (size_t si = 0; si < segs_.size() && !ok; si++)
                    if (reinterpret_cast<uint8_t *>(f) >= segs_[si] && reinterpret_cast<uint8_t *>(f) + avail <= segs_[si] + segCaps_[si])
                        ok = true;
                if (!ok)
                {
                    fprintf(stderr, "[gc] BLOC LIBRE HORS SEGMENT: f=%p avail=%zu need=%zu\n", (void *)f, avail, need);
                    abort();
                }
            }
            Obj *replacement = nullptr;
            if (remainder > 0)
            {
                replacement = reinterpret_cast<Obj *>(reinterpret_cast<uint8_t *>(f) + need);
                replacement->kind = ObjKind::Free;
                replacement->arrayLen = static_cast<int32_t>(remainder);
                replacement->freeNext = f->freeNext;
            }
            if (prev)
                prev->freeNext = replacement ? replacement : f->freeNext;
            else
                freeList_ = replacement ? replacement : f->freeNext;
            usedTotal_ += need;
            return f;
        }
        return nullptr;
    }

    Obj *Heap::allocObj(ObjKind kind, int32_t cells)
    {
        cells = (cells > 0) ? cells : 0;
        size_t need = sizeof(Obj) + cells * sizeof(Value);
        // JME_GC_STRESS=1 : force un cycle GC à CHAQUE allocation (au lieu de seulement quand le tas est
        // sous pression) -- fait apparaître en quelques trames un marquage incomplet (racine oubliée) qui,
        // avec un déclenchement normal (rare), ne se manifesterait qu'après un long moment de jeu.
        static const bool stress = []() { const char *e = getenv("JME_GC_STRESS"); return e && atoi(e) != 0; }();
        if (stress && rootScanner_)
            collectGarbage();
        for (int attempt = 0; attempt < 2; attempt++)
        {
            if (Obj *o = allocFromFreeList(need))
            {
                initObj(o, kind, cells);
                return o;
            }
            size_t freeInCur = segCaps_.back() - off_;
            if (need <= freeInCur)
            {
                Obj *o = reinterpret_cast<Obj *>(segs_.back() + off_);
                off_ += need;
                usedTotal_ += need;
                initObj(o, kind, cells);
                return o;
            }
            // Ni bloc libre assez grand, ni place en fin de segment courant : un cycle GC peut suffire à
            // libérer assez d'espace sans faire grossir le tas (essentiel sous JME_HEAP_MAX -- sur un plafond
            // dur, ne PAS tenter le GC avant de déclarer OOM ferait échouer des jeux qui allouent beaucoup de
            // courte durée par trame alors que le tas réel, une fois nettoyé, tiendrait largement). Un seul
            // essai : si le GC ne libère rien d'utilisable, grossir (ou échouer) plutôt que boucler.
            if (attempt == 0 && rootScanner_)
            {
                collectGarbage();
                continue;
            }
            break;
        }
        // Auto-grow : nouveau segment assez grand (double au moins).
        // `growTo - capTotal_` peut rester < need : growTo est borne par
        // usedTotal_+need (un total incluant l'espace deja perdu en fin
        // des anciens segments), donc le soustraire de capTotal_ (qui
        // inclut ce meme espace perdu) ne garantit PAS que le nouveau
        // segment a lui seul fasse >= need. Un objet plus gros que le
        // segment ainsi sous-dimensionne ecrit alors au-dela du buffer
        // fraichement alloue -- corruption du tas silencieuse, qui ne se
        // manifeste que beaucoup plus tard sur une allocation malloc()
        // sans rapport (observe via un test unitaire : Heap(64) suivi
        // d'allocations de 8 cellules fait exploser un segment cense
        // faire 72 octets alors que need=136). Il faut borner segSize
        // par need lui-meme, pas seulement par la capacite totale visee.
        size_t growTo = std::max(capTotal_ * 2, initCap_);
        growTo = std::max(growTo, usedTotal_ + need);
        size_t segSize = growTo - capTotal_;
        if (segSize < need)
            segSize = need;
        if (maxCap_)
        {
            // Plafond dur : on rogne le segment sur la marge restante ; s'il ne
            // peut plus contenir l'objet -> OOM (jamais d'écriture hors segment).
            size_t room = maxCap_ > capTotal_ ? maxCap_ - capTotal_ : 0;
            if (segSize > room)
                segSize = room;
            if (segSize < need)
            {
                if (jvm::jmeDebug())
                    fprintf(stderr, "allocObj OOM (JME_HEAP_MAX): need=%zu used=%zu cap=%zu max=%zu\n",
                            need, usedTotal_, capTotal_, maxCap_);
                oom_ = true;
                return nullptr;
            }
        }
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
            if (jvm::jmeDebug())
                fprintf(stderr, "allocObj OOM: kind=%d cells=%d need=%zu used=%zu cap=%zu\n",
                        (int)kind, cells, need, usedTotal_, capTotal_);
            oom_ = true;
            return nullptr;
        }
        segUsed_.push_back(off_); // fige la frontière de remplissage du segment qu'on retire du service
        segs_.push_back(seg);
        segCaps_.push_back(segSize);
        capTotal_ += segSize;
        off_ = 0;
        Obj *o = reinterpret_cast<Obj *>(segs_.back() + off_);
        off_ += need;
        usedTotal_ += need;
        initObj(o, kind, cells);
        return o;
    }

    // Parcourt UN segment par en-têtes consécutifs (objets vivants + blocs Free), de 0 à `used` -- valable
    // aussi bien en phase de marquage (construire l'ensemble des adresses vivantes) qu'en phase de balayage
    // (reconstruire la liste des blocs libres). `visit` reçoit chaque `Obj*` et sa taille totale en octets.
    template <typename Fn>
    static void walkSegment(uint8_t *base, size_t used, Fn &&visit)
    {
        size_t p = 0;
        while (p < used)
        {
            Obj *o = reinterpret_cast<Obj *>(base + p);
            size_t sz = (o->kind == ObjKind::Free) ? static_cast<size_t>(o->arrayLen)
                                                    : sizeof(Obj) + static_cast<size_t>(o->cellCount) * sizeof(Value);
            if (sz == 0) // ne devrait jamais arriver (un Obj vivant fait au moins sizeof(Obj)) -- filet anti-boucle infinie
                break;
            visit(o, sz);
            p += sz;
        }
    }

    void Heap::Marker::scan(const void *base, size_t bytes)
    {
        const uint64_t *w = reinterpret_cast<const uint64_t *>(base);
        size_t n = bytes / sizeof(uint64_t);
        for (size_t i = 0; i < n; i++)
        {
            Obj *cand = reinterpret_cast<Obj *>(static_cast<uintptr_t>(w[i]));
            if (cand)
                markObj(cand);
        }
    }

    void Heap::Marker::markObj(Obj *o)
    {
        // ORDRE CRITIQUE : `o` est un mot CANDIDAT (potentiellement n'importe quel entier/flottant qui
        // ressemble à une adresse par coïncidence) -- on vérifie son appartenance à l'ensemble des adresses
        // RÉELLEMENT allouées AVANT de le déréférencer. Le dérérencer en premier (ex. lire `o->marked`)
        // planterait sur un candidat invalide (observé : `Value::fromInt(42)` scanné conservativement
        // interprété comme `Obj* 0x2a` -> SIGSEGV).
        if (!o)
            return;
        if (heap_->liveSet_.find(o) == heap_->liveSet_.end())
            return; // ne correspond à aucun objet réellement alloué -- ignoré (marquage conservateur)
        if (o->marked || o->kind == ObjKind::Free)
            return;
        o->marked = true;
        worklist_->push_back(o);
    }

    void Heap::collectGarbage()
    {
        if (!rootScanner_)
            return; // pas de racines connues (ex. tests unitaires) : un balayage à l'aveugle libèrerait tout
        gcCount_++;

        // 1) Reconstruit l'ensemble des adresses d'objets VIVANTS actuels (parcours par en-têtes) et remet
        //    leur bit de marquage à zéro (celui du cycle précédent est périmé).
        liveSet_.clear();
        bool gcDebugMark = getenv("JME_GC_DEBUG") != nullptr;
        for (size_t si = 0; si < segs_.size(); si++)
        {
            size_t used = (si + 1 == segs_.size()) ? off_ : segUsed_[si];
            uint8_t *base = segs_[si];
            size_t segCap = segCaps_[si];
            walkSegment(base, used, [&](Obj *o, size_t sz) {
                if (gcDebugMark && (reinterpret_cast<uint8_t *>(o) < base || reinterpret_cast<uint8_t *>(o) + sz > base + segCap))
                {
                    fprintf(stderr, "[gc] OBJET HORS SEGMENT au marquage: o=%p sz=%zu base=%p segCap=%zu used=%zu si=%zu\n",
                            (void *)o, sz, (void *)base, segCap, used, si);
                    abort();
                }
                if (o->kind != ObjKind::Free)
                {
                    o->marked = false;
                    liveSet_.insert(o);
                }
            });
        }

        // 2) Marquage : racines internes au tas (chaînes internées, cache de Class) puis externes
        //    (`rootScanner_` : piles/locales Java, `statics`, caches natifs...), fermeture transitive via
        //    les cellules de chaque objet marqué.
        std::vector<Obj *> worklist;
        Marker marker(this, &worklist);
        for (auto &kv : internTable_)
            marker.markObj(kv.second);
        for (auto &kv : classCache_)
            marker.markObj(kv.second);
        rootScanner_(marker);
        while (!worklist.empty())
        {
            Obj *o = worklist.back();
            worklist.pop_back();
            if (o->cellCount > 0)
                marker.scan(o->cells, static_cast<size_t>(o->cellCount) * sizeof(Value));
        }

        // 3) Balayage : reconstruit la liste des blocs libres en fusionnant les blocs morts consécutifs
        //    (y compris d'anciens blocs déjà libres non réutilisés) ; détruit le `std::string` de chaque
        //    String morte AVANT de rendre ses octets réutilisables (son tampon, s'il existe, vit dans le tas
        //    C++ ordinaire -- pas dans nos segments -- et fuirait sinon indéfiniment).
        freeList_ = nullptr;
        size_t reclaimed = 0;
        bool gcDebug = getenv("JME_GC_DEBUG") != nullptr;
        for (size_t si = 0; si < segs_.size(); si++)
        {
            size_t used = (si + 1 == segs_.size()) ? off_ : segUsed_[si];
            uint8_t *base = segs_[si];
            size_t segCap = segCaps_[si];
            Obj *pendingFree = nullptr; // bloc libre en cours de fusion, à la fin du segment parcouru jusqu'ici
            walkSegment(base, used, [&](Obj *o, size_t sz) {
                if (gcDebug && (reinterpret_cast<uint8_t *>(o) < base || reinterpret_cast<uint8_t *>(o) + sz > base + segCap))
                {
                    fprintf(stderr, "[gc] OBJET HORS SEGMENT au balayage: o=%p sz=%zu base=%p segCap=%zu used=%zu si=%zu\n",
                            (void *)o, sz, (void *)base, segCap, used, si);
                    abort();
                }
                bool dead = (o->kind != ObjKind::Free) && !o->marked;
                if (dead)
                {
                    if (o->kind == ObjKind::String)
                    {
                        auto it = internTable_.find(o->str);
                        if (it != internTable_.end() && it->second == o)
                            internTable_.erase(it);
                    }
                    else if (o->kind == ObjKind::Class)
                    {
                        for (auto it = classCache_.begin(); it != classCache_.end(); ++it)
                            if (it->second == o) { classCache_.erase(it); break; }
                    }
                    o->str.~basic_string();
                    reclaimed += sz;
                }
                if (dead || o->kind == ObjKind::Free)
                {
                    if (pendingFree && reinterpret_cast<uint8_t *>(pendingFree) + static_cast<size_t>(pendingFree->arrayLen) == reinterpret_cast<uint8_t *>(o))
                        pendingFree->arrayLen += static_cast<int32_t>(sz); // fusionne avec le bloc libre précédent (adjacent)
                    else
                    {
                        pendingFree = o;
                        pendingFree->kind = ObjKind::Free;
                        pendingFree->arrayLen = static_cast<int32_t>(sz);
                        pendingFree->freeNext = freeList_;
                        freeList_ = pendingFree;
                    }
                }
                else
                    pendingFree = nullptr; // objet vivant : rompt la contiguïté, plus rien à fusionner ici
            });
        }
        usedTotal_ -= reclaimed;
        lastReclaimed_ = reclaimed;
        if (jvm::jmeDebug())
            fprintf(stderr, "[gc] cycle %zu : %zu octets recuperes, used=%zu/%zu\n", gcCount_, reclaimed, usedTotal_, capTotal_);
    }

    Obj *Heap::newString(const std::string &s)
    {
        Obj *o = allocObj(ObjKind::String, 0);
        if (!o)
            return nullptr;
        o->str = s;
        return o;
    }

    Obj *Heap::internString(const std::string &s)
    {
        auto it = internTable_.find(s);
        if (it != internTable_.end())
            return it->second;
        Obj *o = newString(s);
        if (o)
            internTable_.emplace(s, o);
        return o;
    }

    Obj *Heap::newStringCat(Obj *a, Obj *b)
    {
        if (!a)
            a = newString("");
        if (!b)
            b = newString("");
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

        static uint8_t buf[1024 * 1024];
        size_t n = jar_->extractClass(internalName, buf, sizeof(buf));
        if (n == 0)
        {
            if (jvm::jmeDebug())
                fprintf(stderr, "loadFromJar: extractClass a echoue pour '%s'\n", internalName.c_str());
            return nullptr;
        }

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