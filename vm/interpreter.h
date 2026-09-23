#pragma once

#include "runtime.h"
#include <functional>

namespace jvm
{

    // Frame d'exécution
    struct Frame
    {
        ClassInfo *cls = nullptr;
        const MethodRecord *method = nullptr;
        Obj *thisObj = nullptr;
        Value *locals = nullptr; // maxLocals cellules (slots; long/double = 2 slots)
        int localsCount = 0;
        Value *stack = nullptr;      // maxStack cellules
        uint8_t *stackCat = nullptr; // catégorie (1 ou 2) par cellule de pile
        int stackCap = 0;
        int sp = 0; // nombre de cellules de pile
        int pc = 0;
    };

    class Interpreter
    {
    public:
        explicit Interpreter(Runtime *rt);
        ~Interpreter();

        // Appel de méthode runtime. args = nargs cellules (le récepteur est
        // args[0] pour une méthode d'instance). result rempli si non-void.
        bool invoke(ClassInfo *cls, const MethodRecord *m, Obj *thisObj,
                    Value *args, int nargs, Value &result);

        // Appel par nom (résolution dynamique).
        bool invokeStatic(ClassInfo *declClass, const std::string &name, const std::string &desc,
                          int nargs, Value *args, Value &result);
        bool invokeSpecial(ClassInfo *declClass, const std::string &name, const std::string &desc,
                           Obj *thisObj, Value *args, int nargs, Value &result);
        bool invokeVirtual(ClassInfo *declClass, const std::string &name, const std::string &desc,
                           Obj *thisObj, Value *args, int nargs, Value &result);

        // <clinit> de la classe si pas encore fait
        bool ensureInit(ClassInfo *cls);

        Runtime *rt() { return rt_; }

        // Budget d'instructions par « trame » coopérative : à -1, illimité.
        // Quand le budget atteint 0 :
        //  - si une yieldFn est active (cf. setYieldFn, thread géré par fibre
        //    dans natives.cpp), elle est appelée -- elle suspend la fibre
        //    courante (swapcontext) jusqu'à la prochaine trame, PUIS l'exécution
        //    REPREND exactement là où elle s'était arrêtée (même pc, mêmes
        //    locales, même pile d'appel C++) avec un budget rafraîchi. C'est le
        //    cas normal pour Thread.run().
        //  - sinon (pas de fibre, ex. startApp()/paint() appelés directement),
        //    ancien comportement : execBytecode abandonne (okResult=false).
        void setInstrBudget(int64_t n)
        {
            instrBudget_ = n;
            instrBudgetQuota_ = n;
        }
        int64_t instrBudgetLeft() const { return instrBudget_; }

        // Fonction appelée quand le budget s'épuise pendant l'exécution d'une
        // fibre : doit suspendre la fibre courante (swapcontext) et ne revenir
        // que lorsqu'elle est réveillée par le scheduler. nullptr = pas de
        // fibre active (comportement budget classique : abandon).
        using YieldFn = std::function<void()>;
        void setYieldFn(YieldFn fn) { yieldFn_ = std::move(fn); }
        void clearYieldFn() { yieldFn_ = nullptr; }

        // Alloue n octets depuis l'arène de frames (ring bump), aligné sur 8.
        void *frameAlloc(size_t n);
        void frameFree(size_t mark);

    private:
        Runtime *rt_;
        uint8_t *arena_;
        size_t arenaSize_;
        size_t arenaOff_ = 0;
        size_t arenaBase_ = 0;
        int64_t instrBudget_ = -1;
        int64_t instrBudgetQuota_ = -1;
        YieldFn yieldFn_;
        // Exception Java en cours de remontée (athrow non rattrapé dans sa
        // propre frame) -- consultée par le site d'appel juste après le
        // retour d'un invoke*, pour tenter un rattrapage dans SA propre
        // table d'exceptions avant de continuer à se propager. nullptr en
        // dehors d'une fenêtre de déroulement de pile.
        Obj *pendingException_ = nullptr;

        bool dispatch(ClassInfo *cls, const MethodRecord *m, Obj *thisObj,
                      Value *args, int nargs, Value &result);
        bool execBytecode(ClassInfo *cls, const MethodRecord *m, Obj *thisObj,
                          Value *args, int nargs, Value &result);
    };

} // namespace jvm