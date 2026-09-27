#pragma once

#include "core/runtime.h"
#include <functional>

namespace jvm
{

    class Interpreter;

    // Contexte fourni à une méthode native.
    // args[0..nargs-1] : arguments (args[0] = récepteur si méthode d'instance).
    struct NativeContext
    {
        Runtime *rt = nullptr;
        Interpreter *interp = nullptr;
        Value *args = nullptr;
        int nargs = 0;
        Obj *thisObj = nullptr;
        Value *result = nullptr; // à écrire si la méthode est non-void
        Obj *exception = nullptr; // renseigné par throwJava() : l'appel échoue et l'exception se propage (rattrapable par un `catch`)
    };

    using NativeFn = std::function<void(NativeContext *)>;

    // Lève une exception Java depuis une native (`java/io/EOFException`, `java/lang/NumberFormatException`...).
    // No-op silencieux si la classe n'est pas enregistrée. À appeler puis `return` (le résultat est ignoré).
    void throwJava(NativeContext *ctx, const char *className);

    // Table des natives : clé "nomClasse.nomMethode:desc"
    NativeFn findNative(const std::string &key);
    const NativeFn *findNativePtr(const std::string &key); // pointeur stable (nœud de la table)
    void registerNative(const std::string &key, NativeFn fn);

    // Dossier de persistance du RecordStore ("" = en mémoire seulement).
    void setRmsDir(const std::string &dir);

    // Enregistrement de toutes les natives (CLDC + MIDP). À appeler une fois.
    void initNatives();

    // Horloge du jeu : avancée à chaque trame de la durée réelle écoulée (ou d'un pas fixe, JME_FRAME_TIME).
    int64_t virtualMillis();
    int64_t virtualMicros();
    void setVirtualMicros(int64_t us); // monotone : ignoré si en arrière
    void advanceVirtualMillis(int64_t ms);

    // GC : sommet de la pile C++ de la fibre ACTIVE, s'il y en a une (voir le commentaire complet dans
    // natives.cpp, à côté de sa définition). Utilisée par src/app/main.cpp pour savoir QUELLE pile scanner
    // conservativement (celle d'une fibre en cours, sinon celle du fil principal).
    bool jme_currentFiberStackTop(uint8_t *&top);

} // namespace jvm