# Intégration

## CMakeLists.txt

Ajouter les deux nouvelles unités de compilation à votre cible existante :

```cmake
add_executable(j2me_emu
    main.cpp
    hal/jar_reader.cpp
    hal/inflate.cpp
    # ... vos autres fichiers hal/
)

target_include_directories(j2me_emu PRIVATE hal)

# Sur PC, active le cache d'index en RAM (plus rapide pour du dev/debug).
# A retirer pour la cible RP2040.
target_compile_definitions(j2me_emu PRIVATE JAR_READER_INDEX_IN_RAM)
```

Pour un futur `CMakeLists.txt` ciblant le RP2040 (pico-sdk), ne pas définir
`JAR_READER_INDEX_IN_RAM`, et définir `JAR_READER_NO_COMMENT_SCAN` pour
éviter le chemin de repli qui peut réserver jusqu'à ~64 KB :

```cmake
target_compile_definitions(j2me_emu_rp2040 PRIVATE JAR_READER_NO_COMMENT_SCAN)
```

## Exemple d'utilisation (main.cpp)

```cpp
#include "hal/jar_reader.h"

void loadMidlet(const char* jarPath) {
    jme::JarReader jar;
    if (!jar.open(jarPath)) {
        fprintf(stderr, "Impossible d'ouvrir %s\n", jarPath);
        return;
    }

    jme::ManifestInfo manifest;
    if (!jar.readManifest(manifest)) {
        fprintf(stderr, "MANIFEST.MF invalide ou MIDlet-1 absent\n");
        return;
    }

    printf("MIDlet: %s (%s) - classe principale: %s\n",
           manifest.midletName.c_str(),
           manifest.midletVersion.c_str(),
           manifest.mainClass.c_str());

    // Buffer statique : évite l'allocation dynamique, taille à ajuster
    // selon la taille max de .class attendue (les .class J2ME dépassent
    // rarement 16-32 KB).
    static uint8_t classBuffer[32 * 1024];
    size_t classSize = jar.extractClass(manifest.mainClass, classBuffer, sizeof(classBuffer));

    if (classSize == 0) {
        fprintf(stderr, "Echec extraction de %s\n", manifest.mainClass.c_str());
        return;
    }

    printf("Classe principale extraite : %zu octets\n", classSize);
    // -> à passer ensuite au class loader de la JVM (étape B).
}
```

## Choix d'implémentation

- **Pas de zlib/miniz externe** : un décodeur DEFLATE (~250 lignes) est
  réécrit à partir de l'algorithme public RFC 1951 (technique de décodage
  canonique bit-à-bit, cf. `puff.c` de Mark Adler pour la méthode). Cela
  évite une dépendance tierce à porter/configurer sur RP2040 et garde un
  contrôle total sur l'empreinte mémoire.
- **Aucune fenêtre glissante séparée** : DEFLATE autorise des
  back-references jusqu'à 32 KB en arrière. Comme on décompresse
  directement dans le buffer de destination fourni par l'appelant (déjà
  dimensionné à la taille décompressée exacte, lue dans le central
  directory), ce buffer sert lui-même de fenêtre — aucune copie ni buffer
  intermédiaire de 32 KB nécessaire.
- **Scan séquentiel du central directory par défaut** (pas de `std::vector`
  d'entrées) : pour un jar avec ~100-300 entrées, garder tous les noms de
  fichiers en RAM en permanence coûterait plusieurs KB inutilement sur
  RP2040. Le flag `JAR_READER_INDEX_IN_RAM` permet de basculer en mode
  "cache" pour le développement PC où la RAM n'est pas un problème.
- **Buffers fournis par l'appelant** : `extractClass`/`extractEntry` ne font
  jamais de `new`/`malloc` en interne. Le futur class loader JVM pourra donc
  réutiliser un seul buffer scratch pour charger les classes une par une (le
  pool de classes n'a pas besoin de garder le bytecode brut au-delà du
  parsing en structures internes).
- **Un seul `FILE*` dépendant de la libc** : c'est le seul point à modifier
  lors du portage RP2040 (remplacer par vos primitives `hal_file_*` sur
  flash externe / carte SD). Toute la logique ZIP/manifeste/inflate en est
  indépendante.

## Limites connues (à combler si besoin)

- Pas de support ZIP64 (inutile pour des .jar J2ME, toujours < 4 GB).
- Pas de support du "line folding" RFC 822 dans le manifeste (rare pour les
  champs `MIDlet-*`, qui tiennent en une ligne dans l'immense majorité des
  jars réels).
- Décodage Huffman bit-à-bit (pas de table de lookup rapide) : simple et
  économe en RAM, mais pas optimal en vitesse. Point d'optimisation naturel
  si le chargement d'un jar s'avère trop lent sur RP2040.