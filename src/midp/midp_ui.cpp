// midp_ui.cpp -- Display/MIDlet/Canvas keys, List/Form/Command (UI réelle minimale)
// Découpé de l'ancien midp_natives.cpp ; état partagé : midp_internal.h.

#include "midp/midp_internal.h"

namespace jvm
{
    namespace midp
    {
        namespace detail
        {
            static void cv_getWidth(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, screenW());
            }
            static void cv_getHeight(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, screenH());
            }
            static void cv_isDoubleBuffered(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, 1);
            }
            static void cv_repaint(NativeContext *ctx)
            {
                if (jvm::jmeDebug())
                    fprintf(stderr, "REPAINT(%p)\n", (void *)ctx->thisObj);
                (void)ctx;
                g_paintRequested = true;
            }
            static void cv_repaintRegion(NativeContext *ctx)
            {
                (void)ctx;
                g_paintRequested = true;
            }
            static void cv_service(NativeContext *ctx) { (void)ctx; }
            static void cv_showNotify(NativeContext *ctx) { (void)ctx; }
            static void cv_hideNotify(NativeContext *ctx) { (void)ctx; }

            // Constantes standard MIDP Canvas.{UP=1,DOWN=6,LEFT=2,RIGHT=5,FIRE=8,
            // GAME_A=9,GAME_B=10,GAME_C=11,GAME_D=12} : ce sont des "game actions",
            // PAS des keyCodes -- ne pas les confondre avec les keyCodes que notre
            // HAL envoie réellement (halKeyToMidp, plus bas : -1/-2/-3/-4/-5 pour
            // UP/DOWN/LEFT/RIGHT/FIRE). cv_gameActionFor doit donc traduire un
            // VRAI keyCode vers l'action correspondante -- y compris la convention
            // MIDP du pavé numérique sans D-pad physique (2/8/4/6/5 -> UP/DOWN/
            // LEFT/RIGHT/FIRE) et 1/3/7/9 -> GAME_A..D. L'ancienne version testait
            // le keyCode contre 1/2/5/6/8 (les valeurs d'action elles-mêmes) : ça ne
            // correspondait jamais à un vrai keyCode, donc getGameAction() renvoyait
            // toujours 0 -- tout MIDlet pilotant ses déplacements via
            // getGameAction(keyCode) plutôt que via le keyCode brut restait
            // totalement inerte aux touches (régression observée : le même appui
            // D-pad fonctionne sur un jeu qui lit le keyCode brut mais pas sur un
            // autre qui passe par getGameAction, donnant l'impression que les deux
            // jeux n'ont pas le même mapping clavier).
            static int cv_gameActionFor(int keyCode)
            {
                if (jvm::jmeDebug())
                    fprintf(stderr, "GAMEACTION keyCode=%d\n", keyCode);
                switch (keyCode)
                {
                case -1:
                case '2':
                    return 1; // UP
                case -2:
                case '8':
                    return 6; // DOWN
                case -3:
                case '4':
                    return 2; // LEFT
                case -4:
                case '6':
                    return 5; // RIGHT
                case -5:
                case '5':
                    return 8; // FIRE
                case '1':
                    return 9; // GAME_A
                case '3':
                    return 10; // GAME_B
                case '7':
                    return 11; // GAME_C
                case '9':
                    return 12; // GAME_D
                default:
                    return 0;
                }
            }
            static void cv_getKeyName(NativeContext *ctx)
            {
                int k = argInt(ctx, 1);
                const char *n = nullptr;
                char buf[8];
                if (k >= 48 && k <= 57) { buf[0] = static_cast<char>(k); buf[1] = 0; n = buf; }
                else if (k == 42) n = "*";
                else if (k == 35) n = "#";
                else switch (k)
                {
                case -1: n = "UP"; break;
                case -2: n = "DOWN"; break;
                case -3: n = "LEFT"; break;
                case -4: n = "RIGHT"; break;
                case -5: n = "SELECT"; break;
                case -6: n = "SOFT1"; break;
                case -7: n = "SOFT2"; break;
                case -8: n = "CLEAR"; break;
                default: n = "UNKNOWN"; break;
                }
                setRef(ctx, g_rt->heap().newString(n));
            }
            static void ui_vibrate(NativeContext *ctx) { setInt(ctx, 1); }
            static void ui_displayColor(NativeContext *ctx) { setInt(ctx, argInt(ctx, 1) == 0 ? 0xFFFFFF : 0x000000); }
            static void ui_platformRequest(NativeContext *ctx) { setInt(ctx, 0); }
            static void ui_checkPermission(NativeContext *ctx) { setInt(ctx, 1); }
            static void cv_getGameAction(NativeContext *ctx) { setInt(ctx, cv_gameActionFor(argInt(ctx, 1))); }
            // Inverse de cv_gameActionFor : renvoie le keyCode canonique pour une
            // action donnée (même confusion action/keyCode corrigée ici).
            static void cv_getKeyCode(NativeContext *ctx)
            {
                switch (argInt(ctx, 1))
                {
                case 1:
                    setInt(ctx, -1); // UP
                    return;
                case 6:
                    setInt(ctx, -2); // DOWN
                    return;
                case 2:
                    setInt(ctx, -3); // LEFT
                    return;
                case 5:
                    setInt(ctx, -4); // RIGHT
                    return;
                case 8:
                    setInt(ctx, -5); // FIRE
                    return;
                case 9:
                    setInt(ctx, '1');
                    return;
                case 10:
                    setInt(ctx, '3');
                    return;
                case 11:
                    setInt(ctx, '7');
                    return;
                case 12:
                    setInt(ctx, '9');
                    return;
                default:
                    setInt(ctx, 0);
                    return;
                }
            }

            // ---------------------------------------------------------------------
            // Display natives
            // ---------------------------------------------------------------------

            static void d_getDisplay(NativeContext *ctx)
            {
                if (!g_display)
                    g_display = makeInstance("javax/microedition/lcdui/Display");
                setRef(ctx, g_display);
            }
            static void d_getCurrent(NativeContext *ctx)
            {
                setRef(ctx, g_current);
            }
            static void d_isColor(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, 1);
            }
            static void d_numColors(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, 65536);
            }
            static void d_numAlpha(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, 256);
            }
            static void d_flashBacklight(NativeContext *ctx) { (void)ctx; }
            static void d_repaint(NativeContext *ctx)
            {
                (void)ctx;
                g_paintRequested = true;
            }
            static void d_callSerially(NativeContext *ctx) { (void)ctx; }

            // ---------------------------------------------------------------------
            // MIDlet natives
            // ---------------------------------------------------------------------

            static void mid_init(NativeContext *ctx)
            {
                g_midlet = ctx->thisObj;
            }
            static void mid_getAppProperty(NativeContext *ctx)
            {
                const std::string &key = (argRef(ctx, 1) && argRef(ctx, 1)->kind == ObjKind::String) ? argRef(ctx, 1)->str : "";
                for (const auto &kv : g_appProps)
                    if (kv.first == key)
                    {
                        setRef(ctx, g_rt->heap().newString(kv.second));
                        return;
                    }
                if (jvm::jmeDebug())
                    fprintf(stderr, "[midp] getAppProperty(\"%s\") absent\n", key.c_str());
                // Propriété absente : `null` (spec MIDP) -- sauf pour les jeux qui l'enchaînent sans test de
                // null : les Gameloft lisent des attributs du .jad (« HAS-BLOOD », « Blood-Censor »...) que
                // le .jar seul n'a pas et font `.equals("yes")` : `null` les fait planter (NPE dès le
                // démarrage d'AC III), une chaîne vide les fait prendre la branche « non ». À l'inverse
                // nmania (`Commit`) fait `.charAt(0)` sur la valeur sans test d'égalité et ne tolère PAS la
                // chaîne vide. Règle : chaîne vide si l'éditeur est Gameloft ou si JME_PROP_EMPTY=1
                // (`.conf`), `null` sinon. `PROP:Nom=valeur` dans <jeu>.conf fournit la vraie valeur.
                static const bool forceEmpty = getenv("JME_PROP_EMPTY") && atoi(getenv("JME_PROP_EMPTY")) != 0;
                std::string vendor;
                for (const auto &kv : g_appProps)
                    if (kv.first == "MIDlet-Vendor")
                        vendor = kv.second;
                for (char &c : vendor)
                    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                if (forceEmpty || vendor.find("gameloft") != std::string::npos)
                    setRef(ctx, g_rt->heap().newString(""));
                else
                    setRef(ctx, nullptr);
            }
            static void mid_notifyDestroyed(NativeContext *ctx)
            {
                (void)ctx;
                if (jvm::jmeDebug())
                    fprintf(stderr, "MIDlet.notifyDestroyed() appele\n");
                g_destroyed = true;
            }
            static void mid_notifyPaused(NativeContext *ctx) { (void)ctx; }
            static void mid_resumeRequest(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, 0);
            }

            // ---------------------------------------------------------------------
            // Stubs pour les UI ponctuelles
            // ---------------------------------------------------------------------

            static void ui_noop(NativeContext *ctx) { (void)ctx; }
            static void ui_true(NativeContext *ctx)
            {
                (void)ctx;
                setInt(ctx, 1);
            }
            void registerUiNatives()
            {
                regN("com/nokia/mid/ui/DeviceControl.setLights:(II)V", ui_noop);
                regN("java/io/PrintStream.<init>:(Ljava/io/OutputStream;)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.getWidth:()I", cv_getWidth);
                regN("javax/microedition/lcdui/Canvas.getHeight:()I", cv_getHeight);
                regN("javax/microedition/lcdui/Canvas.isDoubleBuffered:()Z", cv_isDoubleBuffered);
                regN("javax/microedition/lcdui/Canvas.hasPointerEvents:()Z", cv_isDoubleBuffered); // toujours vrai : souris/tactile
                regN("javax/microedition/lcdui/Canvas.hasPointerMotionEvents:()Z", cv_isDoubleBuffered);
                regN("javax/microedition/lcdui/Canvas.hasRepeatEvents:()Z", cv_isDoubleBuffered);
                regN("javax/microedition/lcdui/Canvas.repaint:()V", cv_repaint);
                regN("javax/microedition/lcdui/Canvas.repaint:(IIII)V", cv_repaintRegion);
                regN("javax/microedition/lcdui/Canvas.serviceRepaints:()V", cv_service);
                regN("javax/microedition/lcdui/Canvas.showNotify:()V", cv_showNotify);
                regN("javax/microedition/lcdui/Canvas.hideNotify:()V", cv_hideNotify);
                regN("javax/microedition/lcdui/Canvas.getGameAction:(I)I", cv_getGameAction);
                regN("javax/microedition/lcdui/Canvas.getKeyCode:(I)I", cv_getKeyCode);
                regN("javax/microedition/lcdui/Canvas.getKeyName:(I)Ljava/lang/String;", cv_getKeyName);
                regN("javax/microedition/lcdui/Display.vibrate:(I)Z", ui_vibrate);
                regN("javax/microedition/lcdui/Display.getColor:(I)I", ui_displayColor);
                regN("javax/microedition/midlet/MIDlet.platformRequest:(Ljava/lang/String;)Z", ui_platformRequest);
                regN("javax/microedition/midlet/MIDlet.checkPermission:(Ljava/lang/String;)I", ui_checkPermission);
                regN("javax/microedition/lcdui/Canvas.keyPressed:(I)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.keyReleased:(I)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.keyRepeated:(I)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.pointerPressed:(II)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.pointerReleased:(II)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.pointerDragged:(II)V", ui_noop);
                regN("javax/microedition/lcdui/Display.getDisplay:(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;", d_getDisplay);
                regN("javax/microedition/lcdui/Display.getCurrent:()Ljavax/microedition/lcdui/Displayable;", d_getCurrent);
                regN("javax/microedition/lcdui/Display.isColor:()Z", d_isColor);
                regN("javax/microedition/lcdui/Display.numColors:()I", d_numColors);
                regN("javax/microedition/lcdui/Display.numAlphaLevels:()I", d_numAlpha);
                regN("javax/microedition/lcdui/Display.flashBacklight:(I)V", d_flashBacklight);
                regN("javax/microedition/lcdui/Display.repaint:()V", d_repaint);
                regN("javax/microedition/lcdui/Display.callSerially:(Ljava/lang/Runnable;)V", d_callSerially);
                regN("javax/microedition/lcdui/Display.getBorderlessW:()I", ui_true);
                regN("javax/microedition/lcdui/Display.isFullscreen:()Z", ui_true);
                regN("javax/microedition/midlet/MIDlet.<init>:()V", mid_init);
                regN("javax/microedition/midlet/MIDlet.getAppProperty:(Ljava/lang/String;)Ljava/lang/String;", mid_getAppProperty);
                regN("javax/microedition/midlet/MIDlet.notifyDestroyed:()V", mid_notifyDestroyed);
                regN("javax/microedition/midlet/MIDlet.notifyPaused:()V", mid_notifyPaused);
                regN("javax/microedition/midlet/MIDlet.resumeRequest:()Z", mid_resumeRequest);
            }
        } // namespace detail
    } // namespace midp
} // namespace jvm
