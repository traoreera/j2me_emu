// test_main.cpp
// Point d'entrée du binaire de tests unitaires (j2me_tests). Exécute tous les
// TEST(...) enregistrés par les autres fichiers tests/test_*.cpp et rend un
// code de sortie non nul si au moins un test échoue (utilisable en CI).

#include "framework.h"

int main()
{
    int passed = 0, failed = 0;
    for (const auto &tc : jtest::registry())
    {
        try
        {
            tc.fn();
            std::printf("[PASS] %s\n", tc.name.c_str());
            passed++;
        }
        catch (const jtest::AssertionFailure &e)
        {
            std::printf("[FAIL] %s: %s\n", tc.name.c_str(), e.message.c_str());
            failed++;
        }
        catch (const std::exception &e)
        {
            std::printf("[FAIL] %s: exception non geree: %s\n", tc.name.c_str(), e.what());
            failed++;
        }
        catch (...)
        {
            std::printf("[FAIL] %s: exception inconnue\n", tc.name.c_str());
            failed++;
        }
    }
    std::printf("\n%d/%d tests passes", passed, passed + failed);
    if (failed)
        std::printf(" (%d ECHECS)\n", failed);
    else
        std::printf("\n");
    return failed == 0 ? 0 : 1;
}
