#pragma once
#include <string>

// Launcher SDL2 : liste les .jar d'un dossier (nom/version lus dans le manifeste),
// clavier / souris / molette, et renvoie le chemin du JAR choisi ("" = quitter).
// Ouvre et referme lui-même l'affichage (le jeu ré-initialise le sien ensuite).
namespace launcher
{
    std::string run(const std::string &gamesDir, const std::string &preselect);
}
