#pragma once

// Couche javax.microedition.midlet / lcdui implémentée en C++ (côté CAM).
// Les classes natives sont enregistrées dans le Runtime et leurs méthodes
// routées vers ces fonctions. main.cpp pilote la boucle via midp::tick().

#include <cstdint>
#include <string>

namespace jvm
{

    struct Runtime;
    class Interpreter;
    struct Obj;

    namespace midp
    {

        // Enregistre les classes natives API + leurs natives. À appeler une fois,
        // après initNatives() et avant de lancer le MIDlet.
        void init(Runtime *rt, Interpreter *interp);

        // Fournit les propriétés MIDlet-xxx (Service de MIDlet.getAppProperty()).
        void setAppProperty(const std::string &key, const std::string &value);

        // Le MIDlet a appelé notifyDestroyed() ?
        bool midletDestroyed();
        void resetDestroyed();

        // Objet récepteur courant (ce que Display.setCurrent a posé), nullptr sinon.
        Obj *currentDisplayable();

        // Une frame : dispatch des touches réelles, repaint si demandé, present.
        // keyMaskJust/Released : masques bruts du HAL.
        void tick(uint32_t pressedMask, uint32_t justPressedMask, uint32_t justReleasedMask);

        // Simule un appui stylet/tactile sur le canvas courant : dispatch
        // pointerPressed puis pointerReleased au point (x, y) (écran logique).
        // Ne fait rien si l'affichable courant n'est pas un Canvas.
        void simulatePointer(int x, int y);

        // Debug : nombre d'écritures de pixels dans le framebuffer écran.
        int jme_screenWrites();
        int jme_screenPix();
        int jme_canvasPix();
        int jme_flushCalls();

    } // namespace midp
} // namespace jvm