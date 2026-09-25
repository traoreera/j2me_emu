// framework.h
// Micro-framework de tests unitaires, header-only, sans dépendance externe
// (pas de gtest/catch2 : le projet vise aussi le RP2040, on garde l'outillage
// aussi léger que le reste du code). Les tests d'exécutable liés à ce header
// ne tournent que côté PC (voir tests/README.md) -- rien ici ne cible le
// RP2040 lui-même, seul le code testé (hal/vm) le fait.

#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace jtest
{

    struct TestCase
    {
        std::string name;
        std::function<void()> fn;
    };

    inline std::vector<TestCase> &registry()
    {
        static std::vector<TestCase> r;
        return r;
    }

    struct Registrar
    {
        Registrar(const char *name, std::function<void()> fn)
        {
            registry().push_back({name, std::move(fn)});
        }
    };

    struct AssertionFailure
    {
        std::string message;
    };

} // namespace jtest

#define JTEST_CONCAT_(a, b) a##b
#define JTEST_CONCAT(a, b) JTEST_CONCAT_(a, b)

#define TEST(name)                                                          \
    static void JTEST_CONCAT(jtest_fn_, name)();                            \
    static ::jtest::Registrar JTEST_CONCAT(jtest_reg_, name)(               \
        #name, JTEST_CONCAT(jtest_fn_, name));                              \
    static void JTEST_CONCAT(jtest_fn_, name)()

#define ASSERT_TRUE(cond)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                         \
        {                                                                    \
            std::ostringstream _oss;                                        \
            _oss << "ASSERT_TRUE(" #cond ") failed at " __FILE__ ":" << __LINE__; \
            throw ::jtest::AssertionFailure{_oss.str()};                     \
        }                                                                    \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b)                                                      \
    do                                                                       \
    {                                                                        \
        auto _a = (a);                                                      \
        auto _b = (b);                                                      \
        if (!(_a == _b))                                                     \
        {                                                                    \
            std::ostringstream _oss;                                        \
            _oss << "ASSERT_EQ(" #a ", " #b ") failed: " << _a << " != " << _b \
                 << " at " __FILE__ ":" << __LINE__;                        \
            throw ::jtest::AssertionFailure{_oss.str()};                     \
        }                                                                    \
    } while (0)

#define ASSERT_NE(a, b)                                                      \
    do                                                                       \
    {                                                                        \
        auto _a = (a);                                                      \
        auto _b = (b);                                                      \
        if (!(_a != _b))                                                     \
        {                                                                    \
            std::ostringstream _oss;                                        \
            _oss << "ASSERT_NE(" #a ", " #b ") failed: both equal " << _a    \
                 << " at " __FILE__ ":" << __LINE__;                        \
            throw ::jtest::AssertionFailure{_oss.str()};                     \
        }                                                                    \
    } while (0)
