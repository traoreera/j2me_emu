// midp_natives.cpp
// Native APIs MIDP/CLDC : midlet, lcdui (Display/Canvas/GameCanvas/Graphics/
// Font/Image). S'appuie sur le HAL display/input. Les classes natives sont
// enregistrées dans le Runtime ; leurs méthodes routent vers ces handlers.
//
// Rendu : les Graphics dessinent soit sur un buffer ARGB (Image), soit sur
// l'écran RGB565 (hal) pour le paint système, soit sur un buffer RGB565
// partagé par les GameCanvas (double buffering) qui est présenté par
// flushGraphics().

#include "core/debug.h"
#include "midp/midp.h"
#include "core/native.h"
#include "core/interpreter.h"
#include "hal/display.h"
#include "hal/input.h"
#include "hal/jar_reader.h"
#include "hal/png.h"
#include "core/class_file.h"
#include "kernel/kernel.h"
#include "kernel/audio/audio.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <algorithm>

#include "midp/midp_internal.h"

namespace jvm
{

    const std::vector<Obj *> &jme_threads();
    void jme_threadForget(Obj *r);
    bool jme_threadResume(Obj *r, Interpreter *interp, ClassInfo *cls);

    namespace midp
    {
        using namespace detail;

        int g_fbWrites = 0;
        int jme_screenWrites() { return g_fbWrites; }
        int g_pixdbg = 0;
        int g_screenPix = 0;
        int jme_screenPix() { return g_screenPix; }
        int g_canvasPix = 0;
        int jme_canvasPix() { return g_canvasPix; }
        int g_flushCalls = 0;
        int jme_flushCalls() { return g_flushCalls; }


        // ---------------------------------------------------------------------
        // public : init / tick
        // ---------------------------------------------------------------------

        void setAppProperty(const std::string &key, const std::string &value)
        {
            g_appProps.push_back({key, value});
        }

        bool midletDestroyed() { return g_destroyed; }
        void resetDestroyed() { g_destroyed = false; }
        Obj *currentDisplayable() { return g_current; }

        int halKeyToMidp(hal::KeyCode kc)
        {
            switch (kc)
            {
            case hal::KEY_UP:
                return -1;
            case hal::KEY_DOWN:
                return -2;
            case hal::KEY_LEFT:
                return -3;
            case hal::KEY_RIGHT:
                return -4;
            case hal::KEY_FIRE:
                return -5;
            case hal::KEY_0:
                return '0';
            case hal::KEY_1:
                return '1';
            case hal::KEY_2:
                return '2';
            case hal::KEY_3:
                return '3';
            case hal::KEY_4:
                return '4';
            case hal::KEY_5:
                return '5';
            case hal::KEY_6:
                return '6';
            case hal::KEY_7:
                return '7';
            case hal::KEY_8:
                return '8';
            case hal::KEY_9:
                return '9';
            case hal::KEY_STAR:
                return '*';
            case hal::KEY_HASH:
                return '#';
            case hal::KEY_SOFT1:
                return -6;
            case hal::KEY_SOFT2:
                return -7;
            default:
                return 0;
            }
        }

        uint32_t halKeyState(hal::KeyCode kc)
        {
            switch (kc)
            {
            case hal::KEY_UP:
                return 0x02;
            case hal::KEY_DOWN:
                return 0x40;
            case hal::KEY_LEFT:
                return 0x04;
            case hal::KEY_RIGHT:
                return 0x20;
            case hal::KEY_FIRE:
                return 0x100;
            default:
                return 0;
            }
        }

        void sendKeyEvent(Obj *target, const char *method, int keyCode)
        {
            if (!target || !g_interp)
                return;
            if (jvm::jmeDebug())
                fprintf(stderr, "KEY %s keyCode=%d target=%s\n", method, keyCode,
                        target->cls ? target->cls->name.c_str() : "?");
            Value args[2];
            args[0] = Value::fromRef(target);
            args[1] = Value::fromInt(keyCode);
            Value res;
            g_interp->invokeVirtual(target->cls, method, "(I)V", target, args, 2, res);
        }

        void updateKeyState(uint32_t held)
        {
            uint32_t st = 0;
            for (int b = 0; b < 19; b++)
            {
                if (held & (1u << b))
                    st |= halKeyState(static_cast<hal::KeyCode>(1u << b));
            }
            g_keyStates = st;
        }
        void n_String_initStringBuffer(NativeContext *ctx)
        {
            Obj *self = ctx->thisObj; // L'objet String que l'on initialise
            Obj *sb = argRef(ctx, 1); // Le StringBuffer passé en paramètre

            if (self && sb)
            {
                // On passe l'objet au type String
                self->kind = ObjKind::String;

                // sbStr(sb) extrait proprement la std::string contenue dans le StringBuffer
                self->str = sbCurrentStr(sb);
            }
        }
        void simulatePointer(int x, int y)
        {
            if (!g_interp || !g_current)
                return;
            Obj *cur = g_current;
            if (!cur->cls || !isSubclassOf(cur, "javax/microedition/lcdui/Canvas"))
                return;
            if (jvm::jmeDebug())
                fprintf(stderr, "PTR press+release (%d,%d) target=%s\n", x, y,
                        cur->cls->name.c_str());
            Value args[3];
            args[0] = Value::fromRef(cur);
            args[1] = Value::fromInt(x);
            args[2] = Value::fromInt(y);
            Value res;
            g_interp->invokeVirtual(cur->cls, "pointerPressed", "(II)V", cur, args, 3, res);
            g_interp->invokeVirtual(cur->cls, "pointerReleased", "(II)V", cur, args, 3, res);
        }

        void pointerEvent(int kind, int x, int y)
        {
            if (!g_interp || !g_current)
                return;
            Obj *cur = g_current;
            if (lcduiIsScreen(cur))
            {
                lcduiPointer(kind, x, y);
                return;
            }
            if (!cur->cls || !isSubclassOf(cur, "javax/microedition/lcdui/Canvas"))
                return;
            static const char *const names[3] = {"pointerPressed", "pointerReleased", "pointerDragged"};
            if (kind < 0 || kind > 2)
                return;
            if (jvm::jmeDebug())
                fprintf(stderr, "PTR %s (%d,%d) target=%s\n", names[kind], x, y, cur->cls->name.c_str());
            Value args[3] = {Value::fromRef(cur), Value::fromInt(x), Value::fromInt(y)};
            Value res;
            g_interp->invokeVirtual(cur->cls, names[kind], "(II)V", cur, args, 3, res);
        }

        void setTextInput(const char *ascii, int backspaces) { detail::lcduiSetTextInput(ascii, backspaces); }

        void tick(uint32_t pressedMask, uint32_t justPressedMask, uint32_t justReleasedMask)
        {
            g_tickN++;
            // Durée virtuelle d'une trame, en MILLISECONDES (défaut 16). Lue une fois.
            // Une valeur >= 1000 est presque sûrement écrite en microsecondes
            // (ex. 33333 pour "33 ms") : à 33333 ms/trame l'horloge du jeu avance de
            // 33 SECONDES par trame, ce qui casse les minuteries des MIDlets (dialogue
            // "Sound Set" d'Assassin's Creed 2 figé, touches ignorées). On la
            // convertit donc en ms avec un avertissement plutôt que de la subir.
            // Pas FIXE (ms) si JME_FRAME_TIME est posé : runs déterministes (CI, comparaisons pixel-à-pixel).
            // Sinon l'horloge du jeu suit le temps RÉEL écoulé depuis la trame précédente (bornée à 100 ms pour
            // qu'une trame très lente ou une fenêtre déplacée ne fasse pas sauter les minuteries des jeux).
            static const int fixedFrameMs = []() {
                const char *e = getenv("JME_FRAME_TIME");
                if (!e)
                    return 0;
                long v = atol(e);
                if (v >= 1000)
                {
                    fprintf(stderr, "[midp] JME_FRAME_TIME=%ld interprété en microsecondes (=%ld ms) ; la valeur est en ms.\n", v, v / 1000);
                    v /= 1000;
                }
                return v < 1 ? 1 : static_cast<int>(v);
            }();
            int64_t dtUs;
            if (fixedFrameMs > 0)
                dtUs = static_cast<int64_t>(fixedFrameMs) * 1000;
            else
            {
                static bool firstDt = true;
                static std::chrono::steady_clock::time_point lastReal;
                auto nowReal = std::chrono::steady_clock::now();
                if (firstDt)
                {
                    firstDt = false;
                    dtUs = 16000;
                }
                else
                    dtUs = std::chrono::duration_cast<std::chrono::microseconds>(nowReal - lastReal).count();
                lastReal = nowReal;
                dtUs = std::max<int64_t>(1000, std::min<int64_t>(100000, dtUs));
            }
            const int64_t tickStartUs = virtualMicros();
            const int64_t tickEndUs = tickStartUs + dtUs;
            if (getenv("JME_VTRACE") && g_tickN % 10 == 0)
                fprintf(stderr, "VTRACE frame=%d vms=%lld dt=%lldus\n", g_tickN, (long long)(tickEndUs / 1000), (long long)dtUs);
            if (!g_rt || !g_interp)
            {
                setVirtualMicros(tickEndUs);
                return;
            }
            updateKeyState(pressedMask);
            // Pas encore de Displayable courant : on continue quand même (threads, minuteries) --
            // beaucoup de MIDlets (Super FX-BALL, Sperm Race...) démarrent un thread dans startApp()
            // qui appelle lui-même Display.setCurrent(). Rendre la main ici les figeait à jamais.
            Obj *cur = g_current;

            // Premier paint() : livré AVANT que les threads du jeu ne progressent,
            // comme sur un vrai appareil où Display.setCurrent() déclenche un paint
            // immédiat, bien avant que le thread de chargement ait avancé. On
            // faisait tourner les fibres d'abord : elles avaient déjà attaqué
            // leur chargement (~200k instructions, état de jeu déjà passé à
            // "chargement" mais ses tableaux pas encore alloués) quand ce premier
            // paint arrivait -> NPE dans paint() de Gangstar Rio. Or ce jeu se
            // protège avec un drapeau (`cd = true` en entrée de paint, remis à false
            // en sortie) : l'exception laissait le drapeau bloqué et TOUS les paint()
            // suivants sortaient immédiatement -- écran figé sur le premier dessin
            // pour toujours, alors que le jeu chargeait et tournait normalement.
            static bool initialPaintDone = false;
            if (!initialPaintDone && g_paintRequested && cur && cur->kind == ObjKind::Instance &&
                isSubclassOf(cur, "javax/microedition/lcdui/Canvas"))
            {
                initialPaintDone = true;
                if (jvm::jmeDebug())
                    fprintf(stderr, "TICK paint initial sur %s\n", cur->cls ? cur->cls->name.c_str() : "?");
                Value pargs[2] = {Value::fromRef(cur), Value::fromRef(screenGraphics())};
                Value pres;
                const MethodRecord *pm = cur->cls->findMethodVirtual("paint", "(Ljavax/microedition/lcdui/Graphics;)V");
                if (pm && pm->mi)
                    g_interp->invokeVirtual(cur->cls, "paint", "(Ljavax/microedition/lcdui/Graphics;)V", cur, pargs, 2, pres);
                else if (isSubclassOf(cur, "javax/microedition/lcdui/game/GameCanvas"))
                    presentGameCanvasBuffer(cur);
                g_paintRequested = false;
                hal::display_present(hal::display_get_framebuffer());
            }

            // Chaque thread tourne dans sa propre fibre (ucontext, cf. jme_threadResume
            // dans natives.cpp) : Thread.sleep()/yield(), ou l'épuisement du budget
            // d'instructions ci-dessous, suspend RÉELLEMENT son exécution (pc,
            // locales, pile d'appel C++ intacts) et la reprend exactement là à la
            // trame suivante -- au lieu de relancer run() depuis le début à chaque
            // trame (ce qui empêchait toute progression pour les jeux dont la
            // boucle principale dépasse le budget par trame, ex. limiteurs de FPS
            // en bytecode qui font des dizaines de milliers d'itérations entre deux
            // Thread.sleep()). Le budget ci-dessous n'est donc plus qu'un
            // garde-fou anti-boucle-infinie-sans-yield, pas la seule source de
            // suspension.
            // Budget d'instructions par thread et par trame. Fixe à 200 000 il
            // bridait tout calcul lourd à ~1-2 ms de CPU par trame : un MIDlet qui
            // charge ses niveaux dans son propre thread (Gangstar Rio : décodage de
            // ressources, des dizaines de millions d'instructions) mettait ~150+
            // trames à démarrer alors que la trame en offrait 33 ms. Le budget est
            // donc ADAPTATIF : on mesure la vitesse réelle de l'interpréteur
            // (instructions/ms, moyenne glissante, seulement quand le thread a
            // consommé tout son budget = calcul pur) et on vise ~16 ms de CPU de
            // threads par trame (partagés entre les threads). Les threads qui
            // dorment/yieldent normalement rendent la main bien avant.
            // JME_INSTR_BUDGET=n force un budget FIXE (runs déterministes,
            // comparaisons pixel-à-pixel entre deux builds).
            static const int64_t fixedBudget = []() {
                const char *e = getenv("JME_INSTR_BUDGET");
                return e ? atoll(e) : 0;
            }();
            static double instrPerMs = 20000.0; // estimation initiale prudente
            static int64_t budget = 400000;
            // Copie : un thread qui en démarre un autre (Thread.start() dans run()/showNotify()) fait un push_back
            // sur la liste globale pendant l'itération -> itérateurs invalidés (plantage). Le nouveau thread
            // tourne dès la trame suivante.
            const std::vector<Obj *> threads = jme_threads();
            std::vector<Obj *> done;
            const double targetMs = 16.0 / (threads.empty() ? 1 : threads.size());
            // Ordonnancement par événements sur [tickStartUs, tickEndUs] : un thread endormi par sleep(ms) est
            // repris à SON instant de réveil s'il tombe dans la trame (cf. jme_schedNext).
            jme_schedBegin(g_tickN, tickEndUs);
            while (Obj *r = jme_schedNext(threads))
            {
                const MethodRecord *rm = r->cls->findMethodVirtual("run", "()V");
                if (jvm::jmeDebug() && !rm)
                    fprintf(stderr, "run()V introuvable sur %s (runnable=%p)\n", r->cls->name.c_str(), (void *)r);
                int64_t b = fixedBudget > 0 ? fixedBudget : budget;
                g_interp->setInstrBudget(b);
                auto t0 = std::chrono::steady_clock::now();
                bool finished = jme_threadResume(r, g_interp, r->cls);
                auto t1 = std::chrono::steady_clock::now();
                int64_t left = g_interp->instrBudgetLeft();
                g_interp->setInstrBudget(-1);
                if (!fixedBudget && left <= 100)
                {
                    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                    if (ms > 0.2)
                    {
                        instrPerMs = 0.7 * instrPerMs + 0.3 * (static_cast<double>(b) / ms);
                        double nb = instrPerMs * targetMs;
                        budget = static_cast<int64_t>(nb < 200000 ? 200000 : (nb > 40000000 ? 40000000 : nb));
                    }
                }
                if (jvm::jmeDebug() && jme_tickCount())
                    fprintf(stderr, "run_frame=%d fmt=%zu finished=%d\n",
                            jme_tickCount(), threads.size(), finished ? 1 : 0);
                if (finished)
                    done.push_back(r);
            }
            for (Obj *r : done)
                jme_threadForget(r);
            setVirtualMicros(tickEndUs); // la suite de la trame (minuteries, touches, paint) voit la fin de trame

            fireTimers();
            mediaFlush();

            // g_current peut avoir changé pendant l'exécution des fibres
            // (setCurrent depuis la boucle de jeu) : on re-lit après le run.
            cur = g_current;
            if (!cur || cur->kind != ObjKind::Instance)
                return;

            // Garde-fou : callbacks synchrones (touches, paint) sans fibre -> une boucle infinie dans le
            // bytecode du jeu figeait toute la fenêtre. 200 M d'instructions par trame, puis l'appel est abandonné.
            struct SyncBudget
            {
                explicit SyncBudget() { g_interp->setInstrBudget(200000000LL); }
                ~SyncBudget() { g_interp->setInstrBudget(-1); }
            } syncBudget;
            bool isCanvas = isSubclassOf(cur, "javax/microedition/lcdui/Canvas");
            bool isFullCanvas = isSubclassOf(cur, "com/nokia/mid/ui/FullCanvas");
            // NOTE : GameCanvas hérite de Canvas et NE supprime PAS le mécanisme
            // keyPressed/keyReleased/paint() standard -- getKeyStates() (polling)
            // et flushGraphics() (dessin direct dans le buffer) ne sont que des
            // API SUPPLÉMENTAIRES offertes par GameCanvas, pas un remplacement
            // obligatoire. Exclure GameCanvas de ces deux chemins (comme le
            // faisait l'ancien code, `&& !isGameCanvas` sur les deux blocs
            // ci-dessous) suppose à tort que tout jeu GameCanvas utilise
            // exclusivement le polling/flushGraphics -- faux dès qu'un jeu
            // choisit quand même de surcharger paint() sur son GameCanvas (cas
            // parfaitement légal en MIDP réel). Observé sur
            // games/mortalkomb_9moadjwj.jar : sa classe `cnv extends
            // GameCanvas` surcharge bien `paint(Graphics)` (confirmé via
            // javap) et sa boucle de rendu appelle `repaint()` -- mais
            // l'ancienne exclusion empêchait `paint()` d'être jamais invoqué,
            // donnant un écran perpétuellement noir malgré un thread de rendu
            // qui tournait sans erreur à chaque frame (0 écriture framebuffer,
            // 0 appel flushGraphics -- le jeu n'utilisait PAS le chemin
            // getGraphics()/flushGraphics() du tout, seulement paint()).
            // Commands d'un Canvas non plein écran : les touches programmables déclenchent commandAction()
            // (au lieu d'être livrées comme keyPressed) ; barre de commandes dessinée après paint().
            uint32_t softUsed = (isCanvas || isFullCanvas) ? lcduiCanvasSoftKeys(cur, justPressedMask) : 0;
            justPressedMask &= ~softUsed;
            justReleasedMask &= ~softUsed;
            if (cur != g_current) // une commande a changé l'écran courant
                return;
            if (isCanvas || isFullCanvas)
            {
                for (int b = 0; b < 19; b++)
                {
                    if (justPressedMask & (1u << b))
                        sendKeyEvent(cur, "keyPressed", halKeyToMidp(static_cast<hal::KeyCode>(1u << b)));
                    if (justReleasedMask & (1u << b))
                        sendKeyEvent(cur, "keyReleased", halKeyToMidp(static_cast<hal::KeyCode>(1u << b)));
                }
            }

            if (g_paintRequested && isCanvas)
            {
                if (jvm::jmeDebug())
                    fprintf(stderr, "TICK paint sur %s\n", cur->cls ? cur->cls->name.c_str() : "?");
                Value args[2];
                args[0] = Value::fromRef(cur);
                args[1] = Value::fromRef(screenGraphics());
                Value res;
                const MethodRecord *pm = cur->cls->findMethodVirtual("paint", "(Ljavax/microedition/lcdui/Graphics;)V");
                if (pm && pm->mi) // paint() du jeu (bytecode)
                    g_interp->invokeVirtual(cur->cls, "paint", "(Ljavax/microedition/lcdui/Graphics;)V", cur, args, 2, res);
                else if (isSubclassOf(cur, "javax/microedition/lcdui/game/GameCanvas"))
                    presentGameCanvasBuffer(cur); // paint() par défaut de GameCanvas : affiche le tampon hors écran
                g_paintRequested = false;
                lcduiCanvasOverlay(cur);
                hal::display_present(hal::display_get_framebuffer());
            }

            // --- Écrans lcdui haut niveau (List / Form / Alert / TextBox) : entrées + rendu (midp_lcdui.cpp) ---
            if (lcduiIsScreen(cur))
                lcduiTick(cur, justPressedMask, justReleasedMask);
        }

        void n_SB_appendCharArray(NativeContext *ctx)
        {
            Obj *sb = ctx->thisObj;
            Obj *charArray = argRef(ctx, 1);

            if (sb && sb->kind == ObjKind::Instance && sb->cells && charArray && charArray->kind == ObjKind::CharArray)
            {
                // 1. Récupérer la chaîne actuelle du StringBuffer (via la fonction sbStr déjà présente)
                std::string currentStr = sbCurrentStr(sb);

                // 2. Convertir le tableau de char Java en chaîne C++
                std::string appendStr;
                appendStr.reserve(static_cast<size_t>(charArray->arrayLen));
                for (int i = 0; i < charArray->arrayLen; i++)
                {
                    appendStr += static_cast<char>(charArray->cells[i].u & 0xFF);
                }

                // 3. Mettre à jour directement le champ 'str' (cells[0]) du StringBuffer avec la nouvelle String allouée
                sb->cells[0] = Value::fromRef(ctx->rt->heap().newString(currentStr + appendStr));

                // 4. Renvoyer 'this' (le StringBuffer lui-même) dans le résultat pour le chaînage d'appels append()
                if (ctx->result)
                    *ctx->result = Value::fromRef(sb);
            }
            else
            {
                if (ctx->result)
                    *ctx->result = Value::fromRef(sb);
            }
        }

        void init(Runtime *rt, Interpreter *interp)
        {
            detail::registerCoreNatives();
            detail::registerGraphicsNatives();
            detail::registerGameNatives();
            detail::registerUiNatives();
            detail::registerLcduiNatives();
            detail::registerIoNatives();
            detail::registerMediaNatives();
            g_rt = rt;
            g_interp = interp;

            delete[] g_canvas565;
            g_canvas565 = new uint16_t[static_cast<size_t>(screenW()) * static_cast<size_t>(screenH())]();

            const std::initializer_list<std::pair<const char *, const char *>> none = {};

            // --- java.lang ---
            regClass(rt, "java/lang/Object", nullptr,
                     {{"<init>", "()V"}, {"getClass", "()Ljava/lang/Class;"}, {"equals", "(Ljava/lang/Object;)Z"}, {"hashCode", "()I"}, {"toString", "()Ljava/lang/String;"}, {"wait", "()V"}, {"wait", "(I)V"}, {"wait", "(J)V"}, {"notify", "()V"}, {"notifyAll", "()V"}},
                     none);
            regClass(rt, "java/lang/String", "java/lang/Object",
                     {{"hashCode", "()I"}, {"getBytes", "()[B"}, {"getBytes", "(Ljava/lang/String;)[B"}, {"lastIndexOf", "(I)I"}, {"lastIndexOf", "(II)I"}, {"lastIndexOf", "(Ljava/lang/String;)I"}, {"replace", "(CC)Ljava/lang/String;"}, {"startsWith", "(Ljava/lang/String;I)Z"}, {"toString", "()Ljava/lang/String;"}, {"isEmpty", "()Z"}, {"compareToIgnoreCase", "(Ljava/lang/String;)I"}, {"<init>", "([C)V"}, {"valueOf", "(C)Ljava/lang/String;"}, {"valueOf", "(J)Ljava/lang/String;"}, {"valueOf", "(Z)Ljava/lang/String;"}, {"valueOf", "(F)Ljava/lang/String;"}, {"valueOf", "(D)Ljava/lang/String;"}, {"valueOf", "([C)Ljava/lang/String;"}, {"copyValueOf", "([C)Ljava/lang/String;"}, {"valueOf", "(Ljava/lang/Object;)Ljava/lang/String;"}, {"<init>", "()V"}, {"<init>", "(Ljava/lang/StringBuffer;)V"}, {"<init>", "(Ljava/lang/String;)V"}, {"<init>", "([BLjava/lang/String;)V"}, {"<init>", "([B)V"}, {"<init>", "([BII)V"}, {"<init>", "([BIILjava/lang/String;)V"}, {"<init>", "([CII)V"}, {"length", "()I"}, {"charAt", "(I)C"}, {"toCharArray", "()[C"}, {"concat", "(Ljava/lang/String;)Ljava/lang/String;"}, {"equals", "(Ljava/lang/Object;)Z"}, {"substring", "(I)Ljava/lang/String;"}, {"substring", "(II)Ljava/lang/String;"}, {"indexOf", "(Ljava/lang/String;)I"}, {"indexOf", "(Ljava/lang/String;I)I"}, {"indexOf", "(I)I"}, {"indexOf", "(II)I"}, {"trim", "()Ljava/lang/String;"}, {"toLowerCase", "()Ljava/lang/String;"}, {"toUpperCase", "()Ljava/lang/String;"}, {"compareTo", "(Ljava/lang/String;)I"}, {"startsWith", "(Ljava/lang/String;)Z"}, {"getChars", "(II[CI)V"}, {"endsWith", "(Ljava/lang/String;)Z"}, {"equalsIgnoreCase", "(Ljava/lang/String;)Z"}, {"valueOf", "(I)Ljava/lang/String;"}, {"intern", "()Ljava/lang/String;"}},
                     none);
            regClass(rt, "java/lang/Math", "java/lang/Object",
                     {{"sin", "(D)D"}, {"cos", "(D)D"}, {"tan", "(D)D"}, {"asin", "(D)D"}, {"acos", "(D)D"}, {"atan", "(D)D"}, {"atan2", "(DD)D"}, {"exp", "(D)D"}, {"log", "(D)D"}, {"toRadians", "(D)D"}, {"toDegrees", "(D)D"}, {"abs", "(D)D"}, {"abs", "(F)F"}, {"min", "(FF)F"}, {"max", "(FF)F"}, {"min", "(DD)D"}, {"max", "(DD)D"}, {"round", "(F)I"}, {"abs", "(I)I"}, {"abs", "(J)J"}, {"min", "(II)I"}, {"min", "(JJ)J"}, {"max", "(II)I"}, {"max", "(JJ)J"}, {"sqrt", "(D)D"}, {"floor", "(D)D"}, {"ceil", "(D)D"}, {"round", "(D)J"}, {"pow", "(DD)D"}, {"random", "()D"}},
                     none);
            regClass(rt, "java/lang/System", "java/lang/Object",
                     {{"currentTimeMillis", "()J"}, {"arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V"}, {"gc", "()V"}, {"identityHashCode", "(Ljava/lang/Object;)I"}, {"getProperty", "(Ljava/lang/String;)Ljava/lang/String;"}},
                     {{"out", "Ljava/io/PrintStream;"}});
            regClass(rt, "java/lang/Runtime", "java/lang/Object",
                     {{"getRuntime", "()Ljava/lang/Runtime;"}, {"freeMemory", "()J"}, {"totalMemory", "()J"}, {"maxMemory", "()J"}, {"gc", "()V"}},
                     none);
            regClass(rt, "java/io/PrintStream", "java/lang/Object",
                     {{"println", "(Ljava/lang/Object;)V"}, {"print", "(Ljava/lang/Object;)V"}, {"println", "(C)V"}, {"println", "(J)V"}, {"println", "(Z)V"}, {"print", "(C)V"}, {"print", "(J)V"}, {"print", "(Z)V"}, {"println", "([C)V"}, {"println", "(Ljava/lang/String;)V"}, {"println", "(I)V"}, {"println", "()V"}, {"print", "(Ljava/lang/String;)V"}, {"print", "(I)V"}, {"flush", "()V"}},
                     none);
            regClass(rt, "java/lang/Class", "java/lang/Object",
                     {{"getName", "()Ljava/lang/String;"}, {"newInstance", "()Ljava/lang/Object;"}, {"forName", "(Ljava/lang/String;)Ljava/lang/Class;"}, {"getResourceAsStream", "(Ljava/lang/String;)Ljava/io/InputStream;"}},
                     none);

            // --- java.lang : hiérarchie Throwable/Exception ---
            // Seul Throwable déclare <init>/getMessage/toString/
            // printStackTrace et le champ "message" (cells[0]) -- les
            // sous-classes n'ont rien à redéclarer, elles héritent via la
            // résolution virtuelle normale (cf. natives.cpp). athrow/catch
            // sont gérés par l'interpréteur (table d'exceptions du .class,
            // vm/interpreter.cpp) ; ce bloc ne fait qu'exposer les classes
            // elles-mêmes pour que `new Exception(...)`/`new
            // NullPointerException(...)` etc. résolvent.
            regClass(rt, "java/lang/Throwable", "java/lang/Object",
                     {{"<init>", "()V"}, {"<init>", "(Ljava/lang/String;)V"}, {"getMessage", "()Ljava/lang/String;"}, {"toString", "()Ljava/lang/String;"}, {"printStackTrace", "()V"}},
                     {{"message", "Ljava/lang/String;"}});
            regClass(rt, "java/lang/Exception", "java/lang/Throwable", none, none);
            regClass(rt, "java/lang/RuntimeException", "java/lang/Exception", none, none);
            regClass(rt, "java/lang/NullPointerException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/lang/IndexOutOfBoundsException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/lang/ArrayIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException", none, none);
            regClass(rt, "java/lang/ArrayStoreException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/lang/ClassCastException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/lang/IllegalArgumentException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/lang/IllegalStateException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/lang/NumberFormatException", "java/lang/IllegalArgumentException", none, none);
            regClass(rt, "java/lang/ArithmeticException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/lang/InterruptedException", "java/lang/Exception", none, none);
            regClass(rt, "java/lang/SecurityException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/io/IOException", "java/lang/Exception", none, none);
            regClass(rt, "java/io/Reader", "java/lang/Object",
                     {{"read", "()I"}, {"read", "([C)I"}, {"read", "([CII)I"}, {"ready", "()Z"}, {"skip", "(J)J"}, {"close", "()V"}}, none);
            regClass(rt, "java/io/InputStreamReader", "java/io/Reader",
                     {{"<init>", "(Ljava/io/InputStream;)V"}, {"<init>", "(Ljava/io/InputStream;Ljava/lang/String;)V"}, {"read", "()I"}, {"read", "([C)I"}, {"read", "([CII)I"}, {"ready", "()Z"}, {"skip", "(J)J"}, {"close", "()V"}},
                     {{"in", "Ljava/io/InputStream;"}, {"utf8", "I"}});
            regClass(rt, "java/lang/ClassNotFoundException", "java/lang/Exception", none, none);
            regClass(rt, "java/lang/InstantiationException", "java/lang/Exception", none, none);
            regClass(rt, "java/lang/IllegalAccessException", "java/lang/Exception", none, none);
            regClass(rt, "java/lang/Error", "java/lang/Throwable", none, none);
            regClass(rt, "java/lang/VirtualMachineError", "java/lang/Error", none, none);
            regClass(rt, "java/lang/OutOfMemoryError", "java/lang/VirtualMachineError", none, none);
            regClass(rt, "java/lang/StackOverflowError", "java/lang/VirtualMachineError", none, none);
            regClass(rt, "java/lang/NegativeArraySizeException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/lang/UnsupportedOperationException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/lang/IllegalMonitorStateException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/lang/IllegalThreadStateException", "java/lang/IllegalArgumentException", none, none);
            regClass(rt, "java/lang/StringIndexOutOfBoundsException", "java/lang/IndexOutOfBoundsException", none, none);
            regClass(rt, "java/util/NoSuchElementException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/util/EmptyStackException", "java/lang/RuntimeException", none, none);
            regClass(rt, "java/io/EOFException", "java/io/IOException", none, none);
            regClass(rt, "java/io/UnsupportedEncodingException", "java/io/IOException", none, none);
            regClass(rt, "java/io/InterruptedIOException", "java/io/IOException", none, none);
            regClass(rt, "javax/microedition/io/ConnectionNotFoundException", "java/io/IOException", none, none);
            regClass(rt, "javax/microedition/io/Connection", "java/lang/Object", none, none);
            regClass(rt, "javax/microedition/io/Connector", "java/lang/Object",
                     {{"open", "(Ljava/lang/String;)Ljavax/microedition/io/Connection;"}, {"open", "(Ljava/lang/String;I)Ljavax/microedition/io/Connection;"}, {"open", "(Ljava/lang/String;IZ)Ljavax/microedition/io/Connection;"}, {"openInputStream", "(Ljava/lang/String;)Ljava/io/InputStream;"}, {"openDataInputStream", "(Ljava/lang/String;)Ljava/io/DataInputStream;"}},
                     none);

            // --- java.lang compléments CLDC basiques ---
            regClass(rt, "java/lang/Integer", "java/lang/Object",
                     {{"parseInt", "(Ljava/lang/String;I)I"}, {"toHexString", "(I)Ljava/lang/String;"}, {"toBinaryString", "(I)Ljava/lang/String;"}, {"toOctalString", "(I)Ljava/lang/String;"}, {"toString", "(II)Ljava/lang/String;"}, {"valueOf", "(Ljava/lang/String;)Ljava/lang/Integer;"}, {"<init>", "(I)V"}, {"intValue", "()I"}, {"byteValue", "()B"}, {"shortValue", "()S"}, {"longValue", "()J"}, {"hashCode", "()I"}, {"toString", "()Ljava/lang/String;"}, {"toString", "(I)Ljava/lang/String;"}, {"equals", "(Ljava/lang/Object;)Z"}, {"compareTo", "(Ljava/lang/Integer;)I"}, {"valueOf", "(I)Ljava/lang/Integer;"}, {"parseInt", "(Ljava/lang/String;)I"}},
                     {{"value", "I"}});
            regClass(rt, "java/lang/Boolean", "java/lang/Object",
                     {{"<init>", "(Z)V"}, {"booleanValue", "()Z"}, {"toString", "()Ljava/lang/String;"}, {"hashCode", "()I"}, {"equals", "(Ljava/lang/Object;)Z"}, {"valueOf", "(Z)Ljava/lang/Boolean;"}},
                     {{"value", "Z"}, {"TRUE", "Ljava/lang/Boolean;"}, {"FALSE", "Ljava/lang/Boolean;"}});
            regClass(rt, "java/lang/Long", "java/lang/Object",
                     {{"<init>", "(J)V"}, {"longValue", "()J"}, {"intValue", "()I"}, {"toString", "()Ljava/lang/String;"}, {"toString", "(J)Ljava/lang/String;"}, {"parseLong", "(Ljava/lang/String;)J"}, {"valueOf", "(J)Ljava/lang/Long;"}, {"valueOf", "(Ljava/lang/String;)Ljava/lang/Long;"}, {"hashCode", "()I"}, {"equals", "(Ljava/lang/Object;)Z"}},
                     {{"value", "J"}});
            regClass(rt, "java/lang/Short", "java/lang/Object",
                     {{"<init>", "(S)V"}, {"shortValue", "()S"}, {"intValue", "()I"}, {"toString", "()Ljava/lang/String;"}, {"parseShort", "(Ljava/lang/String;)S"}},
                     {{"value", "I"}});
            regClass(rt, "java/lang/Byte", "java/lang/Object",
                     {{"<init>", "(B)V"}, {"byteValue", "()B"}, {"intValue", "()I"}, {"toString", "()Ljava/lang/String;"}, {"parseByte", "(Ljava/lang/String;)B"}},
                     {{"value", "I"}});
            regClass(rt, "java/lang/Character", "java/lang/Object",
                     {{"<init>", "(C)V"}, {"charValue", "()C"}, {"toString", "()Ljava/lang/String;"}, {"toString", "(C)Ljava/lang/String;"}, {"isDigit", "(C)Z"}, {"isLetter", "(C)Z"}, {"isLetterOrDigit", "(C)Z"}, {"isUpperCase", "(C)Z"}, {"isLowerCase", "(C)Z"}, {"isSpace", "(C)Z"}, {"isWhitespace", "(C)Z"}, {"toUpperCase", "(C)C"}, {"toLowerCase", "(C)C"}, {"digit", "(CI)I"}, {"forDigit", "(II)C"}, {"valueOf", "(C)Ljava/lang/Character;"}, {"hashCode", "()I"}, {"equals", "(Ljava/lang/Object;)Z"}},
                     {{"value", "I"}});
            regClass(rt, "java/lang/Float", "java/lang/Object",
                     {{"<init>", "(F)V"}, {"floatValue", "()F"}, {"doubleValue", "()D"}, {"intValue", "()I"}, {"toString", "()Ljava/lang/String;"}, {"toString", "(F)Ljava/lang/String;"}, {"parseFloat", "(Ljava/lang/String;)F"}, {"valueOf", "(Ljava/lang/String;)Ljava/lang/Float;"}, {"valueOf", "(F)Ljava/lang/Float;"}, {"isNaN", "(F)Z"}, {"isNaN", "()Z"}, {"isInfinite", "(F)Z"}, {"floatToIntBits", "(F)I"}, {"intBitsToFloat", "(I)F"}},
                     {{"value", "F"}});
            regClass(rt, "java/lang/Double", "java/lang/Object",
                     {{"<init>", "(D)V"}, {"doubleValue", "()D"}, {"floatValue", "()F"}, {"intValue", "()I"}, {"toString", "()Ljava/lang/String;"}, {"toString", "(D)Ljava/lang/String;"}, {"parseDouble", "(Ljava/lang/String;)D"}, {"valueOf", "(Ljava/lang/String;)Ljava/lang/Double;"}, {"valueOf", "(D)Ljava/lang/Double;"}, {"isNaN", "(D)Z"}, {"isNaN", "()Z"}, {"isInfinite", "(D)Z"}, {"doubleToLongBits", "(D)J"}, {"longBitsToDouble", "(J)D"}},
                     {{"value", "D"}});
            regClass(rt, "java/util/Enumeration", "java/lang/Object", {{"hasMoreElements", "()Z"}, {"nextElement", "()Ljava/lang/Object;"}}, none);
            regClass(rt, "java/util/ArrayEnumeration", "java/lang/Object", {{"hasMoreElements", "()Z"}, {"nextElement", "()Ljava/lang/Object;"}},
                     {{"arr", "[Ljava/lang/Object;"}, {"pos", "I"}});
            regClass(rt, "java/util/Date", "java/lang/Object",
                     {{"<init>", "()V"}, {"<init>", "(J)V"}, {"getTime", "()J"}, {"setTime", "(J)V"}, {"toString", "()Ljava/lang/String;"}},
                     {{"time", "J"}});
            regClass(rt, "java/util/Calendar", "java/lang/Object",
                     {{"getInstance", "()Ljava/util/Calendar;"}, {"get", "(I)I"}, {"getTime", "()Ljava/util/Date;"}, {"setTime", "(Ljava/util/Date;)V"}, {"getTimeInMillis", "()J"}, {"setTimeInMillis", "(J)V"}},
                     {{"time", "J"}});
            regClass(rt, "java/util/TimeZone", "java/lang/Object",
                     {{"getDefault", "()Ljava/util/TimeZone;"}, {"getID", "()Ljava/lang/String;"}},
                     {{"id", "I"}});
            regClass(rt, "java/lang/StringBuffer", "java/lang/Object",
                     {{"insert", "(ILjava/lang/String;)Ljava/lang/StringBuffer;"}, {"insert", "(IC)Ljava/lang/StringBuffer;"}, {"insert", "(II)Ljava/lang/StringBuffer;"}, {"reverse", "()Ljava/lang/StringBuffer;"}, {"deleteCharAt", "(I)Ljava/lang/StringBuffer;"}, {"capacity", "()I"}, {"ensureCapacity", "(I)V"}, {"append", "([CII)Ljava/lang/StringBuffer;"}, {"indexOf", "(Ljava/lang/String;)I"}, {"substring", "(II)Ljava/lang/String;"}, {"substring", "(I)Ljava/lang/String;"}, {"<init>", "()V"}, {"<init>", "(Ljava/lang/String;)V"}, {"append", "([C)Ljava/lang/StringBuffer;"}, {"<init>", "(I)V"}, {"append", "(Ljava/lang/String;)Ljava/lang/StringBuffer;"}, {"append", "(I)Ljava/lang/StringBuffer;"}, {"append", "(C)Ljava/lang/StringBuffer;"}, {"append", "(J)Ljava/lang/StringBuffer;"}, {"append", "(Z)Ljava/lang/StringBuffer;"}, {"append", "(Ljava/lang/Object;)Ljava/lang/StringBuffer;"}, {"append", "(F)Ljava/lang/StringBuffer;"}, {"append", "(D)Ljava/lang/StringBuffer;"}, {"toString", "()Ljava/lang/String;"}, {"length", "()I"}, {"charAt", "(I)C"}, {"setCharAt", "(IC)V"}, {"setLength", "(I)V"}, {"delete", "(II)Ljava/lang/StringBuffer;"}},
                     {{"str", "Ljava/lang/String;"}

                     });
            regClass(rt, "java/lang/Thread", "java/lang/Object",
                     {{"<init>", "(Ljava/lang/Runnable;)V"}, {"<init>", "(Ljava/lang/Runnable;Ljava/lang/String;)V"}, {"run", "()V"}, {"start", "()V"}, {"sleep", "(J)V"}, {"yield", "()V"}, {"currentThread", "()Ljava/lang/Thread;"}, {"setPriority", "(I)V"}, {"interrupt", "()V"}, {"isAlive", "()Z"}, {"join", "()V"}},
                     {{"r", "Ljava/lang/Runnable;"}});
            regClass(rt, "java/util/Hashtable", "java/lang/Object",
                     {{"keys", "()Ljava/util/Enumeration;"}, {"elements", "()Ljava/util/Enumeration;"}, {"contains", "(Ljava/lang/Object;)Z"}, {"<init>", "(I)V"}, {"<init>", "()V"}, {"get", "(Ljava/lang/Object;)Ljava/lang/Object;"}, {"put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"}, {"remove", "(Ljava/lang/Object;)Ljava/lang/Object;"}, {"containsKey", "(Ljava/lang/Object;)Z"}, {"clear", "()V"}, {"size", "()I"}, {"isEmpty", "()Z"}},
                     {{"count", "I"}, {"table", "[Ljava/lang/Object;"}});
            regClass(rt, "java/util/Random", "java/lang/Object",
                     {{"<init>", "(J)V"}, {"<init>", "()V"}, {"setSeed", "(J)V"}, {"nextInt", "()I"}, {"nextInt", "(I)I"}, {"nextLong", "()J"}, {"nextDouble", "()D"}, {"nextFloat", "()F"}, {"nextBoolean", "()Z"}},
                     {{"seed", "J"}});
            regClass(rt, "java/util/Vector", "java/lang/Object",
                     {{"trimToSize", "()V"}, {"ensureCapacity", "(I)V"}, {"elements", "()Ljava/util/Enumeration;"}, {"copyInto", "([Ljava/lang/Object;)V"}, {"setSize", "(I)V"}, {"capacity", "()I"}, {"indexOf", "(Ljava/lang/Object;I)I"}, {"lastIndexOf", "(Ljava/lang/Object;)I"}, {"toString", "()Ljava/lang/String;"}, {"<init>", "()V"}, {"<init>", "(I)V"}, {"<init>", "(II)V"}, {"addElement", "(Ljava/lang/Object;)V"}, {"elementAt", "(I)Ljava/lang/Object;"}, {"setElementAt", "(Ljava/lang/Object;I)V"}, {"insertElementAt", "(Ljava/lang/Object;I)V"}, {"removeElement", "(Ljava/lang/Object;)Z"}, {"removeElementAt", "(I)V"}, {"removeAllElements", "()V"}, {"size", "()I"}, {"isEmpty", "()Z"}, {"contains", "(Ljava/lang/Object;)Z"}, {"indexOf", "(Ljava/lang/Object;)I"}, {"firstElement", "()Ljava/lang/Object;"}, {"lastElement", "()Ljava/lang/Object;"}},
                     {{"elementCount", "I"}, {"elementData", "[Ljava/lang/Object;"}});
            regClass(rt, "java/util/Stack", "java/util/Vector",
                     {{"<init>", "()V"}, {"push", "(Ljava/lang/Object;)Ljava/lang/Object;"}, {"pop", "()Ljava/lang/Object;"}, {"peek", "()Ljava/lang/Object;"}, {"empty", "()Z"}, {"search", "(Ljava/lang/Object;)I"}},
                     none);

            // --- java.io ---
            regClass(rt, "java/io/InputStream", "java/lang/Object",
                     {{"read", "()I"}, {"read", "([B)I"}, {"read", "([BII)I"}, {"available", "()I"}, {"skip", "(J)J"}, {"close", "()V"}, {"markSupported", "()Z"}, {"mark", "(I)V"}, {"reset", "()V"}},
                     {{"data", "[B"}, {"pos", "I"}, {"limit", "I"}});
            regClass(rt, "java/io/ByteArrayInputStream", "java/io/InputStream",
                     {{"<init>", "([B)V"}, {"<init>", "([BII)V"}, {"read", "()I"}, {"read", "([B)I"}, {"read", "([BII)I"}, {"available", "()I"}, {"skip", "(J)J"}, {"close", "()V"}, {"markSupported", "()Z"}, {"mark", "(I)V"}, {"reset", "()V"}},
                     {{"data", "[B"}, {"pos", "I"}, {"limit", "I"}});
regClass(rt, "java/io/DataInputStream", "java/lang/Object",
                 {{"<init>", "(Ljava/io/InputStream;)V"}, {"read", "()I"}, {"read", "([B)I"}, {"read", "([BII)I"}, {"readBoolean", "()Z"}, {"readByte", "()B"}, {"readUnsignedByte", "()I"}, {"readShort", "()S"}, {"readUnsignedShort", "()I"}, {"readChar", "()C"}, {"readInt", "()I"}, {"readLong", "()J"}, {"readFully", "([B)V"}, {"readFully", "([BII)V"}, {"readUTF", "()Ljava/lang/String;"}, {"skipBytes", "(I)I"}, {"available", "()I"}, {"close", "()V"}, {"mark", "(I)V"}, {"reset", "()V"}},
                 {{"in", "Ljava/io/InputStream;"}, {"markpos", "I"}});
            regClass(rt, "java/io/OutputStream", "java/lang/Object",
                     {{"write", "(I)V"}, {"write", "([B)V"}, {"write", "([BII)V"}, {"flush", "()V"}, {"close", "()V"}},
                     none);
            regClass(rt, "java/io/ByteArrayOutputStream", "java/io/OutputStream",
                     {{"<init>", "()V"}, {"<init>", "(I)V"}, {"write", "(I)V"}, {"write", "([B)V"}, {"write", "([BII)V"}, {"size", "()I"}, {"reset", "()V"}, {"close", "()V"}, {"toByteArray", "()[B"}},
                     {{"buf", "Ljava/lang/String;"}});
            regClass(rt, "java/io/DataOutputStream", "java/io/OutputStream",
                     {{"<init>", "(Ljava/io/OutputStream;)V"}, {"write", "(I)V"}, {"write", "([B)V"}, {"write", "([BII)V"}, {"writeBoolean", "(Z)V"}, {"writeByte", "(I)V"}, {"writeShort", "(I)V"}, {"writeChar", "(I)V"}, {"writeInt", "(I)V"}, {"writeLong", "(J)V"}, {"writeUTF", "(Ljava/lang/String;)V"}, {"flush", "()V"}, {"close", "()V"}, {"size", "()I"}},
                     {{"out", "Ljava/io/OutputStream;"}});

            // --- MIDlet ---
            regClass(rt, "javax/microedition/midlet/MIDlet", "java/lang/Object",
                     {{"platformRequest", "(Ljava/lang/String;)Z"}, {"checkPermission", "(Ljava/lang/String;)I"}, {"<init>", "()V"}, {"getAppProperty", "(Ljava/lang/String;)Ljava/lang/String;"}, {"notifyDestroyed", "()V"}, {"notifyPaused", "()V"}, {"resumeRequest", "()Z"}},
                     none);

            // --- lcdui ---
            regClass(rt, "javax/microedition/lcdui/Displayable", "java/lang/Object",
                     {{"removeCommand", "(Ljavax/microedition/lcdui/Command;)V"}, {"setTicker", "(Ljavax/microedition/lcdui/Ticker;)V"}, {"getTicker", "()Ljavax/microedition/lcdui/Ticker;"}, {"setTitle", "(Ljava/lang/String;)V"}, {"getTitle", "()Ljava/lang/String;"}, {"addCommand", "(Ljavax/microedition/lcdui/Command;)V"}, {"setCommandListener", "(Ljavax/microedition/lcdui/CommandListener;)V"}, {"isShown", "()Z"}, {"getWidth", "()I"}, {"getHeight", "()I"}},
                     none);

            // GC_FULLSCREEN/GC_GFX (cf. plus bas) indexent cells[0]/cells[1] de
            // l'instance via les accesseurs gc_* -- et CES ACCESSEURS SONT PARTAGÉS
            // entre Canvas ET GameCanvas (setFullScreenMode/getGraphics/
            // flushGraphics/getKeyStates sont enregistrés sur les DEUX classes, cf.
            // plus bas). Les 2 champs doivent donc être réservés ici, sur Canvas
            // lui-même (l'ancêtre commun), et non sur GameCanvas seul : un jeu qui
            // étend directement Canvas (ou FullCanvas, qui étend Canvas) et appelle
            // setFullScreenMode()/getGraphics() sans jamais passer par GameCanvas
            // écrivait sinon directement dans cells[0]/cells[1] de l'instance --
            // silencieusement aliasés avec les 2 premiers champs applicatifs de la
            // sous-classe concrète (observé sur games/prince.jar : cells[0] d'un
            // Canvas obfusqué, son propre champ "bi" (référence vers le MIDlet),
            // écrasé par le booléen passé à setFullScreenMode(true), provoquant un
            // crash différé bien plus tard sur un getfield lisant ce même champ).
            regClass(rt, "javax/microedition/lcdui/Canvas", "javax/microedition/lcdui/Displayable",
                     {{"getKeyName", "(I)Ljava/lang/String;"}, {"getWidth", "()I"}, {"getHeight", "()I"}, {"isDoubleBuffered", "()Z"}, {"hasPointerEvents", "()Z"}, {"hasPointerMotionEvents", "()Z"}, {"hasRepeatEvents", "()Z"}, {"repaint", "()V"}, {"repaint", "(IIII)V"}, {"serviceRepaints", "()V"}, {"showNotify", "()V"}, {"hideNotify", "()V"}, {"setFullScreenMode", "(Z)V"}, {"getGraphics", "()Ljavax/microedition/lcdui/Graphics;"}, {"flushGraphics", "()V"}, {"flushGraphics", "(IIII)V"}, {"getKeyStates", "()I"}, {"getGameAction", "(I)I"}, {"getKeyCode", "(I)I"}, {"keyPressed", "(I)V"}, {"keyReleased", "(I)V"}, {"keyRepeated", "(I)V"}, {"pointerPressed", "(II)V"}, {"pointerReleased", "(II)V"}, {"pointerDragged", "(II)V"}},
                     {{"__fullscreen", "Z"}, {"__gfx", "Ljavax/microedition/lcdui/Graphics;"}});
            regClass(rt, "javax/microedition/lcdui/game/GameCanvas", "javax/microedition/lcdui/Canvas",
                     {{"<init>", "(Z)V"}, {"setFullScreenMode", "(Z)V"}, {"getGraphics", "()Ljavax/microedition/lcdui/Graphics;"}, {"flushGraphics", "()V"}, {"flushGraphics", "(IIII)V"}, {"getKeyStates", "()I"}},
                     none);
            // javax.microedition.lcdui.game : Sprite/TiledLayer/LayerManager/Layer.
            // Ces classes ne sont PAS dans le JAR (fournies par l'appareil MIDP) :
            // on les enregistre comme classes natives, résolues à la volée quand le
            // bytecode du jeu les référence (new Cowboy -> super() Sprite). Ordre
            // obligatoire : Layer d'abord (les autres lui empruntent ses slots 0..4
            // via l'héritage).
            regClass(rt, "javax/microedition/lcdui/game/Layer", "java/lang/Object",
                     {{"paint", "(Ljavax/microedition/lcdui/Graphics;)V"}, {"move", "(II)V"}, {"setPosition", "(II)V"}, {"setVisible", "(Z)V"}, {"isVisible", "()Z"}, {"getX", "()I"}, {"getY", "()I"}, {"getWidth", "()I"}, {"getHeight", "()I"}},
                     {{"x", "I"}, {"y", "I"}, {"width", "I"}, {"height", "I"}, {"visible", "Z"}});
            regClass(rt, "javax/microedition/lcdui/game/Sprite", "javax/microedition/lcdui/game/Layer",
                     {{"<init>", "(Ljavax/microedition/lcdui/Image;)V"}, {"<init>", "(Ljavax/microedition/lcdui/Image;II)V"}, {"<init>", "(Ljavax/microedition/lcdui/game/Sprite;)V"}, {"getRawFrameCount", "()I"}, {"setImage", "(Ljavax/microedition/lcdui/Image;II)V"}, {"defineCollisionRectangle", "(IIII)V"}, {"paint", "(Ljavax/microedition/lcdui/Graphics;)V"}, {"move", "(II)V"}, {"setPosition", "(II)V"}, {"setVisible", "(Z)V"}, {"isVisible", "()Z"}, {"getX", "()I"}, {"getY", "()I"}, {"getWidth", "()I"}, {"getHeight", "()I"}, {"setFrame", "(I)V"}, {"getFrame", "()I"}, {"nextFrame", "()V"}, {"prevFrame", "()V"}, {"getFrameSequenceLength", "()I"}, {"setFrameSequence", "([I)V"}, {"setTransform", "(I)V"}, {"defineReferencePixel", "(II)V"}, {"setRefPixelPosition", "(II)V"}, {"getRefPixelX", "()I"}, {"getRefPixelY", "()I"}, {"collidesWith", "(Ljavax/microedition/lcdui/game/Sprite;Z)Z"}, {"collidesWith", "(Ljavax/microedition/lcdui/Image;IZ)Z"}, {"collidesWith", "(Ljavax/microedition/lcdui/game/TiledLayer;Z)Z"}},
                     {{"__image", "Ljavax/microedition/lcdui/Image;"}, {"__fw", "I"}, {"__fh", "I"}, {"__seq", "[I"}, {"__frame", "I"}, {"__tfm", "I"}, {"__rx", "I"}, {"__ry", "I"}, {"__cx", "I"}, {"__cy", "I"}, {"__cw", "I"}, {"__ch", "I"}, {"__cset", "I"}});
            regClass(rt, "javax/microedition/lcdui/game/TiledLayer", "javax/microedition/lcdui/game/Layer",
                     {{"<init>", "(IILjavax/microedition/lcdui/Image;II)V"}, {"paint", "(Ljavax/microedition/lcdui/Graphics;)V"}, {"move", "(II)V"}, {"setPosition", "(II)V"}, {"setVisible", "(Z)V"}, {"isVisible", "()Z"}, {"getX", "()I"}, {"getY", "()I"}, {"getWidth", "()I"}, {"getHeight", "()I"}, {"setCell", "(III)V"}, {"getCell", "(II)I"}, {"fillCells", "(IIIII)V"}, {"getAnimatedTile", "(I)I"}, {"getCellWidth", "()I"}, {"getCellHeight", "()I"}, {"getColumns", "()I"}, {"getRows", "()I"}, {"setStaticTileSet", "(Ljavax/microedition/lcdui/Image;II)V"}, {"createAnimatedTile", "(I)I"}, {"setAnimatedTile", "(II)V"}},
                     {{"__image", "Ljavax/microedition/lcdui/Image;"}, {"__tw", "I"}, {"__th", "I"}, {"__cols", "I"}, {"__rows", "I"}, {"__grid", "[I"}, {"__anim", "[I"}});
            regClass(rt, "javax/microedition/lcdui/game/LayerManager", "java/lang/Object",
                     {{"<init>", "()V"}, {"append", "(Ljavax/microedition/lcdui/game/Layer;)V"}, {"insert", "(Ljavax/microedition/lcdui/game/Layer;I)V"}, {"remove", "(Ljavax/microedition/lcdui/game/Layer;)V"}, {"getSize", "()I"}, {"getLayerAt", "(I)Ljavax/microedition/lcdui/game/Layer;"}, {"setViewWindow", "(IIII)V"}, {"paint", "(Ljavax/microedition/lcdui/Graphics;II)V"}},
                     {{"__layers", "[Ljavax/microedition/lcdui/game/Layer;"}, {"__count", "I"}, {"__cap", "I"}, {"__viewX", "I"}, {"__viewY", "I"}, {"__viewW", "I"}, {"__viewH", "I"}});
            // com.nokia.mid.ui.FullCanvas : extension Nokia UI API (pas MIDP
            // standard), très utilisée par les jeux ciblant les téléphones Nokia de
            // l'époque à la place de javax.microedition.lcdui.Canvas -- observé sur
            // games/mission.jar (GloftMI3). Son API (paint/keyPressed/keyReleased/
            // getWidth/getHeight/repaint/setFullScreenMode/...) est un sur-ensemble
            // de Canvas ; on la fait donc hériter de notre Canvas natif pour
            // réutiliser telles quelles toutes ses natives. Les constantes de touche
            // Nokia (KEY_SOFTKEY1, KEY_UP_ARROW, ...) sont `static final int` côté
            // Java : javac les inline en littéraux au site d'appel, donc aucun champ
            // à déclarer ici pour qu'un getstatic les résolve.

            regClass(rt, "com/nokia/mid/ui/FullCanvas", "javax/microedition/lcdui/Canvas",
                     none, none);
            regClass(rt, "com/nokia/mid/ui/DirectUtils", "java/lang/Object",
                     {{"getDirectGraphics", "(Ljavax/microedition/lcdui/Graphics;)Lcom/nokia/mid/ui/DirectGraphics;"}, {"createImage", "(III)Ljavax/microedition/lcdui/Image;"}},
                     none);
            regClass(rt, "com/nokia/mid/ui/DeviceControl", "java/lang/Object",
                     {{"setLights", "(II)V"}},
                     none);
            // Implémentation native de l'interface DirectGraphics : le jeu appelle
            // ensuite drawPixels en invokeinterface, et notre invokeVirtual résout
            // via la classe RÉELLE du receiver (la classe déclarée par
            // l'InterfaceMethodref, com/nokia/mid/ui/DirectGraphics, n'a pas besoin
            // d'exister côté runtime).
            regClass(rt, "com/nokia/mid/ui/DirectGraphicsWrapper", "java/lang/Object",
                     {{"drawPixels", "([SZIIIIIIII)V"}, {"drawImage", "(Ljavax/microedition/lcdui/Image;IIII)V"}},
                     {{"__gfx", "Ljavax/microedition/lcdui/Graphics;"}});

regClass(rt, "java/util/TimerTask", "java/lang/Object",
                 {{"<init>", "()V"}, {"cancel", "()Z"}},
                 none);

            regClass(rt, "java/util/Timer", "java/lang/Object",
                     {{"<init>", "()V"},
                      {"schedule", "(Ljava/util/TimerTask;J)V"},
                      {"schedule", "(Ljava/util/TimerTask;JJ)V"},
                      {"scheduleAtFixedRate", "(Ljava/util/TimerTask;J)V"},
                      {"scheduleAtFixedRate", "(Ljava/util/TimerTask;JJ)V"},
                      {"cancel", "()V"}},
                     none);

            regClass(rt, "com/nokia/mid/sound/Sound", "java/lang/Object",
                     {{"play", "(I)V"}, {"<init>", "(IJ)V"}, {"<init>", "([BI)V"}}, none);
            // javax.microedition.rms.RecordStore : magasin d'enregistrements MIDP
            // (sauvegardes/scores/préférences). Implémentation minimale en mémoire
            // côté C++ (natives.cpp, rsRegistry()) -- non persistante entre
            // lancements, mais suffisante pour le chemin "aucune sauvegarde
            // existante" (getNumRecords()==0) qu'exercent la plupart des jeux au
            // premier lancement.
            regClass(rt, "javax/microedition/rms/RecordStore", "java/lang/Object",
                     {{"openRecordStore", "(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;"},
                      {"openRecordStore", "(Ljava/lang/String;ZIZ)Ljavax/microedition/rms/RecordStore;"},
                      {"openRecordStore", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljavax/microedition/rms/RecordStore;"},
                      {"getNumRecords", "()I"}, {"getName", "()Ljava/lang/String;"}, {"getVersion", "()I"}, {"getSize", "()I"},
                      {"getSizeAvailable", "()I"}, {"getLastModified", "()J"}, {"getNextRecordID", "()I"},
                      {"getRecord", "(I[BI)I"}, {"getRecord", "(I)[B"}, {"getRecordSize", "(I)I"},
                      {"setRecord", "(I[BII)V"}, {"addRecord", "([BII)I"}, {"deleteRecord", "(I)V"},
                      {"closeRecordStore", "()V"}, {"deleteRecordStore", "(Ljava/lang/String;)V"},
                      {"listRecordStores", "()[Ljava/lang/String;"},
                      {"addRecordListener", "(Ljavax/microedition/rms/RecordListener;)V"}, {"removeRecordListener", "(Ljavax/microedition/rms/RecordListener;)V"},
                      {"setMode", "(IZ)V"},
                      {"enumerateRecords", "(Ljavax/microedition/rms/RecordFilter;Ljavax/microedition/rms/RecordComparator;Z)Ljavax/microedition/rms/RecordEnumeration;"}},
                     {{"__name", "Ljava/lang/String;"}, {"__open", "I"}});
            regClass(rt, "javax/microedition/rms/RecordStoreException", "java/lang/Exception", none, none);
            regClass(rt, "javax/microedition/rms/RecordStoreNotFoundException", "javax/microedition/rms/RecordStoreException", none, none);
            regClass(rt, "javax/microedition/rms/RecordStoreNotOpenException", "javax/microedition/rms/RecordStoreException", none, none);
            regClass(rt, "javax/microedition/rms/RecordStoreFullException", "javax/microedition/rms/RecordStoreException", none, none);
            regClass(rt, "javax/microedition/rms/InvalidRecordIDException", "javax/microedition/rms/RecordStoreException", none, none);
            regClass(rt, "javax/microedition/rms/RecordFilter", "java/lang/Object", {{"matches", "([B)Z"}}, none);
            regClass(rt, "javax/microedition/rms/RecordComparator", "java/lang/Object", {{"compare", "([B[B)I"}}, none);
            regClass(rt, "javax/microedition/rms/RecordListener", "java/lang/Object", none, none);
            regClass(rt, "javax/microedition/rms/RecordEnumeration", "java/lang/Object",
                     {{"hasNextElement", "()Z"}, {"hasPreviousElement", "()Z"}, {"nextRecordId", "()I"}, {"previousRecordId", "()I"}, {"nextRecord", "()[B"}, {"previousRecord", "()[B"}, {"numRecords", "()I"}, {"destroy", "()V"}, {"reset", "()V"}, {"rebuild", "()V"}, {"isKeptUpdated", "()Z"}, {"keepUpdated", "(Z)V"}},
                     none);
            // javax.microedition.rms.RecordEnumeration : implémenté nativement par une classe d'accueil (l'interface n'a pas
            // de bytecode) ; ses instances portent l'instantané des ids (cf. natives.cpp, n_RS_enumerate).
            regClass(rt, "javax/microedition/rms/RecordEnumerationImpl", "java/lang/Object",
                     {{"hasNextElement", "()Z"}, {"hasPreviousElement", "()Z"}, {"nextRecordId", "()I"}, {"previousRecordId", "()I"}, {"nextRecord", "()[B"}, {"previousRecord", "()[B"}, {"numRecords", "()I"}, {"destroy", "()V"}, {"reset", "()V"}, {"rebuild", "()V"}, {"isKeptUpdated", "()Z"}, {"keepUpdated", "(Z)V"}},
                     {{"store", "Ljava/lang/String;"}, {"ids", "[I"}, {"pos", "I"}, {"keep", "I"}, {"filter", "Ljava/lang/Object;"}, {"cmp", "Ljava/lang/Object;"}});
            regClass(rt, "javax/microedition/lcdui/Graphics", "java/lang/Object",
                     {{"getRedComponent", "()I"}, {"getGreenComponent", "()I"}, {"getBlueComponent", "()I"}, {"getDisplayColor", "(I)I"}, {"getStrokeStyle", "()I"}, {"setStrokeStyle", "(I)V"}, {"drawSubstring", "(Ljava/lang/String;IIIII)V"}, {"setColor", "(I)V"}, {"setColor", "(III)V"}, {"getColor", "()I"}, {"setGrayScale", "(I)V"}, {"getGrayScale", "()I"}, {"fillRect", "(IIII)V"}, {"drawRect", "(IIII)V"}, {"drawLine", "(IIII)V"}, {"fillTriangle", "(IIIIII)V"}, {"drawArc", "(IIIIII)V"}, {"fillArc", "(IIIIII)V"}, {"fillRoundRect", "(IIIIII)V"}, {"drawRoundRect", "(IIIIII)V"}, {"setFont", "(Ljavax/microedition/lcdui/Font;)V"}, {"getFont", "()Ljavax/microedition/lcdui/Font;"}, {"drawString", "(Ljava/lang/String;II)V"}, {"drawString", "(Ljava/lang/String;III)V"}, {"drawChar", "(CIII)V"}, {"drawChars", "([CIIIII)V"}, {"drawImage", "(Ljavax/microedition/lcdui/Image;III)V"}, {"drawRegion", "(Ljavax/microedition/lcdui/Image;IIIIIIII)V"}, {"setClip", "(IIII)V"}, {"clipRect", "(IIII)V"}, {"getClipX", "()I"}, {"getClipY", "()I"}, {"getClipWidth", "()I"}, {"getClipHeight", "()I"}, {"translate", "(II)V"}, {"getTranslateX", "()I"}, {"getTranslateY", "()I"}, {"drawRGB", "([IIIIIIIZ)V"}},
                     {{"color", "I"}, {"font", "Ljavax/microedition/lcdui/Font;"}, {"translateX", "I"}, {"translateY", "I"}, {"clipX", "I"}, {"clipY", "I"}, {"clipW", "I"}, {"clipH", "I"}, {"mode", "I"}, {"targetW", "I"}, {"targetH", "I"}, {"stride", "I"}, {"buf", "[I"}});
            regClass(rt, "javax/microedition/lcdui/Font", "java/lang/Object",
                     {{"getFont", "(III)Ljavax/microedition/lcdui/Font;"}, {"getDefaultFont", "()Ljavax/microedition/lcdui/Font;"}, {"getHeight", "()I"}, {"getBaselinePosition", "()I"}, {"getFace", "()I"}, {"getStyle", "()I"}, {"getSize", "()I"}, {"stringWidth", "(Ljava/lang/String;)I"}, {"charWidth", "(C)I"}, {"charsWidth", "([CII)I"}},
                     {{"face", "I"}, {"style", "I"}, {"size", "I"}});
            regClass(rt, "javax/microedition/lcdui/Image", "java/lang/Object",
                     {{"createImage", "(Ljavax/microedition/lcdui/Image;IIIII)Ljavax/microedition/lcdui/Image;"}, {"createImage", "(II)Ljavax/microedition/lcdui/Image;"}, {"createImage", "(Ljava/lang/String;)Ljavax/microedition/lcdui/Image;"}, {"createImage", "(Ljava/io/InputStream;)Ljavax/microedition/lcdui/Image;"}, {"createImage", "([BII)Ljavax/microedition/lcdui/Image;"}, {"createImage", "(Ljavax/microedition/lcdui/Image;)Ljavax/microedition/lcdui/Image;"}, {"createRGBImage", "([IIIZ)Ljavax/microedition/lcdui/Image;"}, {"getGraphics", "()Ljavax/microedition/lcdui/Graphics;"}, {"getWidth", "()I"}, {"getHeight", "()I"}, {"isMutable", "()Z"}, {"getRGB", "([IIIIII)V"}, {"getRGB", "([IIIIIII)V"}},
                     {{"width", "I"}, {"height", "I"}, {"mutable", "Z"}, {"buf", "[I"}, {"gfx", "Ljavax/microedition/lcdui/Graphics;"}});
            regClass(rt, "javax/microedition/lcdui/Display", "java/lang/Object",
                     {{"vibrate", "(I)Z"}, {"getColor", "(I)I"}, {"getBorderStyle", "(Z)I"}, {"getDisplay", "(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;"}, {"getCurrent", "()Ljavax/microedition/lcdui/Displayable;"}, {"setCurrent", "(Ljavax/microedition/lcdui/Displayable;)V"}, {"setCurrent", "(Ljavax/microedition/lcdui/Alert;Ljavax/microedition/lcdui/Displayable;)V"}, {"setCurrentItem", "(Ljavax/microedition/lcdui/Item;)V"}, {"isColor", "()Z"}, {"numColors", "()I"}, {"numAlphaLevels", "()I"}, {"flashBacklight", "(I)V"}, {"repaint", "()V"}, {"callSerially", "(Ljava/lang/Runnable;)V"}},
                     none);

            // --- lcdui haut niveau : Alert / Command / Form / List / Items / TextBox (état dans midp_lcdui.cpp) ---
            regClass(rt, "javax/microedition/lcdui/Alert", "javax/microedition/lcdui/Displayable",
                     {{"<init>", "(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;)V"}, {"<init>", "(Ljava/lang/String;)V"}, {"setTimeout", "(I)V"}, {"getTimeout", "()I"}, {"setString", "(Ljava/lang/String;)V"}, {"getString", "()Ljava/lang/String;"}, {"setIndicator", "(Ljavax/microedition/lcdui/Gauge;)V"}, {"getIndicator", "()Ljavax/microedition/lcdui/Gauge;"}, {"setType", "(Ljavax/microedition/lcdui/AlertType;)V"}, {"setImage", "(Ljavax/microedition/lcdui/Image;)V"}},
                     {{"DISMISS_COMMAND", "Ljavax/microedition/lcdui/Command;"}});
            regClass(rt, "javax/microedition/lcdui/AlertType", "java/lang/Object",
                     {{"<init>", "()V"}},
                     {{"ALARM", "Ljavax/microedition/lcdui/AlertType;"},
                      {"CONFIRMATION", "Ljavax/microedition/lcdui/AlertType;"},
                      {"ERROR", "Ljavax/microedition/lcdui/AlertType;"},
                      {"INFO", "Ljavax/microedition/lcdui/AlertType;"},
                      {"WARNING", "Ljavax/microedition/lcdui/AlertType;"}});
            regClass(rt, "javax/microedition/lcdui/Command", "java/lang/Object",
                     {{"<init>", "(Ljava/lang/String;II)V"}, {"<init>", "(Ljava/lang/String;Ljava/lang/String;II)V"}, {"getLabel", "()Ljava/lang/String;"}, {"getLongLabel", "()Ljava/lang/String;"}, {"getCommandType", "()I"}, {"getPriority", "()I"}},
                     none);
            regClass(rt, "javax/microedition/lcdui/Item", "java/lang/Object",
                     {{"getLabel", "()Ljava/lang/String;"}, {"setLabel", "(Ljava/lang/String;)V"}, {"getLayout", "()I"}, {"setLayout", "(I)V"}, {"addCommand", "(Ljavax/microedition/lcdui/Command;)V"}, {"removeCommand", "(Ljavax/microedition/lcdui/Command;)V"}, {"setDefaultCommand", "(Ljavax/microedition/lcdui/Command;)V"}, {"setItemCommandListener", "(Ljavax/microedition/lcdui/ItemCommandListener;)V"}, {"notifyStateChanged", "()V"}, {"setPreferredSize", "(II)V"}}, none);
            regClass(rt, "javax/microedition/lcdui/Form", "javax/microedition/lcdui/Displayable",
                     {{"<init>", "(Ljava/lang/String;)V"}, {"<init>", "(Ljava/lang/String;[Ljavax/microedition/lcdui/Item;)V"}, {"append", "(Ljavax/microedition/lcdui/Item;)I"}, {"append", "(Ljava/lang/String;)I"}, {"append", "(Ljavax/microedition/lcdui/Image;)I"}, {"insert", "(ILjavax/microedition/lcdui/Item;)V"}, {"set", "(ILjavax/microedition/lcdui/Item;)V"}, {"delete", "(I)V"}, {"deleteAll", "()V"}, {"get", "(I)Ljavax/microedition/lcdui/Item;"}, {"size", "()I"}, {"setItemStateListener", "(Ljavax/microedition/lcdui/ItemStateListener;)V"}},
                     none);
            regClass(rt, "javax/microedition/lcdui/List", "javax/microedition/lcdui/Displayable",
                     {{"<init>", "(Ljava/lang/String;I[Ljava/lang/String;Ljavax/microedition/lcdui/Image;)V"}, {"<init>", "(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V"}, {"<init>", "(Ljava/lang/String;I)V"}, {"setSelectCommand", "(Ljavax/microedition/lcdui/Command;)V"}, {"append", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I"}, {"insert", "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V"}, {"set", "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V"}, {"delete", "(I)V"}, {"deleteAll", "()V"}, {"size", "()I"}, {"getString", "(I)Ljava/lang/String;"}, {"getImage", "(I)Ljavax/microedition/lcdui/Image;"}, {"isSelected", "(I)Z"}, {"getSelectedIndex", "()I"}, {"setSelectedIndex", "(IZ)V"}, {"getSelectedFlags", "([Z)I"}, {"setSelectedFlags", "([Z)V"}, {"setFitPolicy", "(I)V"}, {"getFitPolicy", "()I"}, {"setFont", "(ILjavax/microedition/lcdui/Font;)V"}},
                     {{"SELECT_COMMAND", "Ljavax/microedition/lcdui/Command;"}});
            regClass(rt, "javax/microedition/lcdui/TextBox", "javax/microedition/lcdui/Displayable",
                     {{"<init>", "(Ljava/lang/String;Ljava/lang/String;II)V"}, {"getString", "()Ljava/lang/String;"}, {"setString", "(Ljava/lang/String;)V"}, {"size", "()I"}, {"getMaxSize", "()I"}, {"setMaxSize", "(I)I"}, {"getConstraints", "()I"}, {"setConstraints", "(I)V"}, {"getCaretPosition", "()I"}, {"insert", "(Ljava/lang/String;I)V"}, {"delete", "(II)V"}, {"getChars", "([C)I"}, {"setChars", "([CII)V"}, {"setInitialInputMode", "(Ljava/lang/String;)V"}},
                     none);
            regClass(rt, "javax/microedition/lcdui/TextField", "javax/microedition/lcdui/Item",
                     {{"<init>", "(Ljava/lang/String;Ljava/lang/String;II)V"}, {"getString", "()Ljava/lang/String;"}, {"setString", "(Ljava/lang/String;)V"}, {"size", "()I"}, {"getMaxSize", "()I"}, {"setMaxSize", "(I)I"}, {"getConstraints", "()I"}, {"setConstraints", "(I)V"}, {"getCaretPosition", "()I"}, {"insert", "(Ljava/lang/String;I)V"}, {"delete", "(II)V"}, {"getChars", "([C)I"}, {"setChars", "([CII)V"}, {"setInitialInputMode", "(Ljava/lang/String;)V"}},
                     none);
            regClass(rt, "javax/microedition/lcdui/StringItem", "javax/microedition/lcdui/Item",
                     {{"<init>", "(Ljava/lang/String;Ljava/lang/String;)V"}, {"<init>", "(Ljava/lang/String;Ljava/lang/String;I)V"}, {"setText", "(Ljava/lang/String;)V"}, {"getText", "()Ljava/lang/String;"}, {"getAppearanceMode", "()I"}, {"setFont", "(Ljavax/microedition/lcdui/Font;)V"}},
                     none);
            regClass(rt, "javax/microedition/lcdui/ImageItem", "javax/microedition/lcdui/Item",
                     {{"<init>", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;)V"}, {"<init>", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;I)V"}, {"getImage", "()Ljavax/microedition/lcdui/Image;"}, {"setImage", "(Ljavax/microedition/lcdui/Image;)V"}, {"getAltText", "()Ljava/lang/String;"}, {"setAltText", "(Ljava/lang/String;)V"}, {"getAppearanceMode", "()I"}}, none);
            regClass(rt, "javax/microedition/lcdui/Gauge", "javax/microedition/lcdui/Item",
                     {{"<init>", "(Ljava/lang/String;ZII)V"}, {"setValue", "(I)V"}, {"getValue", "()I"}, {"setMaxValue", "(I)V"}, {"getMaxValue", "()I"}, {"isInteractive", "()Z"}}, none);
            regClass(rt, "javax/microedition/lcdui/ChoiceGroup", "javax/microedition/lcdui/Item",
                     {{"<init>", "(Ljava/lang/String;I[Ljava/lang/String;Ljavax/microedition/lcdui/Image;)V"}, {"<init>", "(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V"}, {"<init>", "(Ljava/lang/String;I)V"}, {"append", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I"}, {"insert", "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V"}, {"set", "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V"}, {"delete", "(I)V"}, {"deleteAll", "()V"}, {"size", "()I"}, {"getString", "(I)Ljava/lang/String;"}, {"getImage", "(I)Ljavax/microedition/lcdui/Image;"}, {"isSelected", "(I)Z"}, {"getSelectedIndex", "()I"}, {"setSelectedIndex", "(IZ)V"}, {"getSelectedFlags", "([Z)I"}, {"setSelectedFlags", "([Z)V"}, {"setFitPolicy", "(I)V"}, {"getFitPolicy", "()I"}, {"setFont", "(ILjavax/microedition/lcdui/Font;)V"}},
                     none);
            regClass(rt, "javax/microedition/lcdui/DateField", "javax/microedition/lcdui/Item", {{"<init>", "(Ljava/lang/String;I)V"}}, none);
            regClass(rt, "javax/microedition/lcdui/Spacer", "javax/microedition/lcdui/Item", {{"<init>", "(II)V"}}, none);
            regClass(rt, "javax/microedition/lcdui/Choice", "java/lang/Object", {{"append", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I"}, {"insert", "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V"}, {"set", "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V"}, {"delete", "(I)V"}, {"deleteAll", "()V"}, {"size", "()I"}, {"getString", "(I)Ljava/lang/String;"}, {"getImage", "(I)Ljavax/microedition/lcdui/Image;"}, {"isSelected", "(I)Z"}, {"getSelectedIndex", "()I"}, {"setSelectedIndex", "(IZ)V"}, {"getSelectedFlags", "([Z)I"}, {"setSelectedFlags", "([Z)V"}, {"setFitPolicy", "(I)V"}, {"getFitPolicy", "()I"}, {"setFont", "(ILjavax/microedition/lcdui/Font;)V"}}, none);
            regClass(rt, "javax/microedition/lcdui/ItemStateListener", "java/lang/Object", {{"itemStateChanged", "(Ljavax/microedition/lcdui/Item;)V"}}, none);
            regClass(rt, "javax/microedition/lcdui/Ticker", "java/lang/Object",
                     {{"<init>", "(Ljava/lang/String;)V"}}, none);
            regClass(rt, "javax/microedition/lcdui/Screen", "javax/microedition/lcdui/Displayable", none, none);
            regClass(rt, "javax/microedition/lcdui/ItemCommandListener", "java/lang/Object", none, none);
            regClass(rt, "javax/microedition/lcdui/CommandListener", "java/lang/Object", none, none);
            regClass(rt, "javax/microedition/lcdui/DisplayableCommandListener", "java/lang/Object", none, none);
            regClass(rt, "javax/microedition/lcdui/CustomItem", "javax/microedition/lcdui/Item",
                     {{"<init>", "(Ljava/lang/String;)V"}, {"getMinContentWidth", "()I"}, {"getMinContentHeight", "()I"}, {"getPrefContentWidth", "(I)I"}, {"getPrefContentHeight", "(I)I"}, {"repaint", "()V"}, {"repaint", "(IIII)V"}, {"invalidate", "()V"}, {"getInteractionModes", "()I"}},
                     none);

            // --- MMAPI (audio simulé : états + événements "started"/"endOfMedia") ---
            regClass(rt, "javax/microedition/media/Player", "java/lang/Object",
                     {{"realize", "()V"}, {"prefetch", "()V"}, {"start", "()V"}, {"stop", "()V"}, {"deallocate", "()V"}, {"close", "()V"}, {"setLoopCount", "(I)V"}, {"getState", "()I"}, {"getDuration", "()J"}, {"getMediaTime", "()J"}, {"setMediaTime", "(J)J"}, {"getContentType", "()Ljava/lang/String;"}, {"getControl", "(Ljava/lang/String;)Ljavax/microedition/media/Control;"}, {"addPlayerListener", "(Ljavax/microedition/media/PlayerListener;)V"}, {"removePlayerListener", "(Ljavax/microedition/media/PlayerListener;)V"}},
                     {{"st", "I"}, {"ln", "Ljava/lang/Object;"}, {"ct", "Ljava/lang/String;"}, {"trk", "I"}});

            regClass(rt, "javax/microedition/media/Manager", "java/lang/Object",
                     {{"createPlayer", "(Ljava/lang/String;)Ljavax/microedition/media/Player;"},
                      {"createPlayer", "(Ljava/io/InputStream;Ljava/lang/String;)Ljavax/microedition/media/Player;"},
                      {"playTone", "(III)V"}},
                     none);
            regClass(rt, "javax/microedition/media/PlayerListener", "java/lang/Object", none, none);
            regClass(rt, "javax/microedition/media/Control", "java/lang/Object", none, none);
            regClass(rt, "javax/microedition/media/Controllable", "java/lang/Object",
                     {{"getControl", "(Ljava/lang/String;)Ljavax/microedition/media/Control;"}},
                     none);
            regClass(rt, "javax/microedition/media/control/VolumeControl", "java/lang/Object",
                     {{"getLevel", "()I"}, {"setLevel", "(I)I"}, {"getMute", "()Z"}, {"setMute", "(Z)V"}},
                     none);
            regClass(rt, "com/nokia/mid/ui/DirectGraphics", "java/lang/Object", none, none);

            // ---- Handlers de base (java.lang) ----
            // ATTENTION : natives.cpp (initNatives(), appelé avant midp::init() —
            // cf. main.cpp) enregistre déjà des implémentations réelles pour
            // Object/String/Math/System/PrintStream/Class.getName+forName.
            // registerNative() fait un simple écrasement de map (registry()[key]=fn) :
            // réenregistrer ces mêmes clés ici avec `ui_noop` les neutralisait
            // SILENCIEUSEMENT (aucune erreur, juste un no-op qui ne renseigne jamais
            // le résultat). Bug sévère et large spectre — entre autres,
            // System.currentTimeMillis() retournait toujours 0 et System.arraycopy()
            // ne faisait plus rien pour absolument tous les MIDlets, symptôme
            // observé indirectement dans plusieurs diagnostics précédents sur ce
            // dépôt. Seule Class.getResourceAsStream n'est PAS dupliquée par
            // natives.cpp (elle a besoin d'accéder au jar, propre à cette couche
            // midp) : c'est la seule entrée qui doit rester ici.

            // ---- Handlers java.io ----


            // ---- Handlers lcdui ----
            regN("java/lang/StringBuffer.append:([C)Ljava/lang/StringBuffer;", n_SB_appendCharArray);

            // Variante à 7 ints (descripteur [IIIIIII)V) référencée par
            // games/prince_of_persia_th_254262.jar : un pop fantôme de plus sur la
            // pile d'opérandes, mais les indices d'arguments réels restent les
            // mêmes dans le corps du native (args[2..7]).



            // Implémentations par défaut (no-op) : keyPressed/keyReleased/keyRepeated
            // et le pointeur tactile sont optionnels en MIDP -- une sous-classe qui
            // n'en surcharge qu'une partie (ex. keyPressed sans keyReleased) ne doit
            // pas faire échouer invokeVirtual quand tick() appelle l'autre.



            // UI stubs

            // ---- Handlers MMAPI (stub : pas de son) ----
            regN("java/lang/String.<init>:(Ljava/lang/StringBuffer;)V", n_String_initStringBuffer);

            // java/lang/System.out : en Java réel, c'est le VM bootstrap qui
            // l'initialise avant tout <clinit> utilisateur -- les classes natives
            // n'ont pas de <clinit> ici pour le faire. Sans ceci, tout
            // System.out.println(...) plante sur "thisObj NULL" (out reste à sa
            // valeur par défaut = référence nulle).
            {
                ClassInfo *sysCls = rt->classInfoOfName("java/lang/System");
                ClassInfo *psCls = rt->classInfoOfName("java/io/PrintStream");
                const MethodRecord *outField = sysCls ? sysCls->findField("out", "Ljava/io/PrintStream;") : nullptr;
                if (sysCls && psCls && outField)
                {
                    Obj *out = rt->heap().newInstance(psCls);
                    if (out)
                        sysCls->statics[outField->slot] = Value::fromRef(out);
                }
            }

            // javax.microedition.lcdui.AlertType.ALARM/CONFIRMATION/ERROR/
            // INFO/WARNING : contrairement aux `static final int`, ce sont
            // de vraies constantes objet (javac ne les inline pas) --
            // même bootstrap que System.out ci-dessus : sans <clinit> réel,
            // elles resteraient à leur référence nulle par défaut, et un
            // `getstatic AlertType.ERROR` planterait/échouerait juste avant
            // tout code qui les utilise (ex. games/jump.jar : Jump.errorMsg,
            // appelé par le catch d'une exception levée pendant la
            // création du Canvas -- observé pendant l'implémentation du
            // rattrapage d'exceptions réel dans l'interpréteur).
            {
                ClassInfo *atCls = rt->classInfoOfName("javax/microedition/lcdui/AlertType");
                if (atCls)
                {
                    static const char *kAlertConsts[] = {"ALARM", "CONFIRMATION", "ERROR", "INFO", "WARNING"};
                    for (const char *name : kAlertConsts)
                    {
                        const MethodRecord *f = atCls->findField(name, "Ljavax/microedition/lcdui/AlertType;");
                        if (f)
                        {
                            Obj *inst = rt->heap().newInstance(atCls);
                            if (inst)
                                atCls->statics[f->slot] = Value::fromRef(inst);
                        }
                    }
                }
            }

            // java.lang.Boolean.TRUE/FALSE : constantes objet (non inlinées par javac), amorcées comme AlertType.
            {
                ClassInfo *bc = rt->classInfoOfName("java/lang/Boolean");
                if (bc)
                    for (int v = 0; v < 2; v++)
                    {
                        const MethodRecord *f = bc->findField(v ? "TRUE" : "FALSE", "Ljava/lang/Boolean;");
                        Obj *inst = f ? rt->heap().newInstance(bc) : nullptr;
                        if (inst)
                        {
                            inst->cells[0] = Value::fromInt(v);
                            bc->statics[f->slot] = Value::fromRef(inst);
                        }
                    }
            }

            // List.SELECT_COMMAND / Alert.DISMISS_COMMAND : constantes objet (non inlinées par javac).
            lcduiBootstrap(rt);
        }

    } // namespace midp
} // namespace jvm
