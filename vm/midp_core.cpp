// midp_core.cpp -- état global, accès aux args, helpers communs (regClass/regN)
// Découpé de l'ancien midp_natives.cpp ; état partagé : midp_internal.h.

#include "midp_internal.h"

namespace jvm
{
    namespace midp
    {
        namespace detail
        {


            Runtime *g_rt = nullptr;
            Interpreter *g_interp = nullptr;

            Obj *g_midlet = nullptr;
            Obj *g_display = nullptr;
            Obj *g_current = nullptr;
            bool g_paintRequested = false;
            bool g_destroyed = false;

            std::vector<std::pair<std::string, std::string>> g_appProps;

            Obj *g_screenGfx = nullptr;
            Obj *g_fontCache[4][3] = {};

            // Buffer RGB565 hors-heap pour les GameCanvas, dimensionné à la résolution
            // écran effective (cf. init(), appelé après hal::display_init). Allocation
            // unique au démarrage -- pas de (re)allocation en cours de partie.
            uint16_t *g_canvas565 = nullptr;

            uint32_t g_keyStates = 0; // masque MIDP getKeyStates()
            int g_tickN = 0;
            int jme_tickCount() { return g_tickN; }

            Obj *argRef(NativeContext *ctx, int i)
            {
                return (i >= 0 && i < ctx->nargs) ? ctx->args[i].o : nullptr;
            }

            int32_t argInt(NativeContext *ctx, int i)
            {
                return (i >= 0 && i < ctx->nargs) ? ctx->args[i].i : 0;
            }

            void setInt(NativeContext *ctx, int32_t v)
            {
                if (ctx->result)
                    *ctx->result = Value::fromInt(v);
            }

            void setLong(NativeContext *ctx, int64_t v)
            {
                if (ctx->result)
                    *ctx->result = Value::fromLong(v);
            }

            void setRef(NativeContext *ctx, Obj *o)
            {
                if (ctx->result)
                    *ctx->result = Value::fromRef(o);
            }
            // Lit le contenu actuel d'un java.lang.StringBuffer : son champ "str"
            // (cells[0], cf. regClass "java/lang/StringBuffer" plus bas) référence
            // l'Obj String courant.
            std::string sbCurrentStr(Obj *sb)
            {
                if (!sb || sb->kind != ObjKind::Instance || sb->cellCount < 1)
                    return "";
                Obj *s = sb->cells[0].o;
                return (s && s->kind == ObjKind::String) ? s->str : "";
            }
            Obj *makeInstance(const char *internalName)
            {
                ClassInfo *c = g_rt ? g_rt->lookupNativeClass(internalName) : nullptr;
                return c ? g_rt->heap().newInstance(c) : nullptr;
            }

            bool isSubclassOf(Obj *o, const char *internalName)
            {
                if (!o || o->kind != ObjKind::Instance)
                    return false;
                ClassInfo *c = o->cls;
                while (c)
                {
                    if (c->name == internalName)
                        return true;
                    c = c->super;
                }
                return false;
            }

            uint16_t argb565(uint32_t c)
            {
                return static_cast<uint16_t>((((c >> 16) & 0xFF) >> 3) << 11) |
                       static_cast<uint16_t>((((c >> 8) & 0xFF) >> 2) << 5) |
                       static_cast<uint16_t>((c & 0xFF) >> 3);
            }

            int64_t argLongL(NativeContext *ctx, int i)
            {
                return (i >= 0 && i < ctx->nargs) ? ctx->args[i].l : 0;
            }

            // ---------------------------------------------------------------------
            // Enregistrement des classes natives
            // ---------------------------------------------------------------------

            struct N
            {
                const char *name;
                const char *desc;
                NativeFn fn;
            };

            void regClass(Runtime *rt, const char *name, const char *super,
                          std::initializer_list<std::pair<const char *, const char *>> methods,
                          std::initializer_list<std::pair<const char *, const char *>> fields)
            {
                std::vector<std::pair<std::string, std::string>> m;
                for (auto &kv : methods)
                    m.push_back({kv.first, kv.second});
                std::vector<std::pair<std::string, std::string>> f;
                for (auto &kv : fields)
                    f.push_back({kv.first, kv.second});
                rt->registerNativeClass(name, super ? super : "", m, f);
            }

            void regN(const char *key, NativeFn fn)
            {
                registerNative(key, fn);
            }
            void registerCoreNatives()
            {
            }
        } // namespace detail
    } // namespace midp
} // namespace jvm
