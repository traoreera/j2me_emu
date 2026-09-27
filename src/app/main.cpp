// main.cpp
// Émulateur J2ME/MIDP : charge le .jar, lit le manifeste, instancie le MIDlet
// et exécute son bytecode via l'interpréteur. Le rendu se fait par le HAL
// display (SDL2 côté PC, écran RGB565 côté RP2040).

#include <vector>
#include <ctime>
#include <algorithm>
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

// Repli quand ni le manifeste ni un .jad ne donnent MIDlet-1 (JAR mal packagé) : première classe
// du JAR dont la super-classe est javax.microedition.midlet.MIDlet. PC uniquement (index en RAM).
static std::string findMidletClass(jme::JarReader &jar)
{
    std::vector<uint8_t> buf;
    for (const jme::JarEntry &e : jar.entries())
    {
        if (e.name.size() < 7 || e.name.compare(e.name.size() - 6, 6, ".class") != 0 || e.uncompressedSize == 0)
            continue;
        buf.resize(e.uncompressedSize);
        size_t n = jar.extractEntry(e.name, buf.data(), buf.size());
        jvm::ClassFile cf;
        if (n && jvm::ClassFile::parse(buf.data(), n, cf) && cf.superClassName() == "javax/microedition/midlet/MIDlet")
        {
            std::string c = cf.thisClassName();
            for (auto &ch : c)
                if (ch == '/') ch = '.';
            return c;
        }
    }
    return "";
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
    // GC : borne HAUTE de la pile C++ du fil PRINCIPAL (les piles grandissent vers le bas -- une variable
    // locale prise ici, tout au début, est à l'adresse la plus haute que le programme utilisera sur ce fil).
    // Voir le commentaire de jvm::jme_currentFiberStackTop : quand aucune fibre ne tourne, c'est cette borne
    // qui sert à scanner conservativement la pile C++ active (locales de handlers natifs, etc.) pendant un GC.
    uint8_t mainStackTopProbe;
    uint8_t *const mainStackTop = &mainStackTopProbe;
    jvm::profileInit();
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

    // Descripteur d'application "<jeu>.jad" à côté du .jar (facultatif) : certains JAR (ex. Akatis,
    // packagé par Ant) n'ont pas MIDlet-1 dans le manifeste, il n'est que dans le .jad. Le .jad
    // complète le manifeste (et ses attributs sont lisibles via getAppProperty, comme sur un vrai téléphone).
    std::string jadText;
    {
        std::string jadPath(jarPath);
        if (jadPath.size() > 4 && jadPath.compare(jadPath.size() - 4, 4, ".jar") == 0)
            jadPath.resize(jadPath.size() - 4);
        jadPath += ".jad";
        if (FILE *jf = fopen(jadPath.c_str(), "rb"))
        {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, jf)) > 0)
                jadText.append(buf, n);
            fclose(jf);
            fprintf(stderr, "[jad] descripteur charge : %s\n", jadPath.c_str());
        }
    }
    jme::ManifestInfo manifest;
    if (!jar.readManifest(manifest))
    {
        if (jadText.empty() || !jme::parseManifestText(jadText.data(), jadText.size(), manifest))
        {
            manifest.mainClass = findMidletClass(jar);
            if (manifest.mainClass.empty())
            {
                fprintf(stderr, "MANIFEST.MF invalide ou MIDlet-1 absent (ni .jad voisin, ni classe MIDlet)\n");
                return 1;
            }
            fprintf(stderr, "[manifest] MIDlet-1 absent : classe MIDlet detectee = %s\n", manifest.mainClass.c_str());
            manifest.valid = true;
        }
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
    hal::display_set_title(("J2ME Emu - " + manifest.midletName).c_str());
    // JME_AUDIO_TEST=1 : bip de 0,8 s à 440 Hz au démarrage (vérifie la chaîne audio de bout en bout).
    if (const char *at = getenv("JME_AUDIO_TEST"))
        if (atoi(at) != 0)
            kernel::audio::playTone(440.f, 800, 0.5f);
    float masterVol = 1.0f, volBeforeMute = 1.0f;
    bool muted = false;
    int osdFrames = 0;
    char osdText[32] = "";

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
    {
        char stackProbe;
        interp.setStackLow(&stackProbe - 6 * 1024 * 1024); // pile principale : 8 Mo par défaut, on garde 2 Mo de marge
    }
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
    // Ramasse-miettes : marquage-balayage conservateur (voir Heap::collectGarbage dans core/runtime.cpp).
    // Racines : piles/locales de l'exécution Java active (interp.scanActiveFrames -- fil principal ou fibre en
    // cours), `statics` de chaque classe chargée, et tout ce que retient nativement la couche MIDP (écrans
    // lcdui, threads, sons, minuteries -- jvm::midp::gcMarkRoots). JME_GC=0 le désactive (comportement
    // d'avant l'introduction du GC : le tas ne fait plus jamais que grossir) -- filet de sécurité.
    if (!getenv("JME_GC") || atoi(getenv("JME_GC")) != 0)
    {
        rt.heap().setRootScanner([&rt, &interp, mainStackTop](jvm::Heap::Marker &m) {
            interp.scanActiveFrames(m);
            rt.forEachClass([&m](jvm::ClassInfo *ci) {
                if (!ci->statics.empty())
                    m.scan(ci->statics.data(), ci->statics.size() * sizeof(jvm::Value));
            });
            jvm::midp::gcMarkRoots(m);
            // Pile C++ RÉELLEMENT active à cet instant (fibre en cours, sinon le fil principal) : couvre les
            // variables locales des handlers natifs (ex. un `Obj*` fraîchement alloué pas encore écrit dans
            // un champ Java -- voir le commentaire complet dans natives.cpp). Bornée à kStackScanWindow
            // (généreux : des centaines d'appels C++ imbriqués) plutôt que de remonter jusqu'au sommet réel
            // de la pile (8 Mo) : plus la fenêtre est large, plus elle contient de vieilles cases de pile
            // ABANDONNÉES par des appels déjà retournés (jamais réécrites depuis, car un appel plus récent au
            // même niveau n'a pas eu besoin d'autant de place) qui gardent encore le bit à bit d'un ANCIEN
            // `Obj*` -- ces cases sont scannées comme n'importe quelle autre et gardent alors l'objet vivant
            // pour toujours, même longtemps après que plus rien (Java ou C++) ne le référence réellement.
            // Mesuré sur games/os_pacman.jar : sans cette borne, le GC ne récupérait quasiment plus rien après
            // quelques cycles (0 octet récupéré) malgré un tas exigu qui finissait quand même par s'épuiser --
            // une fenêtre de 256 Ko (des centaines de frames C++, bien plus que la profondeur d'un handler
            // natif) restitue une vraie collecte tout en couvrant tous les cas réels observés.
            static constexpr size_t kStackScanWindow = 256 * 1024;
            uint8_t *stackTop = mainStackTop;
            jvm::jme_currentFiberStackTop(stackTop);
            uint8_t currentSpProbe;
            uint8_t *sp = &currentSpProbe;
            if (stackTop > sp && static_cast<size_t>(stackTop - sp) > kStackScanWindow)
                stackTop = sp + kStackScanWindow;
            // Alignement sur 8 octets (Marker::scan lit des mots de 8 octets) : arrondi vers le haut, en
            // perdant au plus 7 octets tout en bas de la région -- négligeable, une vraie référence stockée
            // dans une variable C++ y est de toute façon naturellement alignée.
            sp = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(sp) + 7) & ~uintptr_t(7));
            if (sp < stackTop)
                m.scan(sp, static_cast<size_t>(stackTop - sp));
        });
    }
    jvm::midp::setAppProperty("MIDlet-Name", manifest.midletName);
    jvm::midp::setAppProperty("MIDlet-Version", manifest.midletVersion);
    jvm::midp::setAppProperty("MIDlet-Vendor", manifest.midletVendor);
    // Tous les attributs du manifeste (et du .jad s'il était fourni) sont lisibles
    // via MIDlet.getAppProperty() -- pas seulement les trois ci-dessus.
    {
        static uint8_t mfBuf[16384];
        size_t n = jar.extractEntry("META-INF/MANIFEST.MF", mfBuf, sizeof(mfBuf));
        auto exposeProps = [](const std::string &mf)
        {
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
        };
        exposeProps(std::string(reinterpret_cast<char *>(mfBuf), n));
        exposeProps(jadText); // le .jad prime sur le manifeste
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

    // startApp() : appel synchrone, sans fibre ni yield -> garde-fou anti-boucle infinie (1,5 G d'instructions,
    // largement au-delà d'un démarrage réel) pour ne jamais figer toute la fenêtre.
    interp.setInstrBudget(1500000000LL);
    bool startOk = interp.invokeVirtual(mainCls, "startApp", "()V", midlet, &thisV, 1, res);
    interp.setInstrBudget(-1);
    if (!startOk)
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

    // JME_AUTOTEXT="texte" + JME_AUTOTEXTFRAME=n : injecte du texte tapé (comme SDL_TEXTINPUT) dans le
    // TextField/TextBox actif à la trame n -- utile pour traverser en headless les écrans de saisie
    // (nom de joueur...) qu'aucun softkey/clic ne peut remplir.
    const char *autoText = getenv("JME_AUTOTEXT");
    int autoTextFrame = 0;
    if (const char *atf = getenv("JME_AUTOTEXTFRAME"))
        autoTextFrame = atoi(atf);

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
    // Cadence : l'horloge du jeu suit le temps réel (cf. midp::tick), la fréquence de trame ne change donc pas la
    // vitesse du jeu, seulement la finesse (les sleep() sont honorés à leur instant exact dans la trame). Défaut 16 ms
    // (~60 trames/s). Avec le vsync actif, le flip bloque déjà ~1/60 s : aucune attente en plus (elle décalerait
    // l'image d'un vsync et donnerait 33/50 ms en alternance) ; seule une garde de 4 ms évite l'emballement si le
    // vsync ne bloque pas (fenêtre cachée).
    uint32_t kFrameBudgetMs = 16;
    bool frameBudgetForced = false;
    if (const char *fb = getenv("JME_FRAME_BUDGET"))
        if (atoi(fb) > 0)
        {
            kFrameBudgetMs = static_cast<uint32_t>(atoi(fb));
            frameBudgetForced = true;
        }
    const bool vsyncPacing = !frameBudgetForced && hal::display_vsync_active();
    uint32_t lateFrames = 0;
    uint32_t frameStart = SDL_GetTicks();
    // JME_RENDER_STATS : durée de traitement de chaque trame (hors attente) pour repérer les à-coups.
    std::vector<uint16_t> workMs;   // temps de traitement (entrée + JVM + rendu + flip)
    std::vector<uint16_t> cpuMs;    // idem en temps CPU du processus (insensible à la charge de la machine)
    std::vector<uint16_t> periodMs; // intervalle réel entre deux débuts de trame
    auto cpuNowUs = []() -> int64_t {
        timespec ts;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
    };
    int64_t cpuFrameStart = cpuNowUs();
    uint32_t prevStart = frameStart;

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

        if (autoText && frame == autoTextFrame)
            jvm::midp::setTextInput(autoText, 0);
        else
            jvm::midp::setTextInput(input.text, input.backspaces);
        jvm::midp::tick(input.pressed, input.justPressed, input.justReleased);

        if (jvm::midp::midletDestroyed())
            break;

        frame++;
        if (maxFrames > 0 && frame >= maxFrames)
            break;

        // Volume maître : F9/F10 (PgBas/PgHaut) par pas de 10 %, F8 = muet. Petit OSD ~1,5 s.
        if (input.volumeStep || input.muteToggle)
        {
            if (input.muteToggle)
            {
                muted = !muted;
            }
            if (input.volumeStep)
            {
                muted = false;
                masterVol = std::max(0.0f, std::min(1.0f, masterVol + 0.1f * input.volumeStep));
            }
            kernel::audio::setMasterVolume(muted ? 0.0f : masterVol);
            snprintf(osdText, sizeof osdText, muted ? "MUET" : "VOL %d%%", (int)(masterVol * 100 + 0.5f));
            osdFrames = 45;
        }
        if (osdFrames > 0)
        {
            osdFrames--;
            hal::Framebuffer *ofb = hal::display_get_framebuffer();
            int tw = (int)strlen(osdText) * 6 + 4;
            if (ofb->width >= tw + 4)
            {
                hal::display_fill_rect(2, 2, tw, 11, 0x0000);
                hal::display_draw_text(4, 4, osdText, muted ? 0xF800 : 0xFFFF);
            }
        }

        const uint32_t beforeFlip = SDL_GetTicks() - frameStart;
        cpuMs.push_back(static_cast<uint16_t>(std::min<int64_t>((cpuNowUs() - cpuFrameStart) / 1000, 65535)));
        hal::display_flip();

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
        workMs.push_back(static_cast<uint16_t>(std::min<uint32_t>(beforeFlip, 65535))); // hors attente du vsync
        if (beforeFlip > 60 && getenv("JME_RENDER_STATS"))
            fprintf(stderr, "[stats] trame %d lente : %u ms\n", frame, beforeFlip);
        const uint32_t target = vsyncPacing ? 4 : kFrameBudgetMs;
        if (elapsed < target)
            SDL_Delay(target - elapsed);
        else if (elapsed > kFrameBudgetMs * 2)
            lateFrames++;
        frameStart = SDL_GetTicks();
        cpuFrameStart = cpuNowUs();
        periodMs.push_back(static_cast<uint16_t>(std::min<uint32_t>(frameStart - prevStart, 65535)));
        prevStart = frameStart;
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
    jvm::profileReport();
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
            auto pct = [](std::vector<uint16_t> v, double q) -> unsigned {
                if (v.empty()) return 0;
                std::sort(v.begin(), v.end());
                return v[std::min(v.size() - 1, static_cast<size_t>(q * v.size()))];
            };
            if (!workMs.empty())
            {
                double sum = 0;
                for (uint16_t w : workMs) sum += w;
                unsigned over66 = 0, over100 = 0;
                for (uint16_t w : workMs) { over66 += w > 66; over100 += w > 100; }
                printf("[stats] traitement/trame ms : moy=%.1f p50=%u p95=%u p99=%u max=%u ; >66ms:%u >100ms:%u\n",
                       sum / workMs.size(), pct(workMs, 0.5), pct(workMs, 0.95), pct(workMs, 0.99), pct(workMs, 1.0), over66, over100);
                printf("[stats] CPU/trame ms (processus entier) : moy=%.1f p50=%u p95=%u p99=%u max=%u\n",
                       [&]() { double t = 0; for (uint16_t c : cpuMs) t += c; return cpuMs.empty() ? 0.0 : t / cpuMs.size(); }(),
                       pct(cpuMs, 0.5), pct(cpuMs, 0.95), pct(cpuMs, 0.99), pct(cpuMs, 1.0));
                printf("[stats] intervalle reel entre trames ms : p50=%u p95=%u max=%u\n",
                       pct(periodMs, 0.5), pct(periodMs, 0.95), pct(periodMs, 1.0));
            }
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
