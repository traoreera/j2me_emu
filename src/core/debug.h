#pragma once
#include <cstdlib>

namespace jvm
{
    // Flags de debug lus UNE seule fois. getenv() est un scan linéaire de
    // environ ; l'appeler dans un chemin chaud (drawImage, invoke, tick...)
    // coûte cher à l'échelle de millions d'appels/seconde.
    inline bool jmeDebug()
    {
        static const bool v = std::getenv("JME_DEBUG") != nullptr;
        return v;
    }
    inline bool drawDbg()
    {
        static const bool v = std::getenv("JME_DRAWDBG") != nullptr;
        return v;
    }
    inline bool pixDbg()
    {
        static const bool v = std::getenv("JME_PIXDBG") != nullptr;
        return v;
    }
} // namespace jvm
