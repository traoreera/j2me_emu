// midp_lcdui.cpp -- interface utilisateur haut niveau MIDP 2.0 (lcdui) : Command, Alert, List, Form + Items
// (StringItem/ImageItem/TextField/ChoiceGroup/Gauge/Spacer/DateField), TextBox.
//
// Modèle : l'état vit côté hôte, indexé par le pointeur de l'objet Java (le tas est un bump allocator
// sans GC : les objets ne bougent ni ne disparaissent avant reset()). Le rendu se fait directement dans
// le framebuffer HAL (police 5x7 agrandie), la navigation au clavier / à la souris / au texte SDL.
// Les Canvas gardent leur chemin (paint()) ; seuls leurs Commands passent ici (touches écran + barre).

#include "midp/midp_internal.h"
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

namespace jvm
{
    int64_t virtualMillis();
    namespace midp
    {
        namespace detail
        {
            // ------------------------------------------------------------------------------------
            // Constantes MIDP
            // ------------------------------------------------------------------------------------
            enum CmdType { C_SCREEN = 1, C_BACK = 2, C_CANCEL = 3, C_OK = 4, C_HELP = 5, C_STOP = 6, C_EXIT = 7, C_ITEM = 8 };
            enum ChoiceType { CH_EXCLUSIVE = 1, CH_MULTIPLE = 2, CH_IMPLICIT = 3, CH_POPUP = 4 };
            enum ItemKind { IK_STRING, IK_IMAGE, IK_TEXT, IK_CHOICE, IK_GAUGE, IK_DATE, IK_SPACER, IK_CUSTOM };
            enum ScrKind { SK_NONE, SK_FORM, SK_LIST, SK_ALERT, SK_TEXTBOX };
            constexpr int kAlertForever = -2;
            constexpr int kAlertDefaultMs = 3000;

            // ------------------------------------------------------------------------------------
            // Modèle
            // ------------------------------------------------------------------------------------
            struct Cmd
            {
                std::string label, longLabel;
                int type = C_SCREEN, prio = 0;
            };
            struct Chc // modèle de Choice (List, ChoiceGroup)
            {
                int type = CH_IMPLICIT;
                std::vector<std::string> txt;
                std::vector<Obj *> img;
                std::vector<char> sel;
                int fit = 0;
                bool popupOpen = false;
                int cursor = 0; // ligne sous le curseur (navigation) -- distinct de la sélection pour EXCLUSIVE/MULTIPLE
                int selectedIndex() const
                {
                    for (size_t i = 0; i < sel.size(); i++)
                        if (sel[i])
                            return static_cast<int>(i);
                    return -1;
                }
                void selectOnly(int i)
                {
                    for (size_t k = 0; k < sel.size(); k++)
                        sel[k] = (static_cast<int>(k) == i);
                }
                int add(const std::string &t, Obj *im, int at = -1)
                {
                    if (at < 0 || at > static_cast<int>(txt.size()))
                        at = static_cast<int>(txt.size());
                    txt.insert(txt.begin() + at, t);
                    img.insert(img.begin() + at, im);
                    sel.insert(sel.begin() + at, 0);
                    if (type != CH_MULTIPLE && txt.size() == 1)
                        sel[0] = 1; // EXCLUSIVE/IMPLICIT/POPUP : premier élément sélectionné par défaut
                    return at;
                }
                void remove(int i)
                {
                    if (i < 0 || i >= static_cast<int>(txt.size()))
                        return;
                    bool was = sel[i];
                    txt.erase(txt.begin() + i);
                    img.erase(img.begin() + i);
                    sel.erase(sel.begin() + i);
                    if (was && type != CH_MULTIPLE && !sel.empty())
                        sel[std::min(i, static_cast<int>(sel.size()) - 1)] = 1;
                    if (cursor >= static_cast<int>(txt.size()))
                        cursor = std::max(0, static_cast<int>(txt.size()) - 1);
                }
                void clear()
                {
                    txt.clear();
                    img.clear();
                    sel.clear();
                    cursor = 0;
                }
            };
            struct It // Item
            {
                int kind = IK_STRING;
                std::string label, text;
                Obj *img = nullptr;
                Obj *owner = nullptr; // Form contenant
                int layout = 0, appearance = 0;
                int constraints = 0, maxSize = 100, caret = 0;
                int gaugeVal = 0, gaugeMax = 0;
                bool interactive = false;
                Chc ch;
                Obj *cmdListener = nullptr, *defCmd = nullptr;
                std::vector<Obj *> cmds;
            };
            struct Scr // Displayable
            {
                int kind = SK_NONE;
                std::string title;
                Obj *listener = nullptr, *ticker = nullptr;
                std::vector<Obj *> cmds;
                // Form
                std::vector<Obj *> items;
                Obj *itemListener = nullptr;
                int focus = 0, scroll = 0;
                Obj *enteredItem = nullptr; // CustomItem pour lequel traverse(NONE) a été appelé
                // List
                Chc ch;
                Obj *selectCmd = nullptr;
                // Alert
                std::string text;
                int timeout = kAlertDefaultMs, alertType = 0;
                Obj *next = nullptr, *prev = nullptr, *indicator = nullptr;
                int64_t shownAt = -1;
                // TextBox
                int constraints = 0, maxSize = 1000, caret = 0;
                // menu de commandes (> 2 commandes)
                bool menuOpen = false;
                int menuSel = 0;
            };

            static std::unordered_map<Obj *, Cmd> g_cmdMap;
            static std::unordered_map<Obj *, It> g_itemMap;
            static std::unordered_map<Obj *, Scr> g_scrMap;
            Obj *g_listSelectCommand = nullptr;
            static Obj *g_alertDismissCommand = nullptr;
            static bool g_uiDirty = true;

            // Texte tapé pendant la trame (SDL_TEXTINPUT) + retours arrière, posés par main via setTextInput().
            static std::string g_textIn;
            static int g_backspaces = 0;

            static const char *cstr(Obj *s) { return (s && s->kind == ObjKind::String) ? s->str.c_str() : ""; }
            static std::string sstr(Obj *s) { return (s && s->kind == ObjKind::String) ? s->str : std::string(); }
            static Obj *newStr(const std::string &s) { return g_rt->heap().newString(s); }

            static Cmd &cmdOf(Obj *c) { return g_cmdMap[c]; }
            static Scr &scrOf(Obj *d)
            {
                Scr &s = g_scrMap[d];
                if (s.kind == SK_NONE)
                {
                    if (isSubclassOf(d, "javax/microedition/lcdui/Form")) s.kind = SK_FORM;
                    else if (isSubclassOf(d, "javax/microedition/lcdui/List")) s.kind = SK_LIST;
                    else if (isSubclassOf(d, "javax/microedition/lcdui/Alert")) s.kind = SK_ALERT;
                    else if (isSubclassOf(d, "javax/microedition/lcdui/TextBox")) s.kind = SK_TEXTBOX;
                }
                return s;
            }
            static It &itemOf(Obj *o) { return g_itemMap[o]; }
            // Choice partagé List / ChoiceGroup
            static Chc *chcOf(Obj *o)
            {
                if (!o)
                    return nullptr;
                if (isSubclassOf(o, "javax/microedition/lcdui/ChoiceGroup"))
                    return &itemOf(o).ch;
                if (isSubclassOf(o, "javax/microedition/lcdui/List"))
                    return &scrOf(o).ch;
                return nullptr;
            }

            bool lcduiIsScreen(Obj *o)
            {
                if (!o || o->kind != ObjKind::Instance)
                    return false;
                return isSubclassOf(o, "javax/microedition/lcdui/Form") || isSubclassOf(o, "javax/microedition/lcdui/List") ||
                       isSubclassOf(o, "javax/microedition/lcdui/Alert") || isSubclassOf(o, "javax/microedition/lcdui/TextBox");
            }
            void lcduiSetTextInput(const char *utf8, int backspaces)
            {
                g_textIn = utf8 ? utf8 : "";
                g_backspaces = backspaces;
            }
            void lcduiMarkDirty() { g_uiDirty = true; }

            // ------------------------------------------------------------------------------------
            // Command
            // ------------------------------------------------------------------------------------
            static void cmd_init3(NativeContext *ctx)
            {
                Cmd &c = cmdOf(ctx->thisObj);
                c.label = sstr(argRef(ctx, 1));
                c.type = argInt(ctx, 2);
                c.prio = argInt(ctx, 3);
            }
            static void cmd_init4(NativeContext *ctx)
            {
                Cmd &c = cmdOf(ctx->thisObj);
                c.label = sstr(argRef(ctx, 1));
                c.longLabel = sstr(argRef(ctx, 2));
                c.type = argInt(ctx, 3);
                c.prio = argInt(ctx, 4);
            }
            static void cmd_getLabel(NativeContext *ctx) { setRef(ctx, newStr(cmdOf(ctx->thisObj).label)); }
            static void cmd_getLongLabel(NativeContext *ctx)
            {
                Cmd &c = cmdOf(ctx->thisObj);
                setRef(ctx, newStr(c.longLabel.empty() ? c.label : c.longLabel));
            }
            static void cmd_getType(NativeContext *ctx) { setInt(ctx, cmdOf(ctx->thisObj).type); }
            static void cmd_getPriority(NativeContext *ctx) { setInt(ctx, cmdOf(ctx->thisObj).prio); }

            void uiCmdSetLabel(Obj *cmd, Obj *label)
            {
                if (cmd)
                    cmdOf(cmd).label = sstr(label);
            }

            // ------------------------------------------------------------------------------------
            // Displayable
            // ------------------------------------------------------------------------------------
            static void disp_setTitle(NativeContext *ctx) { scrOf(ctx->thisObj).title = sstr(argRef(ctx, 1)); g_uiDirty = true; }
            static void disp_getTitle(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                setRef(ctx, s.title.empty() ? nullptr : newStr(s.title));
            }
            static void disp_addCommand(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                Obj *c = argRef(ctx, 1);
                if (c && std::find(s.cmds.begin(), s.cmds.end(), c) == s.cmds.end())
                    s.cmds.push_back(c);
                g_uiDirty = true;
            }
            static void disp_removeCommand(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                s.cmds.erase(std::remove(s.cmds.begin(), s.cmds.end(), argRef(ctx, 1)), s.cmds.end());
                g_uiDirty = true;
            }
            static void disp_setListener(NativeContext *ctx) { scrOf(ctx->thisObj).listener = argRef(ctx, 1); }
            static void disp_setTicker(NativeContext *ctx) { scrOf(ctx->thisObj).ticker = argRef(ctx, 1); }
            static void disp_getTicker(NativeContext *ctx) { setRef(ctx, scrOf(ctx->thisObj).ticker); }
            static void disp_isShown(NativeContext *ctx) { setInt(ctx, ctx->thisObj == g_current ? 1 : 0); }
            static void disp_getWidth(NativeContext *ctx) { setInt(ctx, screenW()); }
            static void disp_getHeight(NativeContext *ctx) { setInt(ctx, screenH()); }
            static void noop(NativeContext *) {}

            // Dispatch commandAction(Command, Displayable) sur le listener de l'écran.
            void uiDispatchCommand(Obj *cmd, Obj *disp)
            {
                if (!cmd || !disp || !g_interp)
                    return;
                Scr &s = scrOf(disp);
                if (!s.listener)
                    return;
                Value args[3] = {Value::fromRef(s.listener), Value::fromRef(cmd), Value::fromRef(disp)}, res;
                if (jvm::jmeDebug())
                    fprintf(stderr, "UI commandAction cmd=%s disp=%s\n", cmdOf(cmd).label.c_str(), disp->cls ? disp->cls->name.c_str() : "?");
                g_interp->invokeVirtual(s.listener->cls, "commandAction",
                                        "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V", s.listener, args, 3, res);
                g_uiDirty = true;
            }
            static void dispatchItemCommand(Obj *cmd, Obj *item)
            {
                It &it = itemOf(item);
                if (!cmd || !it.cmdListener || !g_interp)
                    return;
                Value args[3] = {Value::fromRef(it.cmdListener), Value::fromRef(cmd), Value::fromRef(item)}, res;
                g_interp->invokeVirtual(it.cmdListener->cls, "commandAction",
                                        "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Item;)V", it.cmdListener, args, 3, res);
                g_uiDirty = true;
            }
            static void notifyItemChanged(Obj *form, Obj *item)
            {
                if (!form || !g_interp)
                    return;
                Scr &s = scrOf(form);
                if (!s.itemListener)
                    return;
                Value args[2] = {Value::fromRef(s.itemListener), Value::fromRef(item)}, res;
                g_interp->invokeVirtual(s.itemListener->cls, "itemStateChanged", "(Ljavax/microedition/lcdui/Item;)V", s.itemListener, args, 2, res);
                g_uiDirty = true;
            }

            // ------------------------------------------------------------------------------------
            // Display.setCurrent
            // ------------------------------------------------------------------------------------
            static void enterScreen(Obj *d)
            {
                if (!d)
                    return;
                g_uiDirty = true;
                if (lcduiIsScreen(d))
                {
                    Scr &s = scrOf(d);
                    s.menuOpen = false;
                    if (s.kind == SK_ALERT)
                        s.shownAt = jvm::virtualMillis();
                }
            }
            static void clearFramebuffers()
            {
                if (auto *fb = hal::display_get_framebuffer())
                    for (int i = 0; i < fb->width * fb->height; i++)
                        fb->pixels[i] = 0;
            }
            // Canvas.showNotify()/hideNotify() (MIDP) : appelés quand le Canvas devient / cesse d'être l'écran courant,
            // showNotify() AVANT le premier paint(). Beaucoup de jeux y démarrent/arrêtent leur thread de rendu
            // (BluWar : le thread de rafraîchissement n'est lancé que dans showNotify(), écran figé sans cela).
            static void callNotify(Obj *d, const char *name)
            {
                if (!d || d->kind != ObjKind::Instance || !d->cls || !g_interp)
                    return;
                if (!isSubclassOf(d, "javax/microedition/lcdui/Canvas"))
                    return;
                const MethodRecord *pm = d->cls->findMethodVirtual(name, "()V");
                if (!pm || !pm->mi) // pas de surcharge bytecode : rien à faire
                    return;
                Value args[1] = {Value::fromRef(d)};
                Value res;
                g_interp->invokeVirtual(d->cls, name, "()V", d, args, 1, res);
            }
            // Change le Displayable courant en notifiant l'ancien (hideNotify) puis le nouveau (showNotify).
            static void switchCurrent(Obj *next)
            {
                Obj *old = g_current;
                g_current = next;
                if (old == next)
                    return;
                callNotify(old, "hideNotify");
                if (g_current == next) // hideNotify() peut lui-même avoir changé d'écran
                    callNotify(next, "showNotify");
            }
            static void d_setCurrent(NativeContext *ctx)
            {
                Obj *d = argRef(ctx, 1);
                if (!d || d->kind != ObjKind::Instance)
                    return;
                if (jvm::jmeDebug())
                    fprintf(stderr, "DISPLAY.setCurrent(%s)\n", d->cls ? d->cls->name.c_str() : "?");
                if (g_current != d)
                {
                    if (lcduiIsScreen(d) && scrOf(d).kind == SK_ALERT)
                    {
                        Scr &a = scrOf(d);
                        a.prev = g_current; // sans « next » explicite : retour au displayable précédent
                        a.next = nullptr;
                    }
                    switchCurrent(d);
                    g_paintRequested = true;
                    clearFramebuffers();
                    enterScreen(d);
                }
            }
            static void d_setCurrentAlert(NativeContext *ctx)
            {
                Obj *alert = argRef(ctx, 1), *next = argRef(ctx, 2);
                if (alert && alert->kind == ObjKind::Instance)
                {
                    Scr &a = scrOf(alert);
                    a.next = next;
                    a.prev = g_current == alert ? a.prev : g_current;
                    switchCurrent(alert);
                    clearFramebuffers();
                    enterScreen(alert);
                }
                else
                    switchCurrent(next);
                g_paintRequested = true;
            }
            static void d_setCurrentItem(NativeContext *ctx)
            {
                Obj *item = argRef(ctx, 1);
                if (!item)
                    return;
                It &it = itemOf(item);
                if (it.owner)
                {
                    Scr &f = scrOf(it.owner);
                    for (size_t i = 0; i < f.items.size(); i++)
                        if (f.items[i] == item)
                            f.focus = static_cast<int>(i);
                    switchCurrent(it.owner);
                    g_paintRequested = true;
                    clearFramebuffers();
                    enterScreen(it.owner);
                }
            }

            // ------------------------------------------------------------------------------------
            // Alert
            // ------------------------------------------------------------------------------------
            static void alert_init4(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                s.kind = SK_ALERT;
                s.title = sstr(argRef(ctx, 1));
                s.text = sstr(argRef(ctx, 2));
                s.timeout = kAlertDefaultMs;
                Obj *t = argRef(ctx, 4);
                s.alertType = t ? 1 : 0;
            }
            static void alert_init1(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                s.kind = SK_ALERT;
                s.title = sstr(argRef(ctx, 1));
                s.timeout = kAlertDefaultMs;
            }
            static void alert_setTimeout(NativeContext *ctx) { scrOf(ctx->thisObj).timeout = argInt(ctx, 1); }
            static void alert_getTimeout(NativeContext *ctx) { setInt(ctx, scrOf(ctx->thisObj).timeout); }
            static void alert_setString(NativeContext *ctx) { scrOf(ctx->thisObj).text = sstr(argRef(ctx, 1)); g_uiDirty = true; }
            static void alert_getString(NativeContext *ctx) { setRef(ctx, newStr(scrOf(ctx->thisObj).text)); }
            static void alert_setIndicator(NativeContext *ctx) { scrOf(ctx->thisObj).indicator = argRef(ctx, 1); }
            static void alert_getIndicator(NativeContext *ctx) { setRef(ctx, scrOf(ctx->thisObj).indicator); }

            // ------------------------------------------------------------------------------------
            // Choice (List / ChoiceGroup)
            // ------------------------------------------------------------------------------------
            static void list_init(NativeContext *ctx) // (String, int) et (String, int, String[], Image[]|Image)
            {
                Scr &s = scrOf(ctx->thisObj);
                s.kind = SK_LIST;
                s.title = sstr(argRef(ctx, 1));
                s.ch.clear();
                s.ch.type = argInt(ctx, 2);
                Obj *arr = argRef(ctx, 3), *imgs = argRef(ctx, 4);
                if (arr && arr->kind == ObjKind::ObjArray)
                    for (int i = 0; i < arr->arrayLen; i++)
                    {
                        Obj *im = (imgs && imgs->kind == ObjKind::ObjArray && i < imgs->arrayLen) ? imgs->cells[i].o : nullptr;
                        s.ch.add(sstr(arr->cells[i].o), im);
                    }
            }
            static void cg_init(NativeContext *ctx)
            {
                It &it = itemOf(ctx->thisObj);
                it.kind = IK_CHOICE;
                it.label = sstr(argRef(ctx, 1));
                it.interactive = true;
                it.ch.clear();
                it.ch.type = argInt(ctx, 2);
                Obj *arr = argRef(ctx, 3), *imgs = argRef(ctx, 4);
                if (arr && arr->kind == ObjKind::ObjArray)
                    for (int i = 0; i < arr->arrayLen; i++)
                    {
                        Obj *im = (imgs && imgs->kind == ObjKind::ObjArray && i < imgs->arrayLen) ? imgs->cells[i].o : nullptr;
                        it.ch.add(sstr(arr->cells[i].o), im);
                    }
            }
            static void ch_append(NativeContext *ctx)
            {
                Chc *c = chcOf(ctx->thisObj);
                setInt(ctx, c ? c->add(sstr(argRef(ctx, 1)), argRef(ctx, 2)) : -1);
                g_uiDirty = true;
            }
            static void ch_insert(NativeContext *ctx)
            {
                if (Chc *c = chcOf(ctx->thisObj))
                    c->add(sstr(argRef(ctx, 2)), argRef(ctx, 3), argInt(ctx, 1));
                g_uiDirty = true;
            }
            static void ch_set(NativeContext *ctx)
            {
                Chc *c = chcOf(ctx->thisObj);
                int i = argInt(ctx, 1);
                if (c && i >= 0 && i < static_cast<int>(c->txt.size()))
                {
                    c->txt[i] = sstr(argRef(ctx, 2));
                    c->img[i] = argRef(ctx, 3);
                }
                g_uiDirty = true;
            }
            static void ch_delete(NativeContext *ctx) { if (Chc *c = chcOf(ctx->thisObj)) c->remove(argInt(ctx, 1)); g_uiDirty = true; }
            static void ch_deleteAll(NativeContext *ctx) { if (Chc *c = chcOf(ctx->thisObj)) c->clear(); g_uiDirty = true; }
            static void ch_size(NativeContext *ctx) { Chc *c = chcOf(ctx->thisObj); setInt(ctx, c ? static_cast<int>(c->txt.size()) : 0); }
            static void ch_getString(NativeContext *ctx)
            {
                Chc *c = chcOf(ctx->thisObj);
                int i = argInt(ctx, 1);
                setRef(ctx, (c && i >= 0 && i < static_cast<int>(c->txt.size())) ? newStr(c->txt[i]) : nullptr);
            }
            static void ch_getImage(NativeContext *ctx)
            {
                Chc *c = chcOf(ctx->thisObj);
                int i = argInt(ctx, 1);
                setRef(ctx, (c && i >= 0 && i < static_cast<int>(c->img.size())) ? c->img[i] : nullptr);
            }
            static void ch_isSelected(NativeContext *ctx)
            {
                Chc *c = chcOf(ctx->thisObj);
                int i = argInt(ctx, 1);
                setInt(ctx, (c && i >= 0 && i < static_cast<int>(c->sel.size()) && c->sel[i]) ? 1 : 0);
            }
            static void ch_getSelectedIndex(NativeContext *ctx)
            {
                Chc *c = chcOf(ctx->thisObj);
                setInt(ctx, c ? (c->type == CH_MULTIPLE ? -1 : c->selectedIndex()) : -1);
            }
            static void ch_setSelectedIndex(NativeContext *ctx)
            {
                Chc *c = chcOf(ctx->thisObj);
                int i = argInt(ctx, 1);
                if (!c || i < 0 || i >= static_cast<int>(c->sel.size()))
                    return;
                if (c->type == CH_MULTIPLE)
                    c->sel[i] = argInt(ctx, 2) ? 1 : 0;
                else if (argInt(ctx, 2))
                    c->selectOnly(i);
                c->cursor = i;
                g_uiDirty = true;
            }
            static void ch_getSelectedFlags(NativeContext *ctx)
            {
                Chc *c = chcOf(ctx->thisObj);
                Obj *arr = argRef(ctx, 1);
                int n = 0;
                if (c && arr && arr->cells)
                    for (int i = 0; i < arr->arrayLen; i++)
                    {
                        bool v = i < static_cast<int>(c->sel.size()) && c->sel[i];
                        arr->cells[i] = Value::fromInt(v ? 1 : 0);
                        n += v;
                    }
                setInt(ctx, n);
            }
            static void ch_setSelectedFlags(NativeContext *ctx)
            {
                Chc *c = chcOf(ctx->thisObj);
                Obj *arr = argRef(ctx, 1);
                if (c && arr && arr->cells)
                {
                    for (size_t i = 0; i < c->sel.size(); i++)
                        c->sel[i] = (static_cast<int>(i) < arr->arrayLen && arr->cells[i].i) ? 1 : 0;
                    if (c->type != CH_MULTIPLE)
                    {
                        int first = c->selectedIndex();
                        if (first >= 0)
                            c->selectOnly(first);
                    }
                }
                g_uiDirty = true;
            }
            static void ch_setFit(NativeContext *ctx) { if (Chc *c = chcOf(ctx->thisObj)) c->fit = argInt(ctx, 1); }
            static void ch_getFit(NativeContext *ctx) { Chc *c = chcOf(ctx->thisObj); setInt(ctx, c ? c->fit : 0); }
            static void list_setSelectCommand(NativeContext *ctx) { scrOf(ctx->thisObj).selectCmd = argRef(ctx, 1); }

            // ------------------------------------------------------------------------------------
            // Form + Items
            // ------------------------------------------------------------------------------------
            static void form_init(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                s.kind = SK_FORM;
                s.title = sstr(argRef(ctx, 1));
                s.items.clear();
                Obj *arr = argRef(ctx, 2);
                if (arr && arr->kind == ObjKind::ObjArray)
                    for (int i = 0; i < arr->arrayLen; i++)
                        if (arr->cells[i].o)
                        {
                            s.items.push_back(arr->cells[i].o);
                            itemOf(arr->cells[i].o).owner = ctx->thisObj;
                        }
            }
            // Élément « brut » ajouté par append(String)/append(Image) : encapsulé dans un Item interne.
            static Obj *wrapPlain(Obj *form, Obj *o)
            {
                if (!o)
                    return nullptr;
                if (o->kind == ObjKind::Instance && isSubclassOf(o, "javax/microedition/lcdui/Item"))
                    return o;
                Obj *w = makeInstance(o->kind == ObjKind::String ? "javax/microedition/lcdui/StringItem" : "javax/microedition/lcdui/ImageItem");
                if (!w)
                    return nullptr;
                It &it = itemOf(w);
                if (o->kind == ObjKind::String)
                {
                    it.kind = IK_STRING;
                    it.text = o->str;
                }
                else
                {
                    it.kind = IK_IMAGE;
                    it.img = o;
                }
                it.owner = form;
                return w;
            }
            static void form_append(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                Obj *w = wrapPlain(ctx->thisObj, argRef(ctx, 1));
                if (!w)
                {
                    setInt(ctx, -1);
                    return;
                }
                itemOf(w).owner = ctx->thisObj;
                s.items.push_back(w);
                setInt(ctx, static_cast<int>(s.items.size()) - 1);
                g_uiDirty = true;
            }
            static void form_insert(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                int i = argInt(ctx, 1);
                if (i < 0 || i > static_cast<int>(s.items.size()))
                    i = static_cast<int>(s.items.size());
                if (Obj *w = wrapPlain(ctx->thisObj, argRef(ctx, 2)))
                {
                    itemOf(w).owner = ctx->thisObj;
                    s.items.insert(s.items.begin() + i, w);
                }
                g_uiDirty = true;
            }
            static void form_set(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                int i = argInt(ctx, 1);
                if (Obj *w = wrapPlain(ctx->thisObj, argRef(ctx, 2)))
                    if (i >= 0 && i < static_cast<int>(s.items.size()))
                    {
                        itemOf(w).owner = ctx->thisObj;
                        s.items[i] = w;
                    }
                g_uiDirty = true;
            }
            static void form_delete(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                int i = argInt(ctx, 1);
                if (i >= 0 && i < static_cast<int>(s.items.size()))
                {
                    itemOf(s.items[i]).owner = nullptr;
                    s.items.erase(s.items.begin() + i);
                }
                if (s.focus >= static_cast<int>(s.items.size()))
                    s.focus = std::max(0, static_cast<int>(s.items.size()) - 1);
                g_uiDirty = true;
            }
            static void form_deleteAll(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                for (Obj *o : s.items)
                    itemOf(o).owner = nullptr;
                s.items.clear();
                s.focus = s.scroll = 0;
                g_uiDirty = true;
            }
            static void form_get(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                int i = argInt(ctx, 1);
                setRef(ctx, (i >= 0 && i < static_cast<int>(s.items.size())) ? s.items[i] : nullptr);
            }
            static void form_size(NativeContext *ctx) { setInt(ctx, static_cast<int>(scrOf(ctx->thisObj).items.size())); }
            static void form_setItemStateListener(NativeContext *ctx) { scrOf(ctx->thisObj).itemListener = argRef(ctx, 1); }

            // Item commun
            static void item_getLabel(NativeContext *ctx) { setRef(ctx, itemOf(ctx->thisObj).label.empty() ? nullptr : newStr(itemOf(ctx->thisObj).label)); }
            static void item_setLabel(NativeContext *ctx) { itemOf(ctx->thisObj).label = sstr(argRef(ctx, 1)); g_uiDirty = true; }
            static void item_getLayout(NativeContext *ctx) { setInt(ctx, itemOf(ctx->thisObj).layout); }
            static void item_setLayout(NativeContext *ctx) { itemOf(ctx->thisObj).layout = argInt(ctx, 1); }
            static void item_addCommand(NativeContext *ctx)
            {
                It &it = itemOf(ctx->thisObj);
                if (Obj *c = argRef(ctx, 1))
                    if (std::find(it.cmds.begin(), it.cmds.end(), c) == it.cmds.end())
                        it.cmds.push_back(c);
            }
            static void item_removeCommand(NativeContext *ctx)
            {
                It &it = itemOf(ctx->thisObj);
                it.cmds.erase(std::remove(it.cmds.begin(), it.cmds.end(), argRef(ctx, 1)), it.cmds.end());
            }
            static void item_setDefaultCommand(NativeContext *ctx)
            {
                It &it = itemOf(ctx->thisObj);
                it.defCmd = argRef(ctx, 1);
                if (it.defCmd && std::find(it.cmds.begin(), it.cmds.end(), it.defCmd) == it.cmds.end())
                    it.cmds.push_back(it.defCmd);
            }
            static void item_setCmdListener(NativeContext *ctx) { itemOf(ctx->thisObj).cmdListener = argRef(ctx, 1); }
            static void item_notifyStateChanged(NativeContext *ctx) { notifyItemChanged(itemOf(ctx->thisObj).owner, ctx->thisObj); }

            // StringItem
            static void si_init(NativeContext *ctx)
            {
                It &it = itemOf(ctx->thisObj);
                it.kind = IK_STRING;
                it.label = sstr(argRef(ctx, 1));
                it.text = sstr(argRef(ctx, 2));
                it.appearance = ctx->nargs > 3 ? argInt(ctx, 3) : 0;
            }
            static void si_getText(NativeContext *ctx) { setRef(ctx, newStr(itemOf(ctx->thisObj).text)); }
            static void si_setText(NativeContext *ctx) { itemOf(ctx->thisObj).text = sstr(argRef(ctx, 1)); g_uiDirty = true; }
            static void item_getAppearance(NativeContext *ctx) { setInt(ctx, itemOf(ctx->thisObj).appearance); }
            // ImageItem
            static void ii_init(NativeContext *ctx)
            {
                It &it = itemOf(ctx->thisObj);
                it.kind = IK_IMAGE;
                it.label = sstr(argRef(ctx, 1));
                it.img = argRef(ctx, 2);
                it.layout = argInt(ctx, 3);
                it.text = sstr(argRef(ctx, 4)); // altText
                it.appearance = ctx->nargs > 5 ? argInt(ctx, 5) : 0;
            }
            static void ii_getImage(NativeContext *ctx) { setRef(ctx, itemOf(ctx->thisObj).img); }
            static void ii_setImage(NativeContext *ctx) { itemOf(ctx->thisObj).img = argRef(ctx, 1); g_uiDirty = true; }
            static void ii_getAlt(NativeContext *ctx) { setRef(ctx, newStr(itemOf(ctx->thisObj).text)); }
            static void ii_setAlt(NativeContext *ctx) { itemOf(ctx->thisObj).text = sstr(argRef(ctx, 1)); }
            // TextField / TextBox : (label|title, text, maxSize, constraints)
            static void tf_init(NativeContext *ctx)
            {
                It &it = itemOf(ctx->thisObj);
                it.kind = IK_TEXT;
                it.label = sstr(argRef(ctx, 1));
                it.text = sstr(argRef(ctx, 2));
                it.maxSize = argInt(ctx, 3) > 0 ? argInt(ctx, 3) : 1;
                it.constraints = argInt(ctx, 4);
                it.interactive = true;
                it.caret = static_cast<int>(it.text.size());
            }
            static void tb_init(NativeContext *ctx)
            {
                Scr &s = scrOf(ctx->thisObj);
                s.kind = SK_TEXTBOX;
                s.title = sstr(argRef(ctx, 1));
                s.text = sstr(argRef(ctx, 2));
                s.maxSize = argInt(ctx, 3) > 0 ? argInt(ctx, 3) : 1;
                s.constraints = argInt(ctx, 4);
                s.caret = static_cast<int>(s.text.size());
            }
            // Accès unifié texte : TextField (Item) ou TextBox (Scr)
            struct TextRef
            {
                std::string *text;
                int *maxSize, *constraints, *caret;
            };
            static bool textRef(Obj *o, TextRef &r)
            {
                if (isSubclassOf(o, "javax/microedition/lcdui/TextBox"))
                {
                    Scr &s = scrOf(o);
                    r = {&s.text, &s.maxSize, &s.constraints, &s.caret};
                    return true;
                }
                if (isSubclassOf(o, "javax/microedition/lcdui/TextField"))
                {
                    It &i = itemOf(o);
                    r = {&i.text, &i.maxSize, &i.constraints, &i.caret};
                    return true;
                }
                return false;
            }
            static void tx_getString(NativeContext *ctx)
            {
                TextRef r;
                setRef(ctx, textRef(ctx->thisObj, r) ? newStr(*r.text) : nullptr);
            }
            static void tx_setString(NativeContext *ctx)
            {
                TextRef r;
                if (textRef(ctx->thisObj, r))
                {
                    *r.text = sstr(argRef(ctx, 1));
                    if (static_cast<int>(r.text->size()) > *r.maxSize)
                        r.text->resize(*r.maxSize);
                    *r.caret = static_cast<int>(r.text->size());
                    g_uiDirty = true;
                }
            }
            static void tx_size(NativeContext *ctx) { TextRef r; setInt(ctx, textRef(ctx->thisObj, r) ? static_cast<int>(r.text->size()) : 0); }
            static void tx_getMaxSize(NativeContext *ctx) { TextRef r; setInt(ctx, textRef(ctx->thisObj, r) ? *r.maxSize : 0); }
            static void tx_setMaxSize(NativeContext *ctx)
            {
                TextRef r;
                if (textRef(ctx->thisObj, r))
                {
                    *r.maxSize = std::max(1, argInt(ctx, 1));
                    if (static_cast<int>(r.text->size()) > *r.maxSize)
                        r.text->resize(*r.maxSize);
                    setInt(ctx, *r.maxSize);
                }
            }
            static void tx_getConstraints(NativeContext *ctx) { TextRef r; setInt(ctx, textRef(ctx->thisObj, r) ? *r.constraints : 0); }
            static void tx_setConstraints(NativeContext *ctx) { TextRef r; if (textRef(ctx->thisObj, r)) *r.constraints = argInt(ctx, 1); }
            static void tx_getCaret(NativeContext *ctx) { TextRef r; setInt(ctx, textRef(ctx->thisObj, r) ? *r.caret : 0); }
            static void tx_insert(NativeContext *ctx) // insert(String, int position)
            {
                TextRef r;
                if (!textRef(ctx->thisObj, r))
                    return;
                int pos = std::max(0, std::min(argInt(ctx, 2), static_cast<int>(r.text->size())));
                r.text->insert(pos, sstr(argRef(ctx, 1)));
                if (static_cast<int>(r.text->size()) > *r.maxSize)
                    r.text->resize(*r.maxSize);
                g_uiDirty = true;
            }
            static void tx_delete(NativeContext *ctx) // delete(int offset, int length)
            {
                TextRef r;
                if (!textRef(ctx->thisObj, r))
                    return;
                int off = std::max(0, std::min(argInt(ctx, 1), static_cast<int>(r.text->size())));
                r.text->erase(off, std::max(0, argInt(ctx, 2)));
                *r.caret = std::min(*r.caret, static_cast<int>(r.text->size()));
                g_uiDirty = true;
            }
            static void tx_getChars(NativeContext *ctx)
            {
                TextRef r;
                Obj *arr = argRef(ctx, 1);
                if (!textRef(ctx->thisObj, r) || !arr || !arr->cells)
                {
                    setInt(ctx, 0);
                    return;
                }
                int n = std::min(static_cast<int>(r.text->size()), arr->arrayLen);
                for (int i = 0; i < n; i++)
                    arr->cells[i] = Value::fromInt(static_cast<unsigned char>((*r.text)[i]));
                setInt(ctx, n);
            }
            static void tx_setChars(NativeContext *ctx) // setChars(char[], offset, length)
            {
                TextRef r;
                Obj *arr = argRef(ctx, 1);
                if (!textRef(ctx->thisObj, r) || !arr || !arr->cells)
                    return;
                int off = argInt(ctx, 2), len = argInt(ctx, 3);
                std::string s;
                for (int i = off; i < off + len && i < arr->arrayLen; i++)
                    if (i >= 0)
                        s += static_cast<char>(arr->cells[i].i & 0xFF);
                *r.text = s.substr(0, *r.maxSize);
                *r.caret = static_cast<int>(r.text->size());
                g_uiDirty = true;
            }
            // Gauge
            static void ga_init(NativeContext *ctx) // (label, interactive, maxValue, initialValue)
            {
                It &it = itemOf(ctx->thisObj);
                it.kind = IK_GAUGE;
                it.label = sstr(argRef(ctx, 1));
                it.interactive = argInt(ctx, 2) != 0;
                it.gaugeMax = argInt(ctx, 3);
                it.gaugeVal = argInt(ctx, 4);
            }
            static void ga_getValue(NativeContext *ctx) { setInt(ctx, itemOf(ctx->thisObj).gaugeVal); }
            static void ga_setValue(NativeContext *ctx)
            {
                It &it = itemOf(ctx->thisObj);
                int v = argInt(ctx, 1);
                if (it.gaugeMax > 0)
                    v = std::max(0, std::min(v, it.gaugeMax));
                it.gaugeVal = v;
                g_uiDirty = true;
            }
            static void ga_getMax(NativeContext *ctx) { setInt(ctx, itemOf(ctx->thisObj).gaugeMax); }
            static void ga_setMax(NativeContext *ctx) { itemOf(ctx->thisObj).gaugeMax = argInt(ctx, 1); g_uiDirty = true; }
            static void ga_isInteractive(NativeContext *ctx) { setInt(ctx, itemOf(ctx->thisObj).interactive ? 1 : 0); }
            // CustomItem : le jeu fournit paint()/getPref*/traverse()/keyPressed()... ; ici le socle natif.
            static void ci_init(NativeContext *ctx)
            {
                It &it = itemOf(ctx->thisObj);
                it.kind = IK_CUSTOM;
                it.label = sstr(argRef(ctx, 1));
                it.interactive = true;
            }
            // TRAVERSE_HORIZONTAL|VERTICAL (1|2) + KEY_PRESS|RELEASE|REPEAT (4|8|16) + POINTER_PRESS|RELEASE|DRAG (32|64|128)
            static void ci_interactionModes(NativeContext *ctx) { setInt(ctx, 0xFF); }
            static void ci_repaint(NativeContext *) { g_uiDirty = true; }
            static void date_init(NativeContext *ctx) { It &it = itemOf(ctx->thisObj); it.kind = IK_DATE; it.label = sstr(argRef(ctx, 1)); }
            static void spacer_init(NativeContext *ctx) { itemOf(ctx->thisObj).kind = IK_SPACER; }

            // ------------------------------------------------------------------------------------
            // Rendu
            // ------------------------------------------------------------------------------------
            static uint16_t rgb(int r, int g, int b) { return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
            namespace col
            {
                static const uint16_t bg = rgb(244, 245, 250), fg = rgb(20, 22, 32), titleBg = rgb(32, 58, 116), titleFg = rgb(255, 255, 255),
                                      selBg = rgb(60, 110, 200), selFg = rgb(255, 255, 255), barBg = rgb(30, 34, 52), barFg = rgb(225, 228, 240),
                                      dim = rgb(110, 116, 140), box = rgb(255, 255, 255), border = rgb(90, 96, 120), alertBg = rgb(255, 250, 220),
                                      shade = rgb(60, 60, 70);
            }
            enum HitKind { H_LIST_ROW, H_ITEM, H_CHOICE_ROW, H_SOFT_L, H_SOFT_R, H_ALERT, H_MENU_ROW, H_CUSTOM };
            struct Hit
            {
                int x, y, w, h, kind, idx, sub;
            };
            static std::vector<Hit> g_hits;
            static int g_S = 1;

            static int lineH() { return 9 * g_S; }
            static int charW() { return 6 * g_S; }
            static void rect(int x, int y, int w, int h, uint16_t c) { hal::display_fill_rect(x, y, w, h, c); }
            static void text(int x, int y, const std::string &s, uint16_t c) { hal::display_draw_text_scaled(x, y, s.c_str(), c, g_S); }
            static std::string clip(const std::string &s, int maxChars)
            {
                if (maxChars < 1)
                    return "";
                if (static_cast<int>(s.size()) <= maxChars)
                    return s;
                if (maxChars <= 2)
                    return s.substr(0, maxChars);
                return s.substr(0, maxChars - 2) + "..";
            }
            static std::vector<std::string> wrap(const std::string &s, int maxChars)
            {
                std::vector<std::string> out;
                if (maxChars < 1)
                    return out;
                std::string cur, word;
                auto flushWord = [&]() {
                    while (!word.empty())
                    {
                        int room = maxChars - static_cast<int>(cur.size()) - (cur.empty() ? 0 : 1);
                        if (static_cast<int>(word.size()) <= room)
                        {
                            cur += (cur.empty() ? "" : " ") + word;
                            word.clear();
                        }
                        else if (!cur.empty())
                        {
                            out.push_back(cur);
                            cur.clear();
                        }
                        else
                        {
                            out.push_back(word.substr(0, maxChars));
                            word.erase(0, maxChars);
                        }
                    }
                };
                for (char ch : s)
                {
                    if (ch == '\n')
                    {
                        flushWord();
                        out.push_back(cur);
                        cur.clear();
                    }
                    else if (ch == ' ')
                        flushWord();
                    else
                        word += ch;
                }
                flushWord();
                if (!cur.empty() || out.empty())
                    out.push_back(cur);
                return out;
            }
            // Blit d'une Image MIDP (ARGB) dans le framebuffer, limitée à maxW x maxH.
            static void blitImage(Obj *img, int x, int y, int maxW, int maxH)
            {
                auto *fb = hal::display_get_framebuffer();
                if (!img || img->kind != ObjKind::Instance || !fb || !fb->pixels)
                    return;
                Obj *buf = img->cells[IMG_BUF].o;
                int iw = img->cells[IMG_W].i, ih = img->cells[IMG_H].i;
                if (!buf)
                    return;
                for (int j = 0; j < ih && j < maxH; j++)
                    for (int i = 0; i < iw && i < maxW; i++)
                    {
                        uint32_t p = static_cast<uint32_t>(buf->cells[static_cast<size_t>(j) * iw + i].u);
                        if ((p >> 24) < 128)
                            continue;
                        int px = x + i, py = y + j;
                        if (px >= 0 && py >= 0 && px < fb->width && py < fb->height)
                            fb->pixels[py * fb->stride + px] = argb565(p);
                    }
            }

            // -- commandes / touches programmables
            static bool exitLike(int t) { return t == C_BACK || t == C_CANCEL || t == C_STOP || t == C_EXIT; }
            static std::vector<Obj *> sortedCmds(const Scr &s)
            {
                std::vector<Obj *> v = s.cmds;
                std::stable_sort(v.begin(), v.end(), [](Obj *a, Obj *b) { return cmdOf(a).prio < cmdOf(b).prio; });
                return v;
            }
            struct SoftKeys
            {
                Obj *left = nullptr, *right = nullptr;
                bool menu = false;
                std::vector<Obj *> menuCmds; // contenu du menu si left == « Menu »
                std::string leftLabel, rightLabel;
            };
            static SoftKeys softKeysFor(Obj *disp, Scr &s)
            {
                SoftKeys k;
                std::vector<Obj *> v = sortedCmds(s);
                // Commandes de l'item pris en focus (Form) : rajoutées au menu.
                if (s.kind == SK_FORM && s.focus >= 0 && s.focus < static_cast<int>(s.items.size()))
                {
                    It &it = itemOf(s.items[s.focus]);
                    for (Obj *c : it.cmds)
                        if (c != it.defCmd)
                            v.push_back(c);
                }
                if (v.empty())
                {
                    if (s.kind == SK_ALERT)
                        k.leftLabel = "OK";
                    return k;
                }
                Obj *ex = nullptr;
                for (Obj *c : v)
                    if (exitLike(cmdOf(c).type)) { ex = c; break; }
                if (v.size() == 1)
                {
                    if (ex) k.right = v[0]; else k.left = v[0];
                }
                else if (v.size() == 2)
                {
                    if (ex) { k.right = ex; k.left = (v[0] == ex) ? v[1] : v[0]; }
                    else { k.left = v[0]; k.right = v[1]; }
                }
                else
                {
                    k.right = ex;
                    for (Obj *c : v)
                        if (c != ex)
                            k.menuCmds.push_back(c);
                    if (k.menuCmds.size() == 1)
                        k.left = k.menuCmds[0];
                    else
                        k.menu = true;
                }
                if (k.left) k.leftLabel = cmdOf(k.left).label;
                if (k.menu) k.leftLabel = "Menu";
                if (k.right) k.rightLabel = cmdOf(k.right).label;
                (void)disp;
                return k;
            }

            static void drawSoftBar(const SoftKeys &k, int W, int H)
            {
                int bh = lineH() + 2 * g_S;
                rect(0, H - bh, W, bh, col::barBg);
                int cw = charW(), maxc = (W / 2) / cw - 1;
                if (!k.leftLabel.empty())
                {
                    text(2 * g_S, H - bh + g_S + g_S / 2, clip(k.leftLabel, maxc), col::barFg);
                    g_hits.push_back({0, H - bh, W / 2, bh, H_SOFT_L, 0, 0});
                }
                if (!k.rightLabel.empty())
                {
                    std::string r = clip(k.rightLabel, maxc);
                    text(W - 2 * g_S - static_cast<int>(r.size()) * cw, H - bh + g_S + g_S / 2, r, col::barFg);
                    g_hits.push_back({W / 2, H - bh, W - W / 2, bh, H_SOFT_R, 0, 0});
                }
            }

            static void drawTitle(const std::string &t, int W)
            {
                int th = lineH() + 2 * g_S;
                rect(0, 0, W, th, col::titleBg);
                text(2 * g_S, g_S + g_S / 2, clip(t, W / charW() - 1), col::titleFg);
            }

            // Barre de commandes pour les Canvas non plein écran qui ont des Commands.
            void lcduiCanvasOverlay(Obj *cur)
            {
                auto *fb = hal::display_get_framebuffer();
                if (!cur || !fb || !fb->pixels || cur->cellCount < 1 || cur->cells[GC_FULLSCREEN].i)
                    return;
                auto it = g_scrMap.find(cur);
                if (it == g_scrMap.end() || it->second.cmds.empty())
                    return;
                g_S = std::max(1, std::min(3, fb->width / 120));
                if (fb->width < 100) g_S = 1;
                SoftKeys k = softKeysFor(cur, it->second);
                g_hits.clear();
                drawSoftBar(k, fb->width, fb->height);
            }
            // Touches programmables d'un Canvas non plein écran avec Commands : consommées (renvoie les bits pris).
            uint32_t lcduiCanvasSoftKeys(Obj *cur, uint32_t justPressed)
            {
                if (!cur || cur->cellCount < 1 || cur->cells[GC_FULLSCREEN].i || !(justPressed & (hal::KEY_SOFT1 | hal::KEY_SOFT2)))
                    return 0;
                auto it = g_scrMap.find(cur);
                if (it == g_scrMap.end() || it->second.cmds.empty())
                    return 0;
                SoftKeys k = softKeysFor(cur, it->second);
                uint32_t used = 0;
                if ((justPressed & hal::KEY_SOFT1) && (k.left || k.menu))
                {
                    used |= hal::KEY_SOFT1;
                    if (k.left) uiDispatchCommand(k.left, cur);
                    else if (!k.menuCmds.empty()) uiDispatchCommand(k.menuCmds[0], cur); // sans menu graphique sur un Canvas : 1re commande
                }
                if ((justPressed & hal::KEY_SOFT2) && k.right)
                {
                    used |= hal::KEY_SOFT2;
                    uiDispatchCommand(k.right, cur);
                }
                return used;
            }

            // -- Choice : dessin d'une ligne
            static int choiceRowH() { return lineH() + g_S; }
            static void drawChoiceRow(const Chc &c, int i, int x, int y, int w, bool cursor, bool focusItem)
            {
                int rh = choiceRowH();
                bool on = cursor && focusItem;
                rect(x, y, w, rh, on ? col::selBg : col::bg);
                uint16_t fg = on ? col::selFg : col::fg;
                std::string mark;
                if (c.type == CH_EXCLUSIVE || c.type == CH_POPUP) mark = c.sel[i] ? "(*) " : "( ) ";
                else if (c.type == CH_MULTIPLE) mark = c.sel[i] ? "[x] " : "[ ] ";
                int cw = charW();
                int imgW = 0;
                if (c.img[i])
                {
                    imgW = std::min(c.img[i]->cells[IMG_W].i, 4 * lineH());
                    blitImage(c.img[i], x + 2 * g_S, y, imgW, rh);
                    imgW += 3 * g_S;
                }
                std::string line = mark + c.txt[i];
                text(x + 2 * g_S + imgW, y + g_S / 2, clip(line, (w - 4 * g_S - imgW) / cw), fg);
            }

            static void ensureVisible(int &scroll, int y0, int y1, int viewH)
            {
                if (y0 < scroll)
                    scroll = y0;
                else if (y1 > scroll + viewH)
                    scroll = y1 - viewH;
                if (scroll < 0)
                    scroll = 0;
            }


            // ---- CustomItem : appels vers le bytecode du jeu ----
            static int ciCallInt(Obj *item, const char *name, const char *desc, int arg, bool hasArg)
            {
                Value a[2] = {Value::fromRef(item), Value::fromInt(arg)}, r;
                if (g_interp && g_interp->invokeVirtual(item->cls, name, desc, item, a, hasArg ? 2 : 1, r))
                    return r.i;
                return 0;
            }
            static void ciCallKey(Obj *item, const char *name, int code)
            {
                Value a[2] = {Value::fromRef(item), Value::fromInt(code)}, r;
                if (g_interp)
                    g_interp->invokeVirtual(item->cls, name, "(I)V", item, a, 2, r);
                g_uiDirty = true;
            }
            static void ciCallPointer(Obj *item, const char *name, int x, int y)
            {
                Value a[3] = {Value::fromRef(item), Value::fromInt(x), Value::fromInt(y)}, r;
                if (g_interp)
                    g_interp->invokeVirtual(item->cls, name, "(II)V", item, a, 3, r);
                g_uiDirty = true;
            }
            // traverse(dir, viewportW, viewportH, int[4] visRect) : true = l'item garde le focus et gère lui-même le déplacement.
            static bool ciTraverse(Obj *item, int dir, int vw, int vh)
            {
                Obj *rect = g_rt->heap().newArray(ObjKind::IntArray, 4);
                if (rect)
                {
                    rect->cells[0] = Value::fromInt(0);
                    rect->cells[1] = Value::fromInt(0);
                    rect->cells[2] = Value::fromInt(vw);
                    rect->cells[3] = Value::fromInt(vh);
                }
                Value a[5] = {Value::fromRef(item), Value::fromInt(dir), Value::fromInt(vw), Value::fromInt(vh), Value::fromRef(rect)}, r;
                if (g_interp && g_interp->invokeVirtual(item->cls, "traverse", "(III[I)Z", item, a, 5, r))
                {
                    g_uiDirty = true;
                    return r.i != 0;
                }
                return false;
            }
            static Obj *g_customPressed = nullptr; // item CustomItem qui a reçu pointerPressed (pour dragged/released)
            static int g_customX = 0, g_customY = 0;
            static bool itemFocusable(Obj *o)
            {
                It &it = itemOf(o);
                if (it.kind == IK_TEXT || it.kind == IK_CHOICE || it.kind == IK_CUSTOM)
                    return true;
                if (it.kind == IK_GAUGE && it.interactive)
                    return true;
                if (it.appearance != 0 || it.defCmd || !it.cmds.empty())
                    return true;
                return false;
            }

            static void renderForm(Obj *disp, Scr &s, int W, int H, int bodyTop, int bodyBot)
            {
                const int cw = charW(), lh = lineH(), pad = 2 * g_S;
                const int textCols = std::max(4, (W - 2 * pad) / cw);
                struct Row { int y, h; };
                std::vector<Row> rows(s.items.size());
                // 1) mesure
                int y = 0;
                for (size_t n = 0; n < s.items.size(); n++)
                {
                    It &it = itemOf(s.items[n]);
                    int h = 0;
                    if (!it.label.empty() && it.kind != IK_SPACER) h += lh;
                    switch (it.kind)
                    {
                    case IK_STRING: h += static_cast<int>(wrap(it.text, textCols).size()) * lh; break;
                    case IK_IMAGE: h += it.img ? std::min(it.img->cells[IMG_H].i, (bodyBot - bodyTop) * 3 / 4) : lh; break;
                    case IK_TEXT: h += lh + 2 * g_S; break;
                    case IK_CHOICE:
                        h += (it.ch.type == CH_POPUP && !it.ch.popupOpen) ? choiceRowH() : static_cast<int>(it.ch.txt.size()) * choiceRowH();
                        break;
                    case IK_GAUGE: h += lh; break;
                    case IK_CUSTOM:
                    {
                        int pw = ciCallInt(s.items[n], "getPrefContentWidth", "(I)I", -1, true);
                        int ph = ciCallInt(s.items[n], "getPrefContentHeight", "(I)I", std::min(pw, W), true);
                        it.gaugeVal = std::max(1, std::min(pw > 0 ? pw : W, W));    // largeur retenue
                        it.gaugeMax = std::max(1, std::min(ph > 0 ? ph : lh, 4000)); // hauteur retenue
                        h += it.gaugeMax;
                        break;
                    }
                    case IK_DATE: h += lh; break;
                    case IK_SPACER: h += lh / 2; break;
                    default: h += lh; break;
                    }
                    h += g_S; // marge
                    rows[n] = {y, h};
                    y += h;
                }
                if (s.focus >= static_cast<int>(s.items.size())) s.focus = std::max(0, static_cast<int>(s.items.size()) - 1);
                int viewH = bodyBot - bodyTop;
                if (s.focus >= 0 && s.focus < static_cast<int>(rows.size()))
                    ensureVisible(s.scroll, rows[s.focus].y, rows[s.focus].y + rows[s.focus].h, viewH);
                s.scroll = std::max(0, std::min(s.scroll, std::max(0, y - viewH)));
                // 2) dessin
                for (size_t n = 0; n < s.items.size(); n++)
                {
                    int iy = bodyTop + rows[n].y - s.scroll;
                    if (iy + rows[n].h < bodyTop || iy > bodyBot)
                        continue;
                    It &it = itemOf(s.items[n]);
                    bool foc = static_cast<int>(n) == s.focus && itemFocusable(s.items[n]);
                    int cy = iy;
                    auto inBody = [&](int yy, int hh) { return yy >= bodyTop && yy + hh <= bodyBot; };
                    if (foc && inBody(iy, rows[n].h))
                        rect(0, iy, W, rows[n].h, rgb(226, 232, 250));
                    if (!it.label.empty() && it.kind != IK_SPACER && inBody(cy, lh))
                    {
                        text(pad, cy, clip(it.label, textCols), col::dim);
                        cy += lh;
                    }
                    else if (!it.label.empty() && it.kind != IK_SPACER)
                        cy += lh;
                    switch (it.kind)
                    {
                    case IK_STRING:
                    {
                        auto lines = wrap(it.text, textCols);
                        for (auto &l : lines)
                        {
                            if (inBody(cy, lh))
                                text(pad, cy, it.appearance == 2 ? "[" + clip(l, textCols - 2) + "]" : l, it.appearance == 1 ? rgb(20, 60, 200) : col::fg);
                            cy += lh;
                        }
                        break;
                    }
                    case IK_IMAGE:
                        if (it.img)
                        {
                            if (inBody(cy, 1))
                                blitImage(it.img, pad, cy, W - 2 * pad, std::max(0, bodyBot - cy));
                        }
                        else if (inBody(cy, lh))
                            text(pad, cy, clip(it.text, textCols), col::dim);
                        break;
                    case IK_TEXT:
                    {
                        if (inBody(cy, lh + 2 * g_S))
                        {
                            rect(pad, cy, W - 2 * pad, lh + g_S, col::box);
                            rect(pad, cy, W - 2 * pad, g_S, foc ? col::selBg : col::border);
                            std::string shown = (it.constraints & 0x10000) ? std::string(it.text.size(), '*') : it.text;
                            int maxc = (W - 2 * pad) / cw - 1;
                            if (static_cast<int>(shown.size()) > maxc) shown = shown.substr(shown.size() - maxc);
                            if (foc && ((jvm::virtualMillis() / 400) & 1)) shown += "_";
                            text(pad + g_S, cy + g_S, shown, col::fg);
                        }
                        break;
                    }
                    case IK_CHOICE:
                    {
                        if (it.ch.type == CH_POPUP && !it.ch.popupOpen)
                        {
                            if (inBody(cy, choiceRowH()) && !it.ch.txt.empty())
                            {
                                int si = std::max(0, it.ch.selectedIndex());
                                rect(pad, cy, W - 2 * pad, choiceRowH(), foc ? col::selBg : col::box);
                                text(pad + g_S, cy + g_S / 2, clip(it.ch.txt[si] + " v", textCols - 1), foc ? col::selFg : col::fg);
                            }
                        }
                        else
                        {
                            for (size_t i = 0; i < it.ch.txt.size(); i++)
                            {
                                if (inBody(cy, choiceRowH()))
                                {
                                    drawChoiceRow(it.ch, static_cast<int>(i), pad, cy, W - 2 * pad, static_cast<int>(i) == it.ch.cursor, foc);
                                    g_hits.push_back({pad, cy, W - 2 * pad, choiceRowH(), H_CHOICE_ROW, static_cast<int>(n), static_cast<int>(i)});
                                }
                                cy += choiceRowH();
                            }
                        }
                        break;
                    }
                    case IK_GAUGE:
                    {
                        if (inBody(cy, lh))
                        {
                            int bw = W - 2 * pad - 8 * cw;
                            rect(pad, cy + g_S, bw, lh - 2 * g_S, col::box);
                            rect(pad, cy + g_S, bw, g_S, col::border);
                            int fillw = 0;
                            std::string val;
                            if (it.gaugeMax > 0)
                            {
                                fillw = bw * std::max(0, std::min(it.gaugeVal, it.gaugeMax)) / it.gaugeMax;
                                val = std::to_string(it.gaugeVal) + "/" + std::to_string(it.gaugeMax);
                            }
                            else
                            {
                                fillw = bw / 5;
                                int ofs = static_cast<int>((jvm::virtualMillis() / 30) % (bw - fillw));
                                rect(pad + ofs, cy + 2 * g_S, fillw, lh - 4 * g_S, foc ? col::selBg : col::dim);
                                fillw = 0;
                            }
                            if (fillw > 0)
                                rect(pad, cy + 2 * g_S, fillw, lh - 4 * g_S, foc ? col::selBg : col::dim);
                            text(pad + bw + g_S, cy, val, col::fg);
                        }
                        break;
                    }
                    case IK_CUSTOM:
                    {
                        int cw2 = it.gaugeVal, ch2 = it.gaugeMax;
                        if (jvm::jmeDebug())
                            fprintf(stderr, "[lcdui] custom item n=%zu cy=%d iy=%d w=%d h=%d scroll=%d rowY=%d rowH=%d bodyTop=%d bodyBot=%d\n", n, cy, iy, cw2, ch2, s.scroll, rows[n].y, rows[n].h, bodyTop, bodyBot);
                        int ix = (W - cw2) / 2 < 0 ? 0 : (W - cw2) / 2; // centré
                        // Graphics « écran » : origine sur l'item, clip = item ∩ corps visible (coordonnées relatives à l'item).
                        Obj *g = screenGraphics();
                        int ax0 = std::max(ix, 0), ay0 = std::max(cy, bodyTop), ax1 = std::min(ix + cw2, W), ay1 = std::min(cy + ch2, bodyBot);
                        if (g && ax1 > ax0 && ay1 > ay0)
                        {
                            g->cells[G_TX] = Value::fromInt(ix);
                            g->cells[G_TY] = Value::fromInt(cy);
                            g->cells[G_CLIPX] = Value::fromInt(ax0 - ix);
                            g->cells[G_CLIPY] = Value::fromInt(ay0 - cy);
                            g->cells[G_CLIPW] = Value::fromInt(ax1 - ax0);
                            g->cells[G_CLIPH] = Value::fromInt(ay1 - ay0);
                            Value a[4] = {Value::fromRef(s.items[n]), Value::fromRef(g), Value::fromInt(cw2), Value::fromInt(ch2)}, r;
                            if (g_interp)
                                g_interp->invokeVirtual(s.items[n]->cls, "paint", "(Ljavax/microedition/lcdui/Graphics;II)V", s.items[n], a, 4, r);
                        }
                        g_hits.push_back({ix, std::max(cy, bodyTop), cw2, std::max(0, ay1 - std::max(cy, bodyTop)), H_CUSTOM, static_cast<int>(n), cy});
                        break;
                    }
                    default:
                        break;
                    }
                    if (inBody(iy, rows[n].h) || true)
                        g_hits.push_back({0, std::max(iy, bodyTop), W, std::min(rows[n].h, bodyBot - std::max(iy, bodyTop)), H_ITEM, static_cast<int>(n), 0});
                }
                // ascenseur
                if (y > viewH)
                {
                    int th = std::max(6 * g_S, viewH * viewH / y);
                    int ty = bodyTop + (viewH - th) * s.scroll / std::max(1, y - viewH);
                    rect(W - 2 * g_S, ty, 2 * g_S, th, col::dim);
                }
                (void)disp;
            }

            static void renderList(Obj *disp, Scr &s, int W, int bodyTop, int bodyBot)
            {
                Chc &c = s.ch;
                int rh = choiceRowH() + g_S;
                int viewH = bodyBot - bodyTop, n = static_cast<int>(c.txt.size());
                if (c.cursor >= n) c.cursor = std::max(0, n - 1);
                int total = n * rh;
                ensureVisible(s.scroll, c.cursor * rh, (c.cursor + 1) * rh, viewH);
                s.scroll = std::max(0, std::min(s.scroll, std::max(0, total - viewH)));
                for (int i = 0; i < n; i++)
                {
                    int y = bodyTop + i * rh - s.scroll;
                    if (y + rh < bodyTop || y > bodyBot)
                        continue;
                    if (y < bodyTop || y + rh > bodyBot)
                        continue; // ligne partiellement visible : non dessinée (évite de déborder sur les barres)
                    drawChoiceRow(c, i, 0, y, W - (total > viewH ? 2 * g_S : 0), i == c.cursor, true);
                    g_hits.push_back({0, y, W, rh, H_LIST_ROW, i, 0});
                }
                if (total > viewH)
                {
                    int th = std::max(6 * g_S, viewH * viewH / total);
                    int ty = bodyTop + (viewH - th) * s.scroll / std::max(1, total - viewH);
                    rect(W - 2 * g_S, ty, 2 * g_S, th, col::dim);
                }
                (void)disp;
            }

            static void renderTextBox(Scr &s, int W, int bodyTop, int bodyBot, bool blink)
            {
                const int pad = 2 * g_S, cols = std::max(4, (W - 2 * pad) / charW());
                std::string shown = (s.constraints & 0x10000) ? std::string(s.text.size(), '*') : s.text;
                if (blink) shown += "_";
                auto lines = wrap(shown, cols);
                int y = bodyTop + pad;
                // On garde la fin du texte visible.
                int fit = std::max(1, (bodyBot - bodyTop - pad) / lineH());
                size_t first = lines.size() > static_cast<size_t>(fit) ? lines.size() - fit : 0;
                for (size_t i = first; i < lines.size(); i++, y += lineH())
                    text(pad, y, lines[i], col::fg);
            }

            static void renderAlert(Scr &s, int W, int H)
            {
                rect(0, 0, W, H, col::shade);
                const int pad = 3 * g_S, cw = charW(), lh = lineH();
                int boxW = std::min(W - 4 * g_S, std::max(W * 3 / 4, 22 * cw));
                int cols = (boxW - 2 * pad) / cw;
                auto lines = wrap(s.text, cols);
                int maxLines = std::max(1, (H - 8 * lh) / lh);
                if (static_cast<int>(lines.size()) > maxLines)
                {
                    lines.resize(maxLines);
                    lines.back() = clip(lines.back() + "...", cols);
                }
                int boxH = lh + 2 * g_S + static_cast<int>(lines.size()) * lh + 2 * pad;
                int bx = (W - boxW) / 2, by = (H - boxH) / 2;
                rect(bx - g_S, by - g_S, boxW + 2 * g_S, boxH + 2 * g_S, col::border);
                rect(bx, by, boxW, boxH, col::alertBg);
                rect(bx, by, boxW, lh + 2 * g_S, s.alertType ? rgb(160, 40, 40) : col::titleBg);
                text(bx + pad, by + g_S + g_S / 2, clip(s.title.empty() ? "Alert" : s.title, cols), col::titleFg);
                int y = by + lh + 2 * g_S + pad;
                for (auto &l : lines)
                {
                    int lw = static_cast<int>(l.size()) * cw;
                    text(bx + (boxW - lw) / 2, y, l, col::fg);
                    y += lh;
                }
                g_hits.push_back({bx, by, boxW, boxH, H_ALERT, 0, 0});
            }

            static void renderCmdMenu(const SoftKeys &k, int W, int H, Scr &s)
            {
                const int cw = charW(), rh = choiceRowH() + g_S, bh = lineH() + 2 * g_S;
                int n = static_cast<int>(k.menuCmds.size());
                int menuH = std::min(n * rh + g_S, H - bh - lineH());
                int menuW = std::min(W, std::max(W * 2 / 3, 14 * cw));
                int x = 0, y = H - bh - menuH;
                rect(x, y - g_S, menuW, menuH + g_S, col::border);
                rect(x, y, menuW - g_S, menuH, col::box);
                for (int i = 0; i < n && (i + 1) * rh <= menuH; i++)
                {
                    bool on = i == s.menuSel;
                    rect(x, y + i * rh, menuW - g_S, rh, on ? col::selBg : col::box);
                    text(x + 2 * g_S, y + i * rh + g_S / 2, clip(cmdOf(k.menuCmds[i]).label, menuW / cw - 2), on ? col::selFg : col::fg);
                    g_hits.push_back({x, y + i * rh, menuW, rh, H_MENU_ROW, i, 0});
                }
            }

            static void renderScreen(Obj *disp, Scr &s)
            {
                auto *fb = hal::display_get_framebuffer();
                if (!fb || !fb->pixels)
                    return;
                const int W = fb->width, H = fb->height;
                g_S = std::max(1, std::min(3, W / 120));
                g_hits.clear();
                rect(0, 0, W, H, col::bg);
                SoftKeys k = softKeysFor(disp, s);
                int th = lineH() + 2 * g_S, bh = lineH() + 2 * g_S;
                int bodyTop = 0, bodyBot = H - bh;
                if (s.kind == SK_ALERT)
                {
                    renderAlert(s, W, H);
                    drawSoftBar(k, W, H);
                    return;
                }
                if (!s.title.empty() || s.kind != SK_ALERT)
                {
                    drawTitle(s.title, W);
                    bodyTop = th;
                }
                switch (s.kind)
                {
                case SK_FORM: renderForm(disp, s, W, H, bodyTop, bodyBot); break;
                case SK_LIST: renderList(disp, s, W, bodyTop, bodyBot); break;
                case SK_TEXTBOX: renderTextBox(s, W, bodyTop, bodyBot, (jvm::virtualMillis() / 400) & 1); break;
                default: break;
                }
                drawSoftBar(k, W, H);
                if (s.menuOpen && k.menu)
                    renderCmdMenu(k, W, H, s);
            }

            // ------------------------------------------------------------------------------------
            // Entrées
            // ------------------------------------------------------------------------------------
            static bool typeInto(std::string &txt, int &caret, int maxSize, int constraints, bool &changed)
            {
                (void)caret;
                bool any = false;
                for (int b = 0; b < g_backspaces; b++)
                    if (!txt.empty()) { txt.pop_back(); changed = any = true; }
                for (char ch : g_textIn)
                {
                    unsigned char u = static_cast<unsigned char>(ch);
                    if (u < 0x20 || u > 0x7E)
                        continue;
                    int mode = constraints & 0xFFFF;
                    if (mode == 2 && !(isdigit(u) || u == '-')) continue;                 // NUMERIC
                    if (mode == 3 && !(isdigit(u) || u == '+' || u == '*' || u == '#')) continue; // PHONENUMBER
                    if (mode == 5 && !(isdigit(u) || u == '-' || u == '.')) continue;     // DECIMAL
                    if (static_cast<int>(txt.size()) >= maxSize) break;
                    txt += ch;
                    changed = any = true;
                }
                return any;
            }

            static void dismissAlert(Obj *alert, Scr &a)
            {
                Obj *next = a.next ? a.next : a.prev;
                if (next && next != alert)
                {
                    switchCurrent(next);
                    g_paintRequested = true;
                    clearFramebuffers();
                    enterScreen(next);
                }
                if (a.listener && g_alertDismissCommand)
                    uiDispatchCommand(g_alertDismissCommand, alert);
                g_uiDirty = true;
            }

            static void activateFocusedItem(Obj *form, Scr &s)
            {
                if (s.focus < 0 || s.focus >= static_cast<int>(s.items.size()))
                    return;
                Obj *io = s.items[s.focus];
                It &it = itemOf(io);
                if (it.kind == IK_CHOICE && !it.ch.txt.empty())
                {
                    Chc &c = it.ch;
                    if (c.type == CH_POPUP && !c.popupOpen)
                    {
                        c.popupOpen = true;
                        c.cursor = std::max(0, c.selectedIndex());
                    }
                    else
                    {
                        int i = c.cursor;
                        if (c.type == CH_MULTIPLE) c.sel[i] = !c.sel[i];
                        else c.selectOnly(i);
                        c.popupOpen = false;
                        notifyItemChanged(form, io);
                    }
                    return;
                }
                if (it.defCmd)
                    dispatchItemCommand(it.defCmd, io);
                else if (!it.cmds.empty() && it.kind != IK_TEXT)
                    dispatchItemCommand(it.cmds[0], io);
                g_uiDirty = true;
            }

            static void pressSoftLeft(Obj *disp, Scr &s, const SoftKeys &k)
            {
                if (s.kind == SK_ALERT)
                {
                    if (k.left) uiDispatchCommand(k.left, disp);
                    else dismissAlert(disp, s);
                    return;
                }
                if (k.menu)
                {
                    s.menuOpen = !s.menuOpen;
                    s.menuSel = 0;
                    g_uiDirty = true;
                }
                else if (k.left)
                {
                    // Commande d'item ?
                    if (s.kind == SK_FORM && s.focus >= 0 && s.focus < static_cast<int>(s.items.size()))
                    {
                        It &it = itemOf(s.items[s.focus]);
                        if (std::find(it.cmds.begin(), it.cmds.end(), k.left) != it.cmds.end())
                        {
                            dispatchItemCommand(k.left, s.items[s.focus]);
                            return;
                        }
                    }
                    uiDispatchCommand(k.left, disp);
                }
            }

            void lcduiPointer(int kind, int x, int y)
            {
                Obj *disp = g_current;
                if (!lcduiIsScreen(disp))
                    return;
                if (kind != 0)
                {
                    if (g_customPressed)
                    {
                        ciCallPointer(g_customPressed, kind == 1 ? "pointerReleased" : "pointerDragged", x - g_customX, y - g_customY);
                        if (kind == 1)
                            g_customPressed = nullptr;
                    }
                    return;
                }
                Scr &s = scrOf(disp);
                SoftKeys k = softKeysFor(disp, s);
                for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
                {
                    const Hit &h = *it;
                    if (x < h.x || y < h.y || x >= h.x + h.w || y >= h.y + h.h)
                        continue;
                    switch (h.kind)
                    {
                    case H_SOFT_L: pressSoftLeft(disp, s, k); return;
                    case H_SOFT_R: if (k.right) uiDispatchCommand(k.right, disp); else if (s.kind == SK_ALERT) dismissAlert(disp, s); return;
                    case H_ALERT: if (s.kind == SK_ALERT && s.cmds.empty()) dismissAlert(disp, s); return;
                    case H_MENU_ROW:
                        if (h.idx < static_cast<int>(k.menuCmds.size()))
                        {
                            s.menuOpen = false;
                            uiDispatchCommand(k.menuCmds[h.idx], disp);
                        }
                        return;
                    case H_LIST_ROW:
                    {
                        Chc &c = s.ch;
                        c.cursor = h.idx;
                        if (c.type == CH_MULTIPLE) c.sel[h.idx] = !c.sel[h.idx];
                        else c.selectOnly(h.idx);
                        if (c.type == CH_IMPLICIT)
                            uiDispatchCommand(s.selectCmd ? s.selectCmd : g_listSelectCommand, disp);
                        g_uiDirty = true;
                        return;
                    }
                    case H_CHOICE_ROW:
                    {
                        It &it = itemOf(s.items[h.idx]);
                        s.focus = h.idx;
                        it.ch.cursor = h.sub;
                        if (it.ch.type == CH_MULTIPLE) it.ch.sel[h.sub] = !it.ch.sel[h.sub];
                        else it.ch.selectOnly(h.sub);
                        notifyItemChanged(disp, s.items[h.idx]);
                        g_uiDirty = true;
                        return;
                    }
                    case H_CUSTOM:
                    {
                        Obj *io = s.items[h.idx];
                        s.focus = h.idx;
                        g_customPressed = io;
                        g_customX = h.x;
                        g_customY = h.sub;
                        ciCallPointer(io, "pointerPressed", x - h.x, y - h.sub);
                        return;
                    }
                    case H_ITEM:
                        if (s.kind == SK_FORM && h.idx < static_cast<int>(s.items.size()))
                        {
                            bool already = (s.focus == h.idx);
                            s.focus = h.idx;
                            if (already && itemFocusable(s.items[h.idx]))
                                activateFocusedItem(disp, s);
                            g_uiDirty = true;
                        }
                        return;
                    default: break;
                    }
                }
            }

            // Un tour d'entrées + rendu pour l'écran haut niveau courant.
            void lcduiTick(Obj *disp, uint32_t justPressed, uint32_t justReleased)
            {
                if (!lcduiIsScreen(disp))
                    return;
                Scr &s = scrOf(disp);
                SoftKeys k = softKeysFor(disp, s);
                bool typedSpace = g_textIn.find(' ') != std::string::npos;

                // ---- Alert : minuterie
                if (s.kind == SK_ALERT)
                {
                    if (s.shownAt < 0)
                        s.shownAt = jvm::virtualMillis();
                    if (s.timeout != kAlertForever && s.timeout > 0 && s.cmds.empty() &&
                        jvm::virtualMillis() - s.shownAt >= s.timeout)
                    {
                        dismissAlert(disp, s);
                        return;
                    }
                    if (justPressed & (hal::KEY_FIRE | hal::KEY_SOFT1 | hal::KEY_SOFT2))
                    {
                        if (s.cmds.empty())
                        {
                            dismissAlert(disp, s);
                            return;
                        }
                        if ((justPressed & hal::KEY_SOFT2) && k.right) { uiDispatchCommand(k.right, disp); return; }
                        if ((justPressed & (hal::KEY_SOFT1 | hal::KEY_FIRE)) && (k.left || k.menu)) { pressSoftLeft(disp, s, k); return; }
                    }
                }
                // ---- menu de commandes ouvert
                else if (s.menuOpen && k.menu)
                {
                    int n = static_cast<int>(k.menuCmds.size());
                    if (justPressed & hal::KEY_UP) s.menuSel = (s.menuSel + n - 1) % n;
                    if (justPressed & hal::KEY_DOWN) s.menuSel = (s.menuSel + 1) % n;
                    if (justPressed & hal::KEY_FIRE)
                    {
                        s.menuOpen = false;
                        Obj *c = k.menuCmds[std::min(s.menuSel, n - 1)];
                        if (s.kind == SK_FORM && s.focus >= 0 && s.focus < static_cast<int>(s.items.size()) &&
                            std::find(itemOf(s.items[s.focus]).cmds.begin(), itemOf(s.items[s.focus]).cmds.end(), c) != itemOf(s.items[s.focus]).cmds.end())
                            dispatchItemCommand(c, s.items[s.focus]);
                        else
                            uiDispatchCommand(c, disp);
                        g_uiDirty = true;
                    }
                    if (justPressed & (hal::KEY_SOFT1 | hal::KEY_SOFT2)) { s.menuOpen = false; }
                    g_uiDirty = true;
                }
                else
                {
                    if (justPressed & hal::KEY_SOFT1) pressSoftLeft(disp, s, k);
                    else if ((justPressed & hal::KEY_SOFT2) && k.right) uiDispatchCommand(k.right, disp);
                    if (g_current != disp)
                        return;

                    if (s.kind == SK_LIST)
                    {
                        Chc &c = s.ch;
                        int n = static_cast<int>(c.txt.size());
                        if (n > 0)
                        {
                            if (justPressed & hal::KEY_UP) { c.cursor = (c.cursor + n - 1) % n; if (c.type != CH_MULTIPLE) c.selectOnly(c.cursor); g_uiDirty = true; }
                            if (justPressed & hal::KEY_DOWN) { c.cursor = (c.cursor + 1) % n; if (c.type != CH_MULTIPLE) c.selectOnly(c.cursor); g_uiDirty = true; }
                            if (justPressed & hal::KEY_FIRE)
                            {
                                if (c.type == CH_MULTIPLE) c.sel[c.cursor] = !c.sel[c.cursor];
                                else c.selectOnly(c.cursor);
                                if (c.type == CH_IMPLICIT)
                                    uiDispatchCommand(s.selectCmd ? s.selectCmd : g_listSelectCommand, disp);
                                g_uiDirty = true;
                            }
                        }
                        else if ((justPressed & hal::KEY_FIRE) && c.type == CH_IMPLICIT)
                            uiDispatchCommand(s.selectCmd ? s.selectCmd : g_listSelectCommand, disp);
                    }
                    else if (s.kind == SK_FORM)
                    {
                        int n = static_cast<int>(s.items.size());
                        Obj *fo = (s.focus >= 0 && s.focus < n) ? s.items[s.focus] : nullptr;
                        It *fi = fo ? &itemOf(fo) : nullptr;
                        bool inPopup = fi && fi->kind == IK_CHOICE && fi->ch.popupOpen;
                        bool inChoice = fi && fi->kind == IK_CHOICE && (!fi->ch.txt.empty()) && (fi->ch.type != CH_POPUP || inPopup);
                        bool moved = false;
                        auto step = [&](int dir) {
                            for (int t = 1; t <= n; t++)
                            {
                                int j = ((s.focus + dir * t) % n + n) % n;
                                if (itemFocusable(s.items[j])) { s.focus = j; moved = true; return; }
                            }
                            s.scroll += dir * lineH() * 2; // rien de focusable : simple défilement
                            if (s.scroll < 0) s.scroll = 0;
                            g_uiDirty = true;
                        };
                        if (n > 0 && fi && fi->kind == IK_CUSTOM)
                        {
                            const int vw = screenW(), vh = std::max(1, screenH() - 2 * lineH() - 4 * g_S);
                            if (s.enteredItem != fo)
                            {
                                s.enteredItem = fo;
                                ciTraverse(fo, 0, vw, vh); // CustomItem.NONE : l'item vient de prendre le focus
                            }
                            struct { uint32_t key; int dir; int step; } arrows[4] = {
                                {hal::KEY_UP, 1, -1}, {hal::KEY_DOWN, 6, 1}, {hal::KEY_LEFT, 2, -1}, {hal::KEY_RIGHT, 5, 1}};
                            for (auto &a : arrows)
                                if (justPressed & a.key)
                                    if (!ciTraverse(fo, a.dir, vw, vh))
                                        step(a.step); // l'item ne garde pas le focus : on passe à l'item voisin
                            static const hal::KeyCode others[] = {hal::KEY_FIRE, hal::KEY_0, hal::KEY_1, hal::KEY_2, hal::KEY_3, hal::KEY_4, hal::KEY_5,
                                                                  hal::KEY_6, hal::KEY_7, hal::KEY_8, hal::KEY_9, hal::KEY_STAR, hal::KEY_HASH};
                            for (hal::KeyCode kc : others)
                            {
                                if (justPressed & kc)
                                    ciCallKey(fo, "keyPressed", midp::halKeyToMidp(kc));
                                if (justReleased & kc)
                                    ciCallKey(fo, "keyReleased", midp::halKeyToMidp(kc));
                            }
                            if (s.focus < n && s.items[s.focus] != fo)
                                s.enteredItem = nullptr;
                            g_uiDirty = true;
                        }
                        else if (n > 0)
                        {
                            if (justPressed & hal::KEY_UP)
                            {
                                if (inChoice && fi->ch.cursor > 0) { fi->ch.cursor--; g_uiDirty = true; }
                                else step(-1);
                            }
                            if (justPressed & hal::KEY_DOWN)
                            {
                                if (inChoice && fi->ch.cursor + 1 < static_cast<int>(fi->ch.txt.size())) { fi->ch.cursor++; g_uiDirty = true; }
                                else step(1);
                            }
                            if (fi && fi->kind == IK_GAUGE && fi->interactive && fi->gaugeMax > 0)
                            {
                                int d = ((justPressed & hal::KEY_RIGHT) ? 1 : 0) - ((justPressed & hal::KEY_LEFT) ? 1 : 0);
                                if (d)
                                {
                                    fi->gaugeVal = std::max(0, std::min(fi->gaugeMax, fi->gaugeVal + d * std::max(1, fi->gaugeMax / 10)));
                                    notifyItemChanged(disp, fo);
                                }
                            }
                            else if (fi && fi->kind == IK_CHOICE && fi->ch.type == CH_POPUP && !inPopup && !fi->ch.txt.empty())
                            {
                                int d = ((justPressed & hal::KEY_RIGHT) ? 1 : 0) - ((justPressed & hal::KEY_LEFT) ? 1 : 0);
                                if (d)
                                {
                                    int m = static_cast<int>(fi->ch.txt.size());
                                    fi->ch.selectOnly(((fi->ch.selectedIndex() + d) % m + m) % m);
                                    notifyItemChanged(disp, fo);
                                }
                            }
                            if (justPressed & hal::KEY_FIRE)
                            {
                                if (!(fi && fi->kind == IK_TEXT && typedSpace))
                                {
                                    if (fi && fi->kind == IK_TEXT) step(1);
                                    else activateFocusedItem(disp, s);
                                }
                            }
                            if (fi && fi->kind == IK_TEXT)
                            {
                                bool changed = false;
                                typeInto(fi->text, fi->caret, fi->maxSize, fi->constraints, changed);
                                if (changed) notifyItemChanged(disp, fo);
                            }
                            (void)moved;
                        }
                        g_uiDirty = true;
                    }
                    else if (s.kind == SK_TEXTBOX)
                    {
                        bool changed = false;
                        typeInto(s.text, s.caret, s.maxSize, s.constraints, changed);
                    }
                }

                // ---- rendu (état changé, ou animation : curseur clignotant / jauge / minuterie)
                bool animated = s.kind == SK_TEXTBOX ||
                                (s.kind == SK_FORM && s.focus >= 0 && s.focus < static_cast<int>(s.items.size()) &&
                                 (itemOf(s.items[s.focus]).kind == IK_TEXT || itemOf(s.items[s.focus]).kind == IK_GAUGE)) ||
                                s.kind == SK_ALERT;
                static int64_t lastAnim = 0;
                int64_t now = jvm::virtualMillis();
                if (g_uiDirty || g_paintRequested || (animated && now - lastAnim >= 200))
                {
                    lastAnim = now;
                    g_uiDirty = false;
                    g_paintRequested = false;
                    if (g_current == disp)
                    {
                        renderScreen(disp, scrOf(disp));
                        hal::display_present(hal::display_get_framebuffer());
                    }
                }
            }

            // ------------------------------------------------------------------------------------
            // Enregistrement
            // ------------------------------------------------------------------------------------
            void lcduiBootstrap(Runtime *rt)
            {
                g_cmdMap.clear();
                g_itemMap.clear();
                g_scrMap.clear();
                auto mk = [&](const char *cls, const char *field, const char *label, int type, Obj *&store) {
                    ClassInfo *c = rt->classInfoOfName(cls);
                    ClassInfo *cmdCls = rt->classInfoOfName("javax/microedition/lcdui/Command");
                    if (!c || !cmdCls)
                        return;
                    const MethodRecord *f = c->findField(field, "Ljavax/microedition/lcdui/Command;");
                    Obj *inst = f ? rt->heap().newInstance(cmdCls) : nullptr;
                    if (!inst)
                        return;
                    c->statics[f->slot] = Value::fromRef(inst);
                    Cmd &cm = cmdOf(inst);
                    cm.label = label;
                    cm.type = C_OK;
                    store = inst;
                };
                mk("javax/microedition/lcdui/List", "SELECT_COMMAND", "Select", C_SCREEN, g_listSelectCommand);
                mk("javax/microedition/lcdui/Alert", "DISMISS_COMMAND", "OK", C_OK, g_alertDismissCommand);
            }

            void registerLcduiNatives()
            {
#define R(k, f) regN(k, f)
                // Command
                R("javax/microedition/lcdui/Command.<init>:(Ljava/lang/String;II)V", cmd_init3);
                R("javax/microedition/lcdui/Command.<init>:(Ljava/lang/String;Ljava/lang/String;II)V", cmd_init4);
                R("javax/microedition/lcdui/Command.getLabel:()Ljava/lang/String;", cmd_getLabel);
                R("javax/microedition/lcdui/Command.getLongLabel:()Ljava/lang/String;", cmd_getLongLabel);
                R("javax/microedition/lcdui/Command.getCommandType:()I", cmd_getType);
                R("javax/microedition/lcdui/Command.getPriority:()I", cmd_getPriority);
                // Displayable
                R("javax/microedition/lcdui/Displayable.setTitle:(Ljava/lang/String;)V", disp_setTitle);
                R("javax/microedition/lcdui/Displayable.getTitle:()Ljava/lang/String;", disp_getTitle);
                R("javax/microedition/lcdui/Displayable.addCommand:(Ljavax/microedition/lcdui/Command;)V", disp_addCommand);
                R("javax/microedition/lcdui/Displayable.removeCommand:(Ljavax/microedition/lcdui/Command;)V", disp_removeCommand);
                R("javax/microedition/lcdui/Displayable.setCommandListener:(Ljavax/microedition/lcdui/CommandListener;)V", disp_setListener);
                R("javax/microedition/lcdui/Displayable.setTicker:(Ljavax/microedition/lcdui/Ticker;)V", disp_setTicker);
                R("javax/microedition/lcdui/Displayable.getTicker:()Ljavax/microedition/lcdui/Ticker;", disp_getTicker);
                R("javax/microedition/lcdui/Displayable.isShown:()Z", disp_isShown);
                R("javax/microedition/lcdui/Displayable.getWidth:()I", disp_getWidth);
                R("javax/microedition/lcdui/Displayable.getHeight:()I", disp_getHeight);
                R("javax/microedition/lcdui/Ticker.<init>:(Ljava/lang/String;)V", noop);
                // Display
                R("javax/microedition/lcdui/Display.setCurrent:(Ljavax/microedition/lcdui/Displayable;)V", d_setCurrent);
                R("javax/microedition/lcdui/Display.setCurrent:(Ljavax/microedition/lcdui/Alert;Ljavax/microedition/lcdui/Displayable;)V", d_setCurrentAlert);
                R("javax/microedition/lcdui/Display.setCurrentItem:(Ljavax/microedition/lcdui/Item;)V", d_setCurrentItem);
                // Alert
                R("javax/microedition/lcdui/Alert.<init>:(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;)V", alert_init4);
                R("javax/microedition/lcdui/Alert.<init>:(Ljava/lang/String;)V", alert_init1);
                R("javax/microedition/lcdui/Alert.setTimeout:(I)V", alert_setTimeout);
                R("javax/microedition/lcdui/Alert.getTimeout:()I", alert_getTimeout);
                R("javax/microedition/lcdui/Alert.setString:(Ljava/lang/String;)V", alert_setString);
                R("javax/microedition/lcdui/Alert.getString:()Ljava/lang/String;", alert_getString);
                R("javax/microedition/lcdui/Alert.setIndicator:(Ljavax/microedition/lcdui/Gauge;)V", alert_setIndicator);
                R("javax/microedition/lcdui/Alert.getIndicator:()Ljavax/microedition/lcdui/Gauge;", alert_getIndicator);
                R("javax/microedition/lcdui/Alert.setType:(Ljavax/microedition/lcdui/AlertType;)V", noop);
                R("javax/microedition/lcdui/Alert.setImage:(Ljavax/microedition/lcdui/Image;)V", noop);
                R("javax/microedition/lcdui/AlertType.<init>:()V", noop);
                // List
                R("javax/microedition/lcdui/List.<init>:(Ljava/lang/String;I[Ljava/lang/String;Ljavax/microedition/lcdui/Image;)V", list_init);
                R("javax/microedition/lcdui/List.<init>:(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V", list_init);
                R("javax/microedition/lcdui/List.<init>:(Ljava/lang/String;I)V", list_init);
                R("javax/microedition/lcdui/List.setSelectCommand:(Ljavax/microedition/lcdui/Command;)V", list_setSelectCommand);
                // ChoiceGroup
                R("javax/microedition/lcdui/ChoiceGroup.<init>:(Ljava/lang/String;I[Ljava/lang/String;Ljavax/microedition/lcdui/Image;)V", cg_init);
                R("javax/microedition/lcdui/ChoiceGroup.<init>:(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V", cg_init);
                R("javax/microedition/lcdui/ChoiceGroup.<init>:(Ljava/lang/String;I)V", cg_init);
                for (const char *cls : {"javax/microedition/lcdui/List", "javax/microedition/lcdui/ChoiceGroup"})
                {
                    std::string c = cls;
                    R((c + ".append:(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I").c_str(), ch_append);
                    R((c + ".insert:(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V").c_str(), ch_insert);
                    R((c + ".set:(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V").c_str(), ch_set);
                    R((c + ".delete:(I)V").c_str(), ch_delete);
                    R((c + ".deleteAll:()V").c_str(), ch_deleteAll);
                    R((c + ".size:()I").c_str(), ch_size);
                    R((c + ".getString:(I)Ljava/lang/String;").c_str(), ch_getString);
                    R((c + ".getImage:(I)Ljavax/microedition/lcdui/Image;").c_str(), ch_getImage);
                    R((c + ".isSelected:(I)Z").c_str(), ch_isSelected);
                    R((c + ".getSelectedIndex:()I").c_str(), ch_getSelectedIndex);
                    R((c + ".setSelectedIndex:(IZ)V").c_str(), ch_setSelectedIndex);
                    R((c + ".getSelectedFlags:([Z)I").c_str(), ch_getSelectedFlags);
                    R((c + ".setSelectedFlags:([Z)V").c_str(), ch_setSelectedFlags);
                    R((c + ".setFitPolicy:(I)V").c_str(), ch_setFit);
                    R((c + ".getFitPolicy:()I").c_str(), ch_getFit);
                    R((c + ".setFont:(ILjavax/microedition/lcdui/Font;)V").c_str(), noop);
                }
                // Form
                R("javax/microedition/lcdui/Form.<init>:(Ljava/lang/String;)V", form_init);
                R("javax/microedition/lcdui/Form.<init>:(Ljava/lang/String;[Ljavax/microedition/lcdui/Item;)V", form_init);
                R("javax/microedition/lcdui/Form.append:(Ljavax/microedition/lcdui/Item;)I", form_append);
                R("javax/microedition/lcdui/Form.append:(Ljava/lang/String;)I", form_append);
                R("javax/microedition/lcdui/Form.append:(Ljavax/microedition/lcdui/Image;)I", form_append);
                R("javax/microedition/lcdui/Form.insert:(ILjavax/microedition/lcdui/Item;)V", form_insert);
                R("javax/microedition/lcdui/Form.set:(ILjavax/microedition/lcdui/Item;)V", form_set);
                R("javax/microedition/lcdui/Form.delete:(I)V", form_delete);
                R("javax/microedition/lcdui/Form.deleteAll:()V", form_deleteAll);
                R("javax/microedition/lcdui/Form.get:(I)Ljavax/microedition/lcdui/Item;", form_get);
                R("javax/microedition/lcdui/Form.size:()I", form_size);
                R("javax/microedition/lcdui/Form.setItemStateListener:(Ljavax/microedition/lcdui/ItemStateListener;)V", form_setItemStateListener);
                // Item
                R("javax/microedition/lcdui/Item.getLabel:()Ljava/lang/String;", item_getLabel);
                R("javax/microedition/lcdui/Item.setLabel:(Ljava/lang/String;)V", item_setLabel);
                R("javax/microedition/lcdui/Item.getLayout:()I", item_getLayout);
                R("javax/microedition/lcdui/Item.setLayout:(I)V", item_setLayout);
                R("javax/microedition/lcdui/Item.addCommand:(Ljavax/microedition/lcdui/Command;)V", item_addCommand);
                R("javax/microedition/lcdui/Item.removeCommand:(Ljavax/microedition/lcdui/Command;)V", item_removeCommand);
                R("javax/microedition/lcdui/Item.setDefaultCommand:(Ljavax/microedition/lcdui/Command;)V", item_setDefaultCommand);
                R("javax/microedition/lcdui/Item.setItemCommandListener:(Ljavax/microedition/lcdui/ItemCommandListener;)V", item_setCmdListener);
                R("javax/microedition/lcdui/Item.notifyStateChanged:()V", item_notifyStateChanged);
                R("javax/microedition/lcdui/Item.setPreferredSize:(II)V", noop);
                // StringItem / ImageItem
                R("javax/microedition/lcdui/StringItem.<init>:(Ljava/lang/String;Ljava/lang/String;)V", si_init);
                R("javax/microedition/lcdui/StringItem.<init>:(Ljava/lang/String;Ljava/lang/String;I)V", si_init);
                R("javax/microedition/lcdui/StringItem.getText:()Ljava/lang/String;", si_getText);
                R("javax/microedition/lcdui/StringItem.setText:(Ljava/lang/String;)V", si_setText);
                R("javax/microedition/lcdui/StringItem.getAppearanceMode:()I", item_getAppearance);
                R("javax/microedition/lcdui/StringItem.setFont:(Ljavax/microedition/lcdui/Font;)V", noop);
                R("javax/microedition/lcdui/ImageItem.<init>:(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;)V", ii_init);
                R("javax/microedition/lcdui/ImageItem.<init>:(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;I)V", ii_init);
                R("javax/microedition/lcdui/ImageItem.getImage:()Ljavax/microedition/lcdui/Image;", ii_getImage);
                R("javax/microedition/lcdui/ImageItem.setImage:(Ljavax/microedition/lcdui/Image;)V", ii_setImage);
                R("javax/microedition/lcdui/ImageItem.getAltText:()Ljava/lang/String;", ii_getAlt);
                R("javax/microedition/lcdui/ImageItem.setAltText:(Ljava/lang/String;)V", ii_setAlt);
                R("javax/microedition/lcdui/ImageItem.getAppearanceMode:()I", item_getAppearance);
                // TextField / TextBox
                R("javax/microedition/lcdui/TextField.<init>:(Ljava/lang/String;Ljava/lang/String;II)V", tf_init);
                R("javax/microedition/lcdui/TextBox.<init>:(Ljava/lang/String;Ljava/lang/String;II)V", tb_init);
                for (const char *cls : {"javax/microedition/lcdui/TextField", "javax/microedition/lcdui/TextBox"})
                {
                    std::string c = cls;
                    R((c + ".getString:()Ljava/lang/String;").c_str(), tx_getString);
                    R((c + ".setString:(Ljava/lang/String;)V").c_str(), tx_setString);
                    R((c + ".size:()I").c_str(), tx_size);
                    R((c + ".getMaxSize:()I").c_str(), tx_getMaxSize);
                    R((c + ".setMaxSize:(I)I").c_str(), tx_setMaxSize);
                    R((c + ".getConstraints:()I").c_str(), tx_getConstraints);
                    R((c + ".setConstraints:(I)V").c_str(), tx_setConstraints);
                    R((c + ".getCaretPosition:()I").c_str(), tx_getCaret);
                    R((c + ".insert:(Ljava/lang/String;I)V").c_str(), tx_insert);
                    R((c + ".delete:(II)V").c_str(), tx_delete);
                    R((c + ".getChars:([C)I").c_str(), tx_getChars);
                    R((c + ".setChars:([CII)V").c_str(), tx_setChars);
                    R((c + ".setInitialInputMode:(Ljava/lang/String;)V").c_str(), noop);
                }
                // Gauge / DateField / Spacer
                R("javax/microedition/lcdui/Gauge.<init>:(Ljava/lang/String;ZII)V", ga_init);
                R("javax/microedition/lcdui/Gauge.getValue:()I", ga_getValue);
                R("javax/microedition/lcdui/Gauge.setValue:(I)V", ga_setValue);
                R("javax/microedition/lcdui/Gauge.getMaxValue:()I", ga_getMax);
                R("javax/microedition/lcdui/Gauge.setMaxValue:(I)V", ga_setMax);
                R("javax/microedition/lcdui/Gauge.isInteractive:()Z", ga_isInteractive);
                R("javax/microedition/lcdui/DateField.<init>:(Ljava/lang/String;I)V", date_init);
                R("javax/microedition/lcdui/Spacer.<init>:(II)V", spacer_init);
                R("javax/microedition/lcdui/CustomItem.<init>:(Ljava/lang/String;)V", ci_init);
                R("javax/microedition/lcdui/CustomItem.getInteractionModes:()I", ci_interactionModes);
                R("javax/microedition/lcdui/CustomItem.repaint:()V", ci_repaint);
                R("javax/microedition/lcdui/CustomItem.repaint:(IIII)V", ci_repaint);
                R("javax/microedition/lcdui/CustomItem.invalidate:()V", ci_repaint);
#undef R
            }
        } // namespace detail
    } // namespace midp
} // namespace jvm
