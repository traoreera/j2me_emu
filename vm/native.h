#pragma once

#include "runtime.h"
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
};

using NativeFn = std::function<void(NativeContext *)>;

// Table des natives : clé "nomClasse.nomMethode:desc"
NativeFn findNative(const std::string &key);
void registerNative(const std::string &key, NativeFn fn);

// Enregistrement de toutes les natives (CLDC + MIDP). À appeler une fois.
void initNatives();

// Horloge virtuelle (ms) simulée, avancée à chaque frame (déterministe).
int64_t virtualMillis();
void advanceVirtualMillis(int64_t ms);

} // namespace jvm