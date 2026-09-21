// main.cpp
// Émulateur J2ME/MIDP : charge le .jar, lit le manifeste, instancie le MIDlet
// et exécute son bytecode via l'interpréteur. Le rendu se fait par le HAL
// display (SDL2 côté PC, écran RGB565 côté RP2040).

#include "hal/jar_reader.h"
#include "hal/display.h"
#include "hal/input.h"
#include "hal/png.h"
#include "vm/class_file.h"
#include "vm/runtime.h"
#include "vm/interpreter.h"
#include "vm/native.h"
#include "vm/midp.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <SDL2/SDL.h>

static std::string toInternal(const std::string &name)
{
    std::string s = name;
    for (auto &c : s)
        if (c == '.')
            c = '/';
    return s;
}

int main(int argc, char **argv)
{
    const char *jarPath = (argc > 1) ? argv[1] : "games/assasin.jar";

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
    if (!hal::display_init(&cfg))
    {
        fprintf(stderr, "Echec init display\n");
        return 1;
    }
    hal::input_init();

    jvm::Runtime rt;
    jvm::Interpreter interp(&rt);
    rt.setJar(&jar);

    jvm::initNatives();
    jvm::midp::init(&rt, &interp);
    jvm::midp::setAppProperty("MIDlet-Name", manifest.midletName);
    jvm::midp::setAppProperty("MIDlet-Version", manifest.midletVersion);
    jvm::midp::setAppProperty("MIDlet-Vendor", manifest.midletVendor);

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
            const char *testNames[] = { "3", "14", "dataIGP", nullptr };
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
    if (const char *ak = getenv("JME_AUTOKEY"))
    {
        std::string s(ak);
        if (s == "5") autoKey = hal::KEY_5;
        else if (s == "0") autoKey = hal::KEY_0;
        else if (s == "*") autoKey = hal::KEY_STAR;
        else if (s == "#") autoKey = hal::KEY_HASH;
        else if (s == "FIRE") autoKey = hal::KEY_FIRE;
        else if (s == "SOFT1") autoKey = hal::KEY_SOFT1;
        else if (s == "SOFT2") autoKey = hal::KEY_SOFT2;
        else if (s == "LEFT") autoKey = hal::KEY_LEFT;
        else if (s == "RIGHT") autoKey = hal::KEY_RIGHT;
        else if (s == "UP") autoKey = hal::KEY_UP;
        else if (s == "DOWN") autoKey = hal::KEY_DOWN;
    }

    while (running)
    {
        hal::input_poll(&input);

        if (autoKey)
        {
            input.pressed |= autoKey;
            if (frame < 1) input.justPressed |= autoKey;
        }

        if ((input.justPressed & (hal::KEY_SOFT2 | hal::KEY_SOFT1)) && !(autoKey & (hal::KEY_SOFT2 | hal::KEY_SOFT1)))
            break;

        jvm::midp::tick(input.pressed, input.justPressed, input.justReleased);

        if (jvm::midp::midletDestroyed())
            break;

        frame++;
        if (maxFrames > 0 && frame >= maxFrames)
            break;

        hal::display_present(hal::display_get_framebuffer());
        SDL_Delay(16);
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
                    fputc(r, f); fputc(g, f); fputc(b, f);
                }
            }
            fclose(f);
        }
    }

    hal::input_shutdown();
    hal::display_shutdown();
    printf("Emulation terminee apres %d frames\n", frame);
    return 0;
}
