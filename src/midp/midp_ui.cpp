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
            static void d_setCurrent(NativeContext *ctx)
            {
                Obj *d = argRef(ctx, 1);
                if (d && d->kind == ObjKind::Instance)
                {
                    if (jvm::jmeDebug())
                        fprintf(stderr, "DISPLAY.setCurrent(%s)\n", d->cls ? d->cls->name.c_str() : "?");
                    if (g_current != d)
                    {
                        g_current = d;
                        g_paintRequested = true;
                        for (int i = 0; i < screenW() * screenH(); i++)
                            g_canvas565[i] = 0;
                        if (auto *fb = hal::display_get_framebuffer())
                            for (int i = 0; i < fb->width * fb->height; i++)
                                fb->pixels[i] = 0;
                    }
                }
            }
            static void d_setCurrentAlert(NativeContext *ctx)
            {
                Obj *alert = argRef(ctx, 1);
                Obj *next = argRef(ctx, 2);
                g_current = (alert && alert->kind == ObjKind::Instance) ? alert : next;
                g_paintRequested = true;
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
            static void d_setCurrentItem(NativeContext *ctx) { (void)ctx; }

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
                // Propriété absente : chaîne VIDE (et non null comme le veut la spec
                // MIDP). Les jeux Gameloft lisent des attributs du .jad (ex. "HAS-BLOOD")
                // qu'un .jar seul n'a pas, et enchaînent `.equals("yes")` sans test
                // de null : renvoyer null les fait planter (NPE dès le démarrage d'AC III),
                // une chaîne vide les fait simplement prendre la branche "non".
                // `PROP:Nom=valeur` dans <jeu>.conf permet de fournir la vraie valeur.
                for (const auto &kv : g_appProps)
                    if (kv.first == key)
                    {
                        setRef(ctx, g_rt->heap().newString(kv.second));
                        return;
                    }
                if (jvm::jmeDebug())
                    fprintf(stderr, "[midp] getAppProperty(\"%s\") absent -> \"\"\n", key.c_str());
                setRef(ctx, g_rt->heap().newString(""));
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
            static std::vector<JmeUi> g_ui;
            static std::vector<std::pair<Obj *, Obj *>> g_cmdLabels; // Command -> label String
            Obj *g_listSelectCommand = nullptr;                      // bootstrap SELECT_COMMAND

            JmeUi *uiFind(Obj *disp)
            {
                for (auto &u : g_ui)
                    if (u.disp == disp)
                        return &u;
                return nullptr;
            }
            JmeUi *uiFor(Obj *disp)
            {
                if (JmeUi *u = uiFind(disp))
                    return u;
                g_ui.push_back({});
                JmeUi *u = &g_ui.back();
                u->disp = disp;
                u->sel = 0;
                return u;
            }
            void uiCmdSetLabel(Obj *cmd, Obj *label)
            {
                if (!cmd)
                    return;
                for (auto &kv : g_cmdLabels)
                    if (kv.first == cmd)
                    {
                        kv.second = label;
                        return;
                    }
                g_cmdLabels.push_back({cmd, label});
            }
            static Obj *uiCmdLabel(Obj *cmd)
            {
                for (auto &kv : g_cmdLabels)
                    if (kv.first == cmd)
                        return kv.second;
                return nullptr;
            }
            static void uiItemsFromArray(JmeUi *u, Obj *arr)
            {
                if (!arr || arr->kind != ObjKind::ObjArray)
                    return;
                for (int i = 0; i < arr->arrayLen; i++)
                    u->items.push_back(arr->cells[i].o);
            }
            static const char *uiString(Obj *s)
            {
                return (s && s->kind == ObjKind::String) ? s->str.c_str() : "";
            }

            // ----- Displayable ---------------------------------------------------
            static void ui_disp_setTitle(NativeContext *ctx)
            {
                uiFor(ctx->thisObj)->title = argRef(ctx, 1);
            }
            static void ui_disp_getTitle(NativeContext *ctx)
            {
                setRef(ctx, uiFor(ctx->thisObj)->title);
            }
            static void ui_disp_addCommand(NativeContext *ctx)
            {
                JmeUi *u = uiFor(ctx->thisObj);
                Obj *c = argRef(ctx, 1);
                if (c)
                    u->commands.push_back(c);
            }
            static void ui_disp_setCommandListener(NativeContext *ctx)
            {
                uiFor(ctx->thisObj)->listener = argRef(ctx, 1);
            }

            // ----- Command -------------------------------------------------------
            static void ui_cmd_init3(NativeContext *ctx) { uiCmdSetLabel(ctx->thisObj, argRef(ctx, 1)); }
            static void ui_cmd_init4(NativeContext *ctx) { uiCmdSetLabel(ctx->thisObj, argRef(ctx, 1)); }
            static void ui_cmd_getLabel(NativeContext *ctx) { setRef(ctx, uiCmdLabel(ctx->thisObj)); }

            // ----- List ----------------------------------------------------------
            static void ui_list_init2(NativeContext *ctx)
            {
                JmeUi *u = uiFor(ctx->thisObj);
                u->isList = true;
                u->title = argRef(ctx, 1);
                u->type = argInt(ctx, 2);
                u->items.clear();
                u->selectCmd = nullptr;
                u->sel = 0;
            }
            static void ui_list_init4(NativeContext *ctx)
            {
                ui_list_init2(ctx);
                JmeUi *u = uiFind(ctx->thisObj);
                if (u)
                    uiItemsFromArray(u, argRef(ctx, 3));
            }
            static void ui_list_append(NativeContext *ctx)
            {
                JmeUi *u = uiFor(ctx->thisObj);
                u->items.push_back(argRef(ctx, 1));
                setInt(ctx, static_cast<int32_t>(u->items.size()) - 1);
            }
            static void ui_list_getSel(NativeContext *ctx)
            {
                JmeUi *u = uiFind(ctx->thisObj);
                setInt(ctx, u ? u->sel : 0);
            }
            static void ui_list_setSel(NativeContext *ctx)
            {
                JmeUi *u = uiFor(ctx->thisObj);
                int v = argInt(ctx, 1);
                u->sel = v < 0 ? 0 : v;
            }
            static void ui_list_getString(NativeContext *ctx)
            {
                JmeUi *u = uiFind(ctx->thisObj);
                int i = argInt(ctx, 1);
                setRef(ctx, (u && i >= 0 && i < static_cast<int>(u->items.size())) ? u->items[i] : nullptr);
            }
            static void ui_list_setSelectCmd(NativeContext *ctx)
            {
                uiFor(ctx->thisObj)->selectCmd = argRef(ctx, 1);
            }
            static void ui_list_size(NativeContext *ctx)
            {
                JmeUi *u = uiFind(ctx->thisObj);
                setInt(ctx, u ? static_cast<int32_t>(u->items.size()) : 0);
            }

            // ----- Form -----------------------------------------------------------
            static void ui_form_init(NativeContext *ctx)
            {
                JmeUi *u = uiFor(ctx->thisObj);
                u->isList = false;
                u->title = argRef(ctx, 1);
                u->items.clear();
                u->sel = 0;
            }
            static void ui_form_append(NativeContext *ctx)
            {
                JmeUi *u = uiFor(ctx->thisObj);
                u->items.push_back(argRef(ctx, 1));
                setInt(ctx, static_cast<int32_t>(u->items.size()) - 1);
            }
            static void ui_form_size(NativeContext *ctx)
            {
                JmeUi *u = uiFind(ctx->thisObj);
                setInt(ctx, u ? static_cast<int32_t>(u->items.size()) : 0);
            }

            // ----- Dispatch commandAction(Command, Displayable) sur le listener ----
            void uiDispatchCommand(Obj *cmd, Obj *disp)
            {
                if (!cmd || !disp || !g_interp)
                    return;
                JmeUi *u = uiFind(disp);
                if (!u || !u->listener)
                    return;
                Value args[3];
                args[0] = Value::fromRef(u->listener);
                args[1] = Value::fromRef(cmd);
                args[2] = Value::fromRef(disp);
                Value res;
                if (jvm::jmeDebug())
                {
                    Obj *lab = uiCmdLabel(cmd);
                    fprintf(stderr, "UI commandAction cmd=%s disp=%s\n",
                            lab ? uiString(lab) : "?",
                            disp->cls ? disp->cls->name.c_str() : "?");
                }
                g_interp->invokeVirtual(u->listener->cls, "commandAction",
                                        "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V",
                                        u->listener, args, 3, res);
            }

            // ----- Rendu List/Form dans le framebuffer (RGB565, hal fb) ----------
            static void uiRenderBar(int x, int y, int w, int h, uint16_t color)
            {
                auto *fb = hal::display_get_framebuffer();
                if (!fb || !fb->pixels)
                    return;
                for (int r = y; r < y + h; r++)
                {
                    if (r < 0 || r >= fb->height)
                        continue;
                    for (int c = x; c < x + w; c++)
                    {
                        if (c < 0 || c >= fb->width)
                            continue;
                        fb->pixels[r * fb->stride + c] = color;
                    }
                }
            }
            void uiRenderScreen()
            {
                if (!g_current || g_current->kind != ObjKind::Instance)
                    return;
                JmeUi *u = uiFind(g_current);
                if (!u)
                    return;
                auto *fb = hal::display_get_framebuffer();
                if (!fb || !fb->pixels)
                    return;
                const int W = fb->width, H = fb->height;
                // Fond noir, titre blanc, items avec barre de sélection inversée.
                for (int i = 0; i < W * H; i++)
                    fb->pixels[i] = 0x0000;
                int y = 4;
                if (u->title)
                {
                    const char *t = uiString(u->title);
                    hal::display_draw_text(4, y, t, 0xFFFF);
                    y += 12;
                }
                if (u->isList)
                {
                    const int n = static_cast<int>(u->items.size());
                    if (u->sel >= n)
                        u->sel = n > 0 ? n - 1 : 0;
                    for (int i = 0; i < n; i++)
                    {
                        const char *t = uiString(u->items[i]);
                        if (i == u->sel)
                        {
                            uiRenderBar(2, y - 1, W - 4, 9, 0x7BEF);
                            hal::display_draw_text(4, y, t, 0x0000);
                        }
                        else
                        {
                            hal::display_draw_text(4, y, t, 0xFFFF);
                        }
                        y += 10;
                    }
                }
                else
                {
                    for (Obj *item : u->items)
                    {
                        if (item && item->kind == ObjKind::String)
                        {
                            const char *t = uiString(item);
                            hal::display_draw_text(4, y, t, 0xFFFF);
                            y += 10;
                        }
                        else if (item && item->kind == ObjKind::Instance)
                        {
                            // Item : tente getLabel() via notre registre Command --
                            // hors sujet ici, on saute proprement.
                            y += 10;
                        }
                    }
                }
                // Barre softkeys : 1re commande à gauche, dernière à droite.
                if (!u->commands.empty())
                {
                    const char *sl = uiString(uiCmdLabel(u->commands.front()));
                    const char *sr = uiString(uiCmdLabel(u->commands.back()));
                    hal::display_draw_text(2, H - 9, sl, 0xFFFF);
                    hal::display_draw_text(W - 2 - static_cast<int>(strlen(sr)) * 6, H - 9, sr, 0xFFFF);
                }
            }
            void registerUiNatives()
            {
                regN("com/nokia/mid/ui/DeviceControl.setLights:(II)V", ui_noop);
                regN("java/io/PrintStream.<init>:(Ljava/io/OutputStream;)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.getWidth:()I", cv_getWidth);
                regN("javax/microedition/lcdui/Canvas.getHeight:()I", cv_getHeight);
                regN("javax/microedition/lcdui/Canvas.isDoubleBuffered:()Z", cv_isDoubleBuffered);
                regN("javax/microedition/lcdui/Canvas.repaint:()V", cv_repaint);
                regN("javax/microedition/lcdui/Canvas.repaint:(IIII)V", cv_repaintRegion);
                regN("javax/microedition/lcdui/Canvas.serviceRepaints:()V", cv_service);
                regN("javax/microedition/lcdui/Canvas.showNotify:()V", cv_showNotify);
                regN("javax/microedition/lcdui/Canvas.hideNotify:()V", cv_hideNotify);
                regN("javax/microedition/lcdui/Canvas.getGameAction:(I)I", cv_getGameAction);
                regN("javax/microedition/lcdui/Canvas.getKeyCode:(I)I", cv_getKeyCode);
                regN("javax/microedition/lcdui/Canvas.keyPressed:(I)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.keyReleased:(I)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.keyRepeated:(I)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.pointerPressed:(II)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.pointerReleased:(II)V", ui_noop);
                regN("javax/microedition/lcdui/Canvas.pointerDragged:(II)V", ui_noop);
                regN("javax/microedition/lcdui/Display.getDisplay:(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;", d_getDisplay);
                regN("javax/microedition/lcdui/Display.getCurrent:()Ljavax/microedition/lcdui/Displayable;", d_getCurrent);
                regN("javax/microedition/lcdui/Display.setCurrent:(Ljavax/microedition/lcdui/Displayable;)V", d_setCurrent);
                regN("javax/microedition/lcdui/Display.setCurrent:(Ljavax/microedition/lcdui/Alert;Ljavax/microedition/lcdui/Displayable;)V", d_setCurrentAlert);
                regN("javax/microedition/lcdui/Display.setCurrentItem:(Ljavax/microedition/lcdui/Item;)V", d_setCurrentItem);
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
                regN("javax/microedition/lcdui/Alert.<init>:(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;)V", ui_noop);
                regN("javax/microedition/lcdui/Alert.<init>:(Ljava/lang/String;)V", ui_noop);
                regN("javax/microedition/lcdui/Alert.setTimeout:(I)V", ui_noop);
                regN("javax/microedition/lcdui/Alert.setString:(Ljava/lang/String;)V", ui_noop);
                regN("javax/microedition/lcdui/AlertType.<init>:()V", ui_noop);
                regN("javax/microedition/lcdui/Command.<init>:(Ljava/lang/String;II)V", ui_cmd_init3);
                regN("javax/microedition/lcdui/Command.<init>:(Ljava/lang/String;Ljava/lang/String;II)V", ui_cmd_init4);
                regN("javax/microedition/lcdui/Command.getLabel:()Ljava/lang/String;", ui_cmd_getLabel);
                regN("javax/microedition/lcdui/Item.getLabel:()Ljava/lang/String;", ui_noop);
                regN("javax/microedition/lcdui/Item.setLabel:(Ljava/lang/String;)V", ui_noop);
                regN("javax/microedition/lcdui/Form.<init>:(Ljava/lang/String;)V", ui_form_init);
                regN("javax/microedition/lcdui/Form.<init>:(Ljava/lang/String;Ljavax/microedition/lcdui/Item;)V", ui_form_init);
                regN("javax/microedition/lcdui/Form.append:(Ljavax/microedition/lcdui/Item;)I", ui_form_append);
                regN("javax/microedition/lcdui/Form.append:(Ljava/lang/String;)I", ui_form_append);
                regN("javax/microedition/lcdui/Form.size:()I", ui_form_size);
                regN("javax/microedition/lcdui/Form.set:(ILjavax/microedition/lcdui/Item;)V", ui_noop);
                regN("javax/microedition/lcdui/List.<init>:(Ljava/lang/String;I[Ljava/lang/String;Ljavax/microedition/lcdui/Image;)V", ui_list_init4);
                regN("javax/microedition/lcdui/List.<init>:(Ljava/lang/String;I)V", ui_list_init2);
                regN("javax/microedition/lcdui/List.setSelectedIndex:(IZ)V", ui_list_setSel);
                regN("javax/microedition/lcdui/List.setSelectCommand:(Ljavax/microedition/lcdui/Command;)V", ui_list_setSelectCmd);
                regN("javax/microedition/lcdui/List.append:(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I", ui_list_append);
                regN("javax/microedition/lcdui/List.getSelectedIndex:()I", ui_list_getSel);
                regN("javax/microedition/lcdui/List.size:()I", ui_list_size);
                regN("javax/microedition/lcdui/List.getString:(I)Ljava/lang/String;", ui_list_getString);
                regN("javax/microedition/lcdui/TextBox.<init>:(Ljava/lang/String;Ljava/lang/String;II)V", ui_noop);
                regN("javax/microedition/lcdui/TextBox.setString:(Ljava/lang/String;)V", ui_noop);
                regN("javax/microedition/lcdui/TextBox.getString:()Ljava/lang/String;", ui_noop);
                regN("javax/microedition/lcdui/TextField.<init>:(Ljava/lang/String;Ljava/lang/String;II)V", ui_noop);
                regN("javax/microedition/lcdui/TextField.setString:(Ljava/lang/String;)V", ui_noop);
                regN("javax/microedition/lcdui/TextField.getString:()Ljava/lang/String;", ui_noop);
                regN("javax/microedition/lcdui/StringItem.<init>:(Ljava/lang/String;Ljava/lang/String;)V", ui_noop);
                regN("javax/microedition/lcdui/StringItem.setText:(Ljava/lang/String;)V", ui_noop);
                regN("javax/microedition/lcdui/StringItem.getText:()Ljava/lang/String;", ui_noop);
                regN("javax/microedition/lcdui/ImageItem.<init>:(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;)V", ui_noop);
                regN("javax/microedition/lcdui/Gauge.<init>:(Ljava/lang/String;ZII)V", ui_noop);
                regN("javax/microedition/lcdui/Gauge.setValue:(I)V", ui_noop);
                regN("javax/microedition/lcdui/Gauge.getValue:()I", ui_noop);
                regN("javax/microedition/lcdui/ChoiceGroup.<init>:(Ljava/lang/String;I[Ljava/lang/String;Ljavax/microedition/lcdui/Image;)V", ui_noop);
                regN("javax/microedition/lcdui/ChoiceGroup.append:(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I", ui_noop);
                regN("javax/microedition/lcdui/ChoiceGroup.getSelectedIndex:()I", ui_noop);
                regN("javax/microedition/lcdui/ChoiceGroup.size:()I", ui_noop);
                regN("javax/microedition/lcdui/Ticker.<init>:(Ljava/lang/String;)V", ui_noop);
                regN("javax/microedition/lcdui/Displayable.setTitle:(Ljava/lang/String;)V", ui_disp_setTitle);
                regN("javax/microedition/lcdui/Displayable.getTitle:()Ljava/lang/String;", ui_disp_getTitle);
                regN("javax/microedition/lcdui/Displayable.addCommand:(Ljavax/microedition/lcdui/Command;)V", ui_disp_addCommand);
                regN("javax/microedition/lcdui/Displayable.setCommandListener:(Ljavax/microedition/lcdui/CommandListener;)V", ui_disp_setCommandListener);
                regN("javax/microedition/lcdui/Displayable.isShown:()Z", ui_true);
            }
        } // namespace detail
    } // namespace midp
} // namespace jvm
