// main.cpp
// Émulateur J2ME/MIDP : charge le .jar, lit le manifeste, instancie le MIDlet
// et exécute son bytecode via l'interpréteur. Le rendu se fait par le HAL
// display (SDL2 côté PC, écran RGB565 côté RP2040).

#include <vector>
#include "hal/jar_reader.h"
#include "hal/display.h"
#include "hal/input.h"
#include "hal/png.h"
#include "core/class_file.h"
#include "core/runtime.h"
#include "core/interpreter.h"
#include "core/native.h"
#include "midp/midp.h"
#include "kernel/kernel.h"
#include "kernel/audio/audio.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <SDL2/SDL.h>
#include <unistd.h>
#include "app/launcher.h"

static std::string toInternal(const std::string &name)
{
    std::string s = name;
    for (auto &c : s)
        if (c == '.')
            c = '/';
    return s;
}

// JME_AUDIO=sdl|stub|off ; défaut : stub si CI headless, sinon sdl.
static const kernel::audio::Device *pickAudioDevice()
{
    const char *a = getenv("JME_AUDIO");
    if (a)
    {
        if (strcmp(a, "sdl") == 0)
            return &kernel::audio::kSdlDevice;
        return &kernel::audio::kStubDevice;
    }
    const char *vd = getenv("SDL_VIDEODRIVER");
    if (vd && strcmp(vd, "dummy") == 0)
        return &kernel::audio::kStubDevice;
    return &kernel::audio::kSdlDevice;
}

static_assert(sizeof(void *) == 8, "build 64 bits requis (Pi Zero 2 W : AArch64)");

// Relance ce même binaire (launcher <-> jeu) : état propre à chaque jeu, sans
// avoir à réinitialiser Runtime/heap/globales MIDP dans le même processus.
static void reexec(const char *jar)
{
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0)
        return;
    self[n] = 0;
    if (jar)
        execl(self, self, jar, (char *)nullptr);
    else
        execl(self, self, (char *)nullptr);
    perror("execl");
}

int main(int argc, char **argv)
{
    // Sans argument : launcher (JME_LAUNCHER=0 pour l'ancien défaut games/assasin.jar).
    const bool launcherOff = getenv("JME_LAUNCHER") && atoi(getenv("JME_LAUNCHER")) == 0;
    if (argc <= 1 && !launcherOff)
    {
        const char *gd = getenv("JME_GAMES_DIR");
        const char *last = getenv("JME_LAUNCHER_LAST");
        std::string chosen = launcher::run(gd ? gd : "games", last ? last : "");
        if (chosen.empty())
            return 0;
        setenv("JME_FROM_LAUNCHER", "1", 1);
        setenv("JME_LAUNCHER_LAST", chosen.c_str(), 1);
        reexec(chosen.c_str());
        return 1;
    }
    const bool fromLauncher = getenv("JME_FROM_LAUNCHER") != nullptr;
    std::vector<std::string> confKeys; // variables posées par le .conf (à retirer avant de relancer le launcher)
    bool leaveByQuit = false;
    const char *jarPath = (argc > 1) ? argv[1] : "games/assasin.jar";

    std::vector<std::pair<std::string, std::string>> confProps; // lignes `PROP:Nom=valeur`
    // Profil par jeu : "<jeu>.conf" à côté du .jar, une variable par ligne au
    // format des variables d'environnement (`JME_WIDTH=480`, `JME_HEIGHT=800`,
    // `JME_FRAME_TIME=16`...), `#` = commentaire. Évite de retaper la résolution
    // exigée par chaque MIDlet à chaque lancement. Une variable DÉJÀ définie dans
    // l'environnement l'emporte (surcharge ponctuelle en ligne de commande).
    {
        std::string confPath(jarPath);
        if (confPath.size() > 4 && confPath.compare(confPath.size() - 4, 4, ".jar") == 0)
            confPath.resize(confPath.size() - 4);
        confPath += ".conf";
        if (FILE *cf = fopen(confPath.c_str(), "r"))
        {
            char line[256];
            while (fgets(line, sizeof(line), cf))
            {
                std::string l(line);
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ' || l.back() == '\t'))
                    l.pop_back();
                size_t b = l.find_first_not_of(" \t");
                if (b == std::string::npos || l[b] == '#')
                    continue;
                l = l.substr(b);
                size_t eq = l.find('=');
                if (eq == std::string::npos || eq == 0)
                    continue;
                std::string k = l.substr(0, eq), v = l.substr(eq + 1);
                while (!k.empty() && (k.back() == ' ' || k.back() == '\t'))
                    k.pop_back();
                if (k.compare(0, 5, "PROP:") == 0)
                {
                    // attribut de .jad : lisible via MIDlet.getAppProperty(nom)
                    confProps.emplace_back(k.substr(5), v);
                    continue;
                }
                if (k.compare(0, 4, "JME_") != 0 && k != "SDL_VIDEODRIVER")
                    continue; // on ne touche qu'aux variables de l'émulateur
                if (!getenv(k.c_str()))
                    confKeys.push_back(k);
                setenv(k.c_str(), v.c_str(), 0); // 0 = ne pas écraser l'environnement
            }
            fclose(cf);
            fprintf(stderr, "[conf] profil chargé : %s\n", confPath.c_str());
        }
    }

    jme::JarReader jar;
    if (!jar.open(jarPath))
    {
        fprintf(stderr, "Impossible d'ouvrir %s\n", jarPath);
        return 1;
    }

    jme::ManifestInfo manifest;
    if (!jar.readManifest(manifest))
    {
        fprintf(stderr, "MANIFEST.MF invalide ou MIDlet-1 absent\n");
        return 1;
    }

    printf("MIDlet: %s (%s) - classe principale: %s\n",
           manifest.midletName.c_str(),
           manifest.midletVersion.c_str(),
           manifest.mainClass.c_str());

    hal::DisplayConfig cfg{240, 320};
    if (const char *w = getenv("JME_WIDTH"))
        cfg.width = atoi(w);
    if (const char *h = getenv("JME_HEIGHT"))
        cfg.height = atoi(h);
    if (!hal::display_init(&cfg))
    {
        fprintf(stderr, "Echec init display\n");
        return 1;
    }
    hal::input_init();

    // ---- Boot du noyau : horloge + pilote audio ----
    kernel::setMillisProvider([]() -> kernel::Ms { return (kernel::Ms)SDL_GetTicks(); });
    const kernel::audio::Device *adev = pickAudioDevice();
    kernel::audio::useDevice(adev);
    kernel::Driver audioDrv{};
    audioDrv.name = "audio";
    audioDrv.backend = adev->name;
    audioDrv.init = [](kernel::Driver *d) -> int {
        (void)d;
        const kernel::audio::Device *dev = kernel::audio::activeDevice();
        return (dev && dev->open) ? dev->open(dev) : -1;
    };
    audioDrv.shutdown = [](kernel::Driver *d) {
        (void)d;
        const kernel::audio::Device *dev = kernel::audio::activeDevice();
        if (dev && dev->close)
            dev->close(dev);
    };
    kernel::driverRegister(&audioDrv);
    kernel::kernelBoot(0);

    size_t heapSize = jvm::Heap::kDefaultPoolSize;
    if (const char *hs = getenv("JME_HEAP"))
        heapSize = static_cast<size_t>(atol(hs)) * 1024;
    // JME_HEAP_MAX (KiB) : plafond dur de la capacité totale (profil Pi : 131072).
    size_t heapMax = 0;
    if (const char *hm = getenv("JME_HEAP_MAX"))
    {
        long v = atol(hm);
        if (v <= 0)
        {
            fprintf(stderr, "JME_HEAP_MAX invalide: '%s' (KiB attendus)\n", hm);
            return 1;
        }
        heapMax = static_cast<size_t>(v) * 1024;
        if (heapMax < heapSize)
            fprintf(stderr, "[heap] JME_HEAP_MAX < JME_HEAP : plafond releve a %zu KiB\n", heapSize / 1024);
    }
    jvm::Runtime rt(heapSize, heapMax);
    jvm::Interpreter interp(&rt);
    rt.setJar(&jar);

    jvm::initNatives();
    // RecordStore persistant : "<jeu>.rms/" à côté du .jar (ou JME_RMSDIR=chemin ;
    // JME_RMS=0 pour rester purement en mémoire, ex. tests reproductibles).
    {
        const char *off = getenv("JME_RMS");
        if (!(off && strcmp(off, "0") == 0))
        {
            std::string dir;
            if (const char *d = getenv("JME_RMSDIR"))
                dir = d;
            else
            {
                dir = jarPath;
                if (dir.size() > 4 && dir.compare(dir.size() - 4, 4, ".jar") == 0)
                    dir.resize(dir.size() - 4);
                dir += ".rms";
            }
            jvm::setRmsDir(dir);
        }
    }
    jvm::midp::init(&rt, &interp);
    jvm::midp::setAppProperty("MIDlet-Name", manifest.midletName);
    jvm::midp::setAppProperty("MIDlet-Version", manifest.midletVersion);
    jvm::midp::setAppProperty("MIDlet-Vendor", manifest.midletVendor);
    // Tous les attributs du manifeste (et du .jad s'il était fourni) sont lisibles
    // via MIDlet.getAppProperty() -- pas seulement les trois ci-dessus.
    {
        static uint8_t mfBuf[16384];
        size_t n = jar.extractEntry("META-INF/MANIFEST.MF", mfBuf, sizeof(mfBuf));
        std::string mf(reinterpret_cast<char *>(mfBuf), n);
        size_t pos = 0;
        while (pos < mf.size())
        {
            size_t eol = mf.find('\n', pos);
            if (eol == std::string::npos) eol = mf.size();
            std::string line = mf.substr(pos, eol - pos);
            pos = eol + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0 || line[0] == ' ') continue;
            std::string k = line.substr(0, colon), v = line.substr(colon + 1);
            size_t b = v.find_first_not_of(' ');
            v = b == std::string::npos ? "" : v.substr(b);
            if (k == "MIDlet-Name" || k == "MIDlet-Version" || k == "MIDlet-Vendor") continue;
            jvm::midp::setAppProperty(k, v);
        }
        for (const auto &kv : confProps)
            jvm::midp::setAppProperty(kv.first, kv.second);
    }

    std::string mainInternal = toInternal(manifest.mainClass);
    jvm::ClassInfo *mainCls = rt.loadFromJar(mainInternal);
    if (!mainCls)
    {
        fprintf(stderr, "Echec chargement de la classe %s\n", mainInternal.c_str());
        hal::input_shutdown();
        hal::display_shutdown();
        return 1;
    }

    // <init> de la classe principale (instancie le MIDlet)
    jvm::Obj *midlet = rt.heap().newInstance(mainCls);
    if (!midlet)
    {
        fprintf(stderr, "Echec allocation de l'objet MIDlet\n");
        hal::input_shutdown();
        hal::display_shutdown();
        return 1;
    }
    jvm::Value thisV = jvm::Value::fromRef(midlet);
    jvm::Value res;
    if (!interp.invokeSpecial(mainCls, "<init>", "()V", midlet, &thisV, 1, res))
    {
        fprintf(stderr, "Echec instanciation du MIDlet\n");
        hal::input_shutdown();
        hal::display_shutdown();
        return 1;
    }
    if (midlet->kind != jvm::ObjKind::Instance)
    {
        fprintf(stderr, "Instanciation MIDlet: objet invalide\n");
        hal::input_shutdown();
        hal::display_shutdown();
        return 1;
    }

    // startApp()
    if (!interp.invokeVirtual(mainCls, "startApp", "()V", midlet, &thisV, 1, res))
    {
        fprintf(stderr, "Echec startApp\n");
    }

    // ---- Test chargement ressources PNG depuis le jar ----
    if (!jvm::midp::midletDestroyed())
    {
        jme::JarReader *jar = rt.jar();
        if (jar)
        {
            const char *testNames[] = {"3", "14", "dataIGP", nullptr};
            for (int t = 0; testNames[t]; t++)
            {
                jme::JarEntry e;
                if (jar->findEntry(testNames[t], e) && e.uncompressedSize <= 4u * 1024 * 1024)
                {
                    uint8_t *buf = (uint8_t *)malloc(e.uncompressedSize);
                    if (buf)
                    {
                        size_t n = jar->extractEntry(testNames[t], buf, e.uncompressedSize);
                        if (n > 0 && n <= e.uncompressedSize)
                        {
                            jme::PngHeader hdr;
                            if (jme::pngHeader(buf, n, hdr))
                            {
                                // Remplir le framebuffer de blanc pour valider le décodeur
                                uint16_t *fb = hal::display_get_framebuffer()->pixels;
                                for (int y = 0; y < 320 && y < hdr.h; y++)
                                    for (int x = 0; x < 240 && x < hdr.w; x++)
                                        fb[y * 240 + x] = 0xFFFF; // Blanc RGB565
                            }
                        }
                        free(buf);
                    }
                }
                break; // une ressource suffit pour le test
            }
        }
    }

    bool running = true;
    hal::InputState input{};
    int frame = 0;
    int maxFrames = -1;
    if (const char *mf = getenv("JME_MAXFRAMES"))
        maxFrames = atoi(mf);

    uint32_t autoKey = 0;
    int autoKeyFrame = -1;
    if (const char *ak = getenv("JME_AUTOKEY"))
    {
        std::string s(ak);
        if (s == "5")
            autoKey = hal::KEY_5;
        else if (s == "0")
            autoKey = hal::KEY_0;
        else if (s == "*")
            autoKey = hal::KEY_STAR;
        else if (s == "#")
            autoKey = hal::KEY_HASH;
        else if (s == "FIRE")
            autoKey = hal::KEY_FIRE;
        else if (s == "SOFT1")
            autoKey = hal::KEY_SOFT1;
        else if (s == "SOFT2")
            autoKey = hal::KEY_SOFT2;
        else if (s == "LEFT")
            autoKey = hal::KEY_LEFT;
        else if (s == "RIGHT")
            autoKey = hal::KEY_RIGHT;
        else if (s == "UP")
            autoKey = hal::KEY_UP;
        else if (s == "DOWN")
            autoKey = hal::KEY_DOWN;
    }
    if (const char *akf = getenv("JME_AUTOKEYFRAME"))
        autoKeyFrame = atoi(akf);

    uint32_t holdKey = 0;
    int holdKeyFrame = -1;
    if (const char *hk = getenv("JME_AUTOHOLD"))
    {
        std::string s(hk);
        if (s == "5")
            holdKey = hal::KEY_5;
        else if (s == "0")
            holdKey = hal::KEY_0;
        else if (s == "*")
            holdKey = hal::KEY_STAR;
        else if (s == "#")
            holdKey = hal::KEY_HASH;
        else if (s == "FIRE")
            holdKey = hal::KEY_FIRE;
        else if (s == "SOFT1")
            holdKey = hal::KEY_SOFT1;
        else if (s == "SOFT2")
            holdKey = hal::KEY_SOFT2;
        else if (s == "LEFT")
            holdKey = hal::KEY_LEFT;
        else if (s == "RIGHT")
            holdKey = hal::KEY_RIGHT;
        else if (s == "UP")
            holdKey = hal::KEY_UP;
        else if (s == "DOWN")
            holdKey = hal::KEY_DOWN;
    }
    if (const char *hkf = getenv("JME_AUTOHOLDFRAME"))
        holdKeyFrame = atoi(hkf);

    struct Tap
    {
        int fr;
        uint32_t bits;
    };
    std::vector<Tap> autoTaps;
    if (const char *ats = getenv("JME_AUTOTAPS"))
    {
        std::string seq(ats);
        size_t pos = 0;
        while (pos < seq.size())
        {
            size_t sep = seq.find(',', pos);
            std::string item = seq.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
            pos = (sep == std::string::npos) ? seq.size() : sep + 1;
            size_t colon = item.find(':');
            if (colon == std::string::npos)
                continue;
            int fr = atoi(item.substr(0, colon).c_str());
            std::string k = item.substr(colon + 1);
            uint32_t bits = 0;
            if (k == "5")
                bits = hal::KEY_5;
            else if (k == "0")
                bits = hal::KEY_0;
            else if (k == "*")
                bits = hal::KEY_STAR;
            else if (k == "#")
                bits = hal::KEY_HASH;
            else if (k == "FIRE")
                bits = hal::KEY_FIRE;
            else if (k == "SOFT1")
                bits = hal::KEY_SOFT1;
            else if (k == "SOFT2")
                bits = hal::KEY_SOFT2;
            else if (k == "LEFT")
                bits = hal::KEY_LEFT;
            else if (k == "RIGHT")
                bits = hal::KEY_RIGHT;
            else if (k == "UP")
                bits = hal::KEY_UP;
            else if (k == "DOWN")
                bits = hal::KEY_DOWN;
            autoTaps.push_back({fr, bits});
        }
    }

    // Mapping clavier remplaçable : JME_KEYMAP (liste "touche=TOKEN,...") prime sur
    // un éventuel fichier "<jeu>.keys" placé à côté du .jar (une ligne par entrée).
    {
        std::string mapSpec;
        if (const char *envMap = getenv("JME_KEYMAP"))
        {
            mapSpec = envMap;
        }
        else
        {
            std::string keysPath(jarPath);
            if (keysPath.size() > 4 && keysPath.compare(keysPath.size() - 4, 4, ".jar") == 0)
                keysPath.replace(keysPath.size() - 4, 4, ".keys");
            FILE *kf = fopen(keysPath.c_str(), "r");
            if (kf)
            {
                char line[128];
                while (fgets(line, sizeof(line), kf))
                {
                    mapSpec += line;
                    mapSpec += '\n';
                }
                fclose(kf);
            }
        }
        if (!mapSpec.empty())
            hal::input_applyKeyMap(mapSpec.c_str());
    }

    int autoTouchX = -1, autoTouchY = -1, autoTouchFrame = -1;
    if (const char *at = getenv("JME_AUTOTOUCH"))
    {
        if (std::sscanf(at, "%d,%d", &autoTouchX, &autoTouchY) != 2)
        {
            autoTouchX = -1;
            autoTouchY = -1;
        }
        if (const char *atf = getenv("JME_AUTOTOUCHFRAME"))
            autoTouchFrame = atoi(atf);
    }

    // JME_AUTOTOUCHES="x,y,frame;x,y,frame;..." : plusieurs clics simulés.
    struct ScriptTouch { int x, y, f; };
    std::vector<ScriptTouch> scriptTouches;
    if (const char *ats = getenv("JME_AUTOTOUCHES"))
    {
        const char *p = ats;
        ScriptTouch st;
        int n = 0;
        while (std::sscanf(p, "%d,%d,%d%n", &st.x, &st.y, &st.f, &n) == 3)
        {
            scriptTouches.push_back(st);
            p += n;
            if (*p == ';') p++;
            else break;
        }
    }

    // Budget RÉEL d'une trame (ms). JME_FRAME_BUDGET ; ne pas confondre avec
    // JME_FRAME_TIME (durée VIRTUELLE vue par l'horloge du jeu).
    uint32_t kFrameBudgetMs = 33; // ~30 fps cible
    if (const char *fb = getenv("JME_FRAME_BUDGET"))
        if (atoi(fb) > 0)
            kFrameBudgetMs = static_cast<uint32_t>(atoi(fb));
    uint32_t lateFrames = 0;
    uint32_t frameStart = SDL_GetTicks();

    while (running)
    {
        hal::input_poll(&input);

        if (input.quit)
        {
            if (getenv("JME_DEBUG"))
                fprintf(stderr, "BREAK: input.quit a la frame %d\n", frame);
            leaveByQuit = !input.exitToMenu; // Ctrl+Q / fermeture fenêtre = quitter pour de bon
            break;
        }

        if (autoKey)
        {
            bool hold = (autoKeyFrame < 0);
            input.pressed |= autoKey;
            if (hold ? (frame < 1) : (frame == autoKeyFrame))
                input.justPressed |= autoKey;
            if (!hold && frame == autoKeyFrame)
                input.pressed &= ~autoKey;
        }

        if (holdKey && holdKeyFrame >= 0 && frame >= holdKeyFrame)
        {
            if (frame == holdKeyFrame)
                input.justPressed |= holdKey;
            input.pressed |= holdKey;
        }

        for (const Tap &t : autoTaps)
        {
            if (t.fr == frame)
            {
                input.pressed |= t.bits;
                input.justPressed |= t.bits;
            }
            // La touche ne reste "pressed" qu'une trame (input_poll() la
            // remet à zéro dès la trame suivante en l'absence d'un vrai
            // évènement clavier, cf. hal/input.cpp) mais ça ne génère PAS de
            // justReleased -- un jeu dont keyPressed/keyReleased s'équilibrent
            // (ex. des compteurs incrémentés/décrémentés en paire) ne voit
            // donc jamais le relâchement d'un tap simulé. On le simule
            // explicitement une trame après le tap.
            if (t.fr + 1 == frame)
                input.justReleased |= t.bits;
        }

        // Tap simulé : appui à la trame N, relâchement 3 trames plus tard --
        // comme un vrai clic. Presser et relâcher dans la MÊME trame ne suffit
        // pas pour les jeux qui mémorisent l'état du pointeur et le lisent
        // depuis leur propre thread (ils ne voient jamais l'appui).
        if (autoTouchX >= 0 && autoTouchY >= 0)
        {
            int f0 = autoTouchFrame < 0 ? 0 : autoTouchFrame;
            if (frame == f0)
                jvm::midp::pointerEvent(0, autoTouchX, autoTouchY);
            else if (frame == f0 + 3)
                jvm::midp::pointerEvent(1, autoTouchX, autoTouchY);
        }

        for (const ScriptTouch &t : scriptTouches)
        {
            if (frame == t.f)
                jvm::midp::pointerEvent(0, t.x, t.y);
            else if (frame == t.f + 3)
                jvm::midp::pointerEvent(1, t.x, t.y);
        }

        for (int i = 0; i < input.pointerCount; i++)
            jvm::midp::pointerEvent(input.pointer[i].kind, input.pointer[i].x, input.pointer[i].y);

        jvm::midp::tick(input.pressed, input.justPressed, input.justReleased);

        if (jvm::midp::midletDestroyed())
            break;

        frame++;
        if (maxFrames > 0 && frame >= maxFrames)
            break;

        hal::display_present(hal::display_get_framebuffer());

        // Rythme de trame adaptatif : on ne dort que le temps restant du
        // budget de trame (au lieu d'un SDL_Delay(16) fixe qui s'ajoutait
        // systématiquement au temps de traitement, quelle que soit sa durée
        // -- garantissant un plafond ~62 fps même quand le traitement est
        // rapide, et surtout transformant toute variation du temps
        // d'interprétation bytecode/rendu en saccades visibles puisque la
        // trame totale = temps_variable + 16ms_fixe). Si le traitement a
        // déjà dépassé le budget, on ne dort pas du tout (rattrapage) plutôt
        // que d'accumuler du retard trame après trame.
        uint32_t elapsed = SDL_GetTicks() - frameStart;
        if (elapsed < kFrameBudgetMs)
            SDL_Delay(kFrameBudgetMs - elapsed);
        else
            lateFrames++;
        frameStart = SDL_GetTicks();
    }

    if (const char *dump = getenv("JME_DUMP"))
    {
        auto *fb = hal::display_get_framebuffer();
        FILE *f = fopen(dump, "wb");
        if (f)
        {
            int w = fb->width, h = fb->height;
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = 0; y < h; y++)
            {
                const uint16_t *row = fb->pixels + (size_t)y * fb->stride;
                for (int x = 0; x < w; x++)
                {
                    uint16_t p = row[x];
                    uint8_t r = (uint8_t)((p >> 11) << 3);
                    uint8_t g = (uint8_t)(((p >> 5) & 0x3F) << 2);
                    uint8_t b = (uint8_t)((p & 0x1F) << 3);
                    fputc(r, f);
                    fputc(g, f);
                    fputc(b, f);
                }
            }
            fclose(f);
        }
    }

    kernel::kernelShutdown(0);
    hal::input_shutdown();
    hal::display_shutdown();
    printf("Emulation terminee apres %d frames\n", frame);
    // Lancé depuis le launcher : fin de jeu (F12, notifyDestroyed) -> retour au menu.
    if (fromLauncher && !leaveByQuit && !getenv("JME_MAXFRAMES"))
    {
        for (const std::string &k : confKeys)
            unsetenv(k.c_str());
        reexec(nullptr);
    }
    if (const char *rs = getenv("JME_RENDER_STATS"))
        if (atoi(rs) != 0)
        {
            long rssKb = 0, hwmKb = 0;
            if (FILE *sf = fopen("/proc/self/status", "r"))
            {
                char line[256];
                while (fgets(line, sizeof line, sf))
                {
                    sscanf(line, "VmRSS: %ld", &rssKb);
                    sscanf(line, "VmHWM: %ld", &hwmKb);
                }
                fclose(sf);
            }
            printf("[stats] frames=%d en_retard=%u (budget %u ms) heap=%zu/%zu KiB RSS=%ld KiB pic=%ld KiB\n",
                   frame, lateFrames, kFrameBudgetMs, rt.heap().used() / 1024,
                   rt.heap().capacity() / 1024, rssKb, hwmKb);
            if (hwmKb > 256 * 1024)
                printf("[stats] ATTENTION: pic RSS > 256 MiB (seuil d'alerte Pi Zero 2)\n");
        }
    if (getenv("JME_DEBUG"))
    {
        printf("[dbg] ecritures framebuffer: %d\n", jvm::midp::jme_screenWrites());
        printf("[dbg] dessins ciblant ecran: %d\n", jvm::midp::jme_screenPix());
        printf("[dbg] dessins ciblant canvas565: %d\n", jvm::midp::jme_canvasPix());
        printf("[dbg] flushGraphics: %d\n", jvm::midp::jme_flushCalls());
    }
    return 0;
}
