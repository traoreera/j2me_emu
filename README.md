# J2ME MIDP Emulator

Émulateur expérimental de MIDlets Java ME écrit en C++17. Le projet charge un `.jar`, analyse les fichiers `.class`, exécute le bytecode avec un interpréteur et fournit les API CLDC/MIDP nécessaires au jeu. L’implémentation PC utilise SDL2 et produit un framebuffer RGB565.

Le portage RP2040 est une direction d’architecture, pas un portage terminé. Le noyau et le décodeur ZIP sont conçus pour une mémoire contrainte, mais le runtime, l’interpréteur, l’API MIDP et le chargeur de classes ne respectent pas encore le budget mémoire de 264 Ko.

> **Important :** cet émulateur n’est pas conçu pour isoler du code non fiable. Il ne faut pas l’utiliser pour exécuter des JAR non fiables tant que le bytecode et les formats d’entrée ne sont pas complètement validés.

## Sommaire

- [État du projet](#état-du-projet)
- [Architecture](#architecture)
- [Organisation du dépôt](#organisation-du-dépôt)
- [Compilation](#compilation)
- [Exécution et commandes](#exécution-et-commandes)
- [Configuration](#configuration)
- [Cycle d’exécution](#cycle-dexécution)
- [Sous-systèmes](#sous-systèmes)
- [Compatibilité](#compatibilité)
- [Tests](#tests)
- [Contraintes de conception](#contraintes-de-conception)
- [Points critiques](#points-critiques)
- [Priorités de correction](#priorités-de-correction)
- [Documentation complémentaire](#documentation-complémentaire)

## État du projet

| Domaine | État |
|---|---|
| Exécution PC/SDL2 | Fonctionnelle pour un ensemble limité de MIDlets |
| Lecture JAR/ZIP/DEFLATE | Fonctionnelle pour les entrées non chiffrées, Stored et Deflate |
| API CLDC/MIDP | Couverture partielle, avec quelques extensions Nokia |
| Threads | Ordonnancement coopératif via fibres POSIX `ucontext` |
| Rendu | RGB565, Canvas, GameCanvas, Image, Graphics et Game API |
| Audio | Mélangeur mono fixe, implémentation SDL ou stub, WAV/MIDI/ToneSequence |
| Tests | 55 tests unitaires sur les couches logiques |
| RP2040 | Non exécutable dans le budget mémoire actuel |
| Exécution de code non fiable | Non sûre en l’état |

La cible est la compatibilité avec les MIDlets J2ME de l’époque, généralement publiés pour Java 1.1/1.3 et CLDC. Ce n’est pas une implémentation complète de la JVM moderne.

## Architecture

Pipeline principal :

```text
JAR/ZIP
  → JarReader + DEFLATE
  → ClassFile
  → Runtime et chargement paresseux des classes
  → Interpréteur de bytecode
  → natives CLDC/MIDP
  → kernel audio
  → HAL display/input
  → SDL2 sur PC
```

Le chemin de données principal est volontairement simple : le JAR reste sur disque, chaque classe est extraite dans un buffer, puis ses structures sont conservées par le runtime pendant toute l’exécution. Le rendu passe par un framebuffer RGB565 partagé entre le Game API, `Canvas` et le HAL SDL2.

## Organisation du dépôt

| Chemin | Rôle |
|---|---|
| `main.cpp` | Démarrage, configuration, chargement du MIDlet, boucle de trame et dump PPM |
| `hal/file.*` | Accès fichier abstrait, implémenté par `FILE*` sur PC |
| `hal/jar_reader.*` | Lecture ZIP/JAR et extraction dans des buffers fournis par l’appelant |
| `hal/inflate.*` | Décodeur DEFLATE RFC 1951 autonome |
| `hal/png.*` | Décodeur PNG minimal, dont les passes Adam7 |
| `hal/display.*` | Fenêtre SDL2, texture RGB565 et framebuffer |
| `hal/input.*` | Clavier et pointeur SDL2 vers les masques J2ME |
| `vm/class_file.*` | Lecture du format `.class` et des attributs `Code` |
| `vm/runtime.*` | Objets, tableaux, classes, champs, méthodes et allocateur bump |
| `vm/interpreter.*` | Exécution du bytecode, branches, exceptions natives et cache de résolution |
| `vm/native.*`, `vm/natives.cpp` | Registre natives, classes CLDC, collections, `PrintStream`, RMS et ordonnanceur de fibres |
| `vm/midp.h`, `vm/midp_internal.h` | API publique et état partagé de la couche MIDP |
| `vm/midp_natives.cpp` | Enregistrement des classes natives, événements et pompage par trame |
| `vm/midp_core.cpp` | Utilitaires et état commun MIDP |
| `vm/midp_graphics.cpp` | Canvas, GameCanvas, Image, Graphics, Font et rendu |
| `vm/midp_game.cpp` | Layer, Sprite, TiledLayer et LayerManager |
| `vm/midp_ui.cpp` | Display, MIDlet, Canvas, Form, List et Command |
| `vm/midp_io.cpp` | Streams, ressources du JAR et Timer |
| `vm/midp_media.cpp` | Player, WAV, MIDI, ToneSequence et événements audio |
| `kernel/kernel.*` | Registre de pilotes et horloge monotone |
| `kernel/drivers/audio/` | Mélangeur audio et implémentations SDL/stub |
| `kernel/drivers/{display,input,storage,timer}/` | Interfaces de pilotes, pour l’instant uniquement des en-têtes |
| `CMakeLists.txt` | Cibles `j2me_emu` et `j2me_tests` |
| `tests/` | Micro-framework et tests des couches logiques |
| `games/` | MIDlets utilisés pour les essais manuels |

## Compilation

### Dépendances

- compilateur C++17 compatible avec `<ucontext.h>` sur PC ;
- SDL2 et `pkg-config` pour l’émulateur graphique ;
- CMake 3.16 ou supérieur pour la build CMake.

### CMake

```bash
cmake -S . -B build
cmake --build build -j
```

Cette commande produit notamment `build/j2me_emu` et `build/j2me_tests`.

### Compilation directe

```bash
g++ -std=c++17 -O2 -I. -Ihal -Ivm -DJAR_READER_INDEX_IN_RAM \
    main.cpp hal/jar_reader.cpp hal/inflate.cpp hal/file.cpp \
    hal/display.cpp hal/input.cpp hal/png.cpp \
    vm/class_file.cpp vm/interpreter.cpp vm/runtime.cpp \
    vm/natives.cpp vm/midp_*.cpp \
    kernel/kernel.cpp kernel/drivers/audio/audio.cpp \
    kernel/drivers/audio/sdl_audio.cpp kernel/drivers/audio/stub_audio.cpp \
    -o j2me_emu $(pkg-config --cflags --libs sdl2)
```

La commande CMake ajoute `-Ikernel` pour reproduire les inclusions de la cible. Pour la compilation directe, les trois racines `-I.`, `-Ihal` et `-Ivm` suffisent avec les includes actuels ; `-Ikernel` reste utile pour conserver la configuration du projet.

## Exécution et commandes

```bash
./build/j2me_emu games/assasin.jar
./build/j2me_emu games/mission.jar
```

Sans argument, le jeu par défaut est `games/assasin.jar`.

La boucle cible environ 30 images/s avec un budget de 33 ms par trame. Le temps Java est virtuel : `System.currentTimeMillis()` avance au rythme de `JME_FRAME_TIME`, indépendamment du temps réel passé dans l’émulateur.

### Clavier physique

| Touche | Évaluation |
|---|---|
| Flèches | D-pad |
| Entrée ou Espace | `FIRE` |
| F1 | `SOFT1`, softkey gauche |
| F2 ou Échap | `SOFT2`, softkey droit |
| 0 à 9 | Touches numériques |
| `*` | Asterisk |
| `#` | Touche dièse |
| F12, Ctrl+Q ou fermeture de fenêtre | Quitter |

F1, F2 et Échap ne doivent pas quitter l’émulateur : plusieurs jeux les utilisent pour revenir à un menu.

### Exemples

Exécution sans fenêtre limitée :

```bash
SDL_VIDEODRIVER=dummy JME_AUDIO=stub JME_MAXFRAMES=120 \
    ./build/j2me_emu games/mission.jar
```

Capture du framebuffer final :

```bash
JME_DUMP=/tmp/j2me-frame.ppm ./build/j2me_emu games/mission.jar
```

Simulation d’une touche :

```bash
JME_AUTOTAPS='10:FIRE,20:SOFT1' JME_MAXFRAMES=60 \
    ./build/j2me_emu games/mission.jar
```

Remappage des touches :

```bash
JME_KEYMAP='escape=SOFT2,f2=SOFT1' \
    ./build/j2me_emu games/mission.jar
```

## Configuration

Les variables de configuration sont lues au démarrage ou lors de l’initialisation du sous-système concerné ; les variables de diagnostic et de capture peuvent être lues progressivement.

### Exécution et mémoire

| Variable | Défaut | Effet |
|---|---:|---|
| `JME_MAXFRAMES` | illimité | Arrêt après le numéro de trame indiqué ; `0` ou une valeur négative signifie illimité |
| `JME_WIDTH` | `240` | Largeur logique et physique du framebuffer |
| `JME_HEIGHT` | `320` | Hauteur logique et physique du framebuffer |
| `JME_HEAP` | `512` | Taille initiale du heap JVM, en KiB ; le heap peut ensuite croître |
| `JME_FRAME_TIME` | `16` | Avance virtuelle, en millisecondes, de `System.currentTimeMillis()` ; les valeurs inférieures à 1 sont ramenées à 1 |
| `JME_INSTR_BUDGET` | adaptatif | Budget fixe d’instructions par thread et par trame si strictement positif |
| `JME_AUDIO` | SDL, stub en mode sans fenêtre | `sdl`, `stub` ou `off` ; `off` suit actuellement le chemin stub |
| `JME_RMS` | persistant | `0` désactive la persistance RMS |
| `JME_RMSDIR` | `<nom-du-jar>.rms/` | Répertoire de persistance RMS ; ignoré lorsque `JME_RMS=0` |

Une valeur `JME_FRAME_TIME >= 1000` est considérée comme une valeur en microsecondes, divisée par 1000 avec avertissement. `33333` devient donc 33 ms par trame, pas 33 secondes.

`JME_HEAP` ne définit pas un plafond strict. Le heap alloue de nouveaux segments selon les besoins et peut dépasser la valeur initiale.

### Entrées scriptées

| Variable | Effet |
|---|---|
| `JME_AUTOKEY` | Force une touche dans l’état `pressed` à chaque trame |
| `JME_AUTOKEYFRAME` | Cadre où `justPressed` est injecté pour `JME_AUTOKEY` |
| `JME_AUTOHOLD` | Seconde touche maintenue après le cadre indiqué |
| `JME_AUTOHOLDFRAME` | Cadre de début de `JME_AUTOHOLD`, désactivé si la variable est absente |
| `JME_AUTOTAPS` | Liste `frame:touche,...` d’appuis d’une trame, avec relâchement à la trame suivante |
| `JME_KEYMAP` | Remappage SDL prioritaire, sous forme `touche=TOKEN,...` |
| `JME_AUTOTOUCH` | Coordonnées `x,y` d’un pointeur simulé |
| `JME_AUTOTOUCHFRAME` | Cadre du `pointerPressed`, suivi d’un `pointerReleased` trois trames plus tard |

`JME_AUTOKEY` accepte `0`, `5`, `*`, `#`, `FIRE`, `SOFT1`, `SOFT2`, `LEFT`, `RIGHT`, `UP` et `DOWN`. Sans `JME_AUTOKEYFRAME`, son état `pressed` est réinjecté à chaque trame et `justPressed` n’est produit qu’à la première trame. Avec `JME_AUTOKEYFRAME=n` et `n >= 0`, l’état `pressed` est réinjecté à chaque trame sauf `n`, `justPressed` est produit à `n`, mais aucun `justReleased` n’est produit. Pour un véritable appui suivi d’un relâchement, utiliser `JME_AUTOTAPS`.

`JME_KEYMAP` accepte les jetons `UP`, `DOWN`, `LEFT`, `RIGHT`, `FIRE`, `SOFT1`, `SOFT2`, `STAR`, `HASH`, `0` à `9` et `NONE`. Si la variable est absente, le fichier `<jar sans extension>.keys`, placé à côté du JAR, peut fournir un remappage avec une entrée par ligne.

### Diagnostic et captures

| Variable | Effet |
|---|---|
| `JME_DEBUG` | Active les diagnostics détaillés du runtime et du rendu |
| `JME_TRACE` | Trace l’activation initiale et certaines natives ciblées |
| `JME_TRACEALL` | Trace l’appel de toutes les natives |
| `JME_TRACEM` | Trace une méthode, par exemple `Classe.methode`, ou `*` |
| `JME_VTRACE` | Affiche l’horloge virtuelle toutes les dix trames |
| `JME_DRAWDBG` | Active le diagnostic du dessin |
| `JME_PIXDBG` | Active le diagnostic des écritures de pixels |
| `JME_DUMP` | Écrit le framebuffer final au format PPM/P6 |
| `JME_WAVDUMP` | Écrit les octets bruts de chaque InputStream Player dans `<path>_<slot>.wav` |
| `SDL_VIDEODRIVER=dummy` | Exécute SDL sans fenêtre ; à combiner avec `JME_AUDIO=stub` et `JME_MAXFRAMES` |

`JME_TRACE` n’est pas un désassembleur de bytecode. Pour observer une méthode, `JME_TRACEM` est l’outil prévu.

## Cycle d’exécution

### Démarrage

1. Ouvre le JAR et lit `META-INF/MANIFEST.MF`.
2. Extrait `MIDlet-1` et les propriétés MIDlet.
3. Initialise SDL, l’entrée, l’horloge et l’implémentation audio.
4. Crée le heap, l’interpréteur et les classes natives CLDC/MIDP.
5. Charge la classe principale et son bytecode depuis le JAR.
6. Construit l’objet MIDlet et appelle son constructeur puis `startApp()`.

### Trame

1. Collecte clavier, souris et événements scriptés.
2. Injecte les touches et événements pointeur dans l’API MIDP.
3. Avance l’horloge virtuelle.
4. Reprend chaque thread Java selon son budget d’instructions.
5. Exécute les timers, événements multimédias et rappels en attente.
6. Répartit les événements de touche au `Displayable` courant.
7. Effectue les repeints demandés.
8. Copie le framebuffer vers la texture SDL et présente l’image.
9. Attend seulement le temps restant pour viser 33 ms par trame.

### Arrêt

`notifyDestroyed()`, F12, Ctrl+Q ou la fermeture de la fenêtre terminent la boucle. L’implémentation audio, les entrées, l’affichage et les threads sont ensuite libérés ou fermés selon leur implémentation.

## Sous-systèmes

### Lecture JAR et inflation

`hal/jar_reader.*` localise l’EOCD, lit le répertoire central et localise les entrées sans charger le JAR entier en mémoire. Deux modes sont disponibles :

- sans `JAR_READER_INDEX_IN_RAM`, le répertoire central est parcouru séquentiellement ;
- avec `JAR_READER_INDEX_IN_RAM`, un index RAM accélère les recherches répétées sur PC.

L’extraction écrit dans un buffer fourni par l’appelant. Les méthodes ZIP `Stored` et `Deflate` sont gérées. ZIP64, les entrées chiffrées, le repli de lignes du manifeste et la vérification CRC ne sont pas pris en charge.

`hal/inflate.*` décode RFC 1951 bit à bit, sans bibliothèque zlib et sans fenêtre glissante séparée. La sortie déjà décodée sert de fenêtre aux références arrière DEFLATE. Le choix réduit la mémoire et la complexité, mais est plus lent qu’un décodeur avec table de Huffman.

### Format `.class` et classes

`vm/class_file.*` lit le pool de constantes, les interfaces, les champs, les méthodes et l’attribut `Code`. Le parseur accepte les versions jusqu’à Java 8 (`major <= 52`), mais cette limite de version ne signifie pas que tous les opcodes modernes sont exécutés.

`vm/runtime.*` :

- charge les classes du JAR à la demande ;
- enregistre les classes natives synthétiques ;
- lie les superclasses et calcule les emplacements d’instance ;
- mémorise les méthodes, champs, statiques et caches de résolution ;
- conserve les fichiers `.class` parsés pour la durée de l’exécution.

Le heap est un bump allocator sans GC. Les objets ne sont pas déplacés. Les segments peuvent croître, et `Heap::reset()` est la seule opération générale de récupération.

### Interpréteur

`vm/interpreter.*` exécute les opcodes, les branches, les appels, les champs statiques et d’instance, les tableaux, `new`, les casts, les exceptions et les attributs `LineNumberTable`/`LocalVariableTable` lorsqu’ils sont utiles au diagnostic.

Chaque trame d’appel réserve :

- `maxLocals` valeurs ;
- `maxStack` valeurs ;
- une catégorie de largeur par valeur de pile.

Les trames d’appel sont allouées dans une arène de 128 KiB propre à l’interpréteur. Un budget d’instructions permet de suspendre les threads coopératifs sans relancer leur méthode depuis le début.

### Natives CLDC et MIDP

Les classes absentes du JAR sont enregistrées comme classes natives synthétiques. Cela couvre notamment :

- `java.lang` et les classes d’enveloppe des nombres ;
- `java.io`, `java.util`, `Timer` et collections courantes ;
- `javax.microedition.midlet.MIDlet` ;
- `Display`, `Displayable`, `Canvas`, `Image`, `Graphics` et `Font` ;
- `Form`, `List`, `Command` et traitement minimal des écouteurs ;
- `javax.microedition.lcdui.game` ;
- quelques extensions `com.nokia.mid.ui` ;
- `javax.microedition.media` ;
- `javax.microedition.rms.RecordStore`.

Cette approche ne recrée pas toute la bibliothèque standard. Certaines méthodes sont des stubs, certaines classes seulement les interfaces utilisées par les jeux testés, et la compatibilité dépend fortement de l’obfuscation et du MIDlet.

### Threads

Chaque `Thread.start()` alloue une `JmeFiber` avec une pile C++ de 256 KiB. Le ordonnanceur utilise `getcontext`, `makecontext` et `swapcontext` pour suspendre une méthode au milieu de sa boucle Java.

Ce mécanisme permet de conserver `pc`, les locales et la pile C++ lors d’un `sleep()`, d’un `yield()` ou d’un budget épuisé. Il reste dépendant de l’ABI POSIX, n’est pas portable RP2040 et ne fournit pas de détection des dépassements de pile C++.

### Rendu

`vm/midp_graphics.cpp` fournit :

- `Image` mutable ou immuable ;
- images RGB, images PNG chargées depuis le JAR ;
- `Graphics` avec découpage, texte bitmap, remplissage, lignes, rectangles et mélange ;
- framebuffer RGB565 commun ;
- `GameCanvas`, sprites, transformations, collisions et couches tuilées.

L’implémentation SDL2 transforme le framebuffer RGB565 en texture de flux. La fenêtre est créée à deux fois la résolution logique.

### Audio

`kernel/drivers/audio/audio.cpp` mélange huit voix :

- PCM ;
- tonalité sinusoïdale ;
- séquence de tons.

L’implémentation SDL produit du son mono 16 bits à 22 050 Hz. L’implémentation stub conserve les mêmes états d’avancement et permet l’exécution sans fenêtre.

`vm/midp_media.cpp` lit les InputStream des Player, prend en charge les WAV PCM, certains MIDI et `audio/x-tone-seq`, rééchantillonne vers 22 050 Hz puis alimente les voix du noyau. Les données audio volumineuses restent dans des `std::vector` et ne sont pas intégrées au heap JVM.

### RecordStore

`RecordStore` partage un magasin entre les ouvertures du même nom. La persistance est activée par défaut dans un dossier portant le nom du JAR, avec un fichier par magasin. `JME_RMS=0` conserve les données uniquement en mémoire.

## Compatibilité

### Fonctionnalités MIDP couvertes

- cycle de vie du MIDlet ;
- Display et Displayable ;
- Canvas, GameCanvas, FullCanvas Nokia et DirectUtils minimal ;
- Image, Graphics et Font bitmap ;
- Sprite, TiledLayer, LayerManager et couches ;
- streams, timers et accès aux ressources du JAR ;
- RecordStore ;
- Player, WAV, MIDI et ToneSequence ;
- clavier numérique, D-pad, softkeys et pointeur.

### Limites de la JVM

Les opcodes suivants ne sont pas implémentés ou ne sont pas fidèlement exécutés :

- `dup2_x2` (`0x5d`) ;
- `jsr`, `ret` et `invokedynamic` ;
- `0xc8`, qui est traité comme `goto_w` alors que la numérotation JVM lui assigne `jsr_w` ;
- les conversions `f2i`, `f2l`, `f2d`, `d2i`, `d2l` et `d2f` (`0x8b–0x90`), qui réinterprètent les bits au lieu de convertir la valeur flottante ;
- les opérations arithmétiques flottantes standards.

Le sous-ensemble entier, long, objet, tableau, branche, appel, champ, `new`, cast et gestion d’exceptions est beaucoup plus complet que le sous-ensemble flottant.

Autres limites importantes :

- pas de GC ;
- pas de finalizer ni de références faibles ;
- pas de modèle de threads Java complet ;
- pas d’instrumentation, de profilage, de débogueur ni de JDWP ;
- pas de vérification du bytecode ;
- chargement paresseux limité aux entrées du JAR, sans vérification complète des classes modernes ;
- résolution des interfaces et diagnostics de types incomplets ;
- comportement de plusieurs méthodes MMAPI réduit à un sous-ensemble fonctionnel.

## Tests

### Couverture actuelle

Les tests utilisent un micro-framework sans dépendance externe :

| Fichier | Nombre de tests |
|---|---:|
| `tests/test_inflate.cpp` | 5 |
| `tests/test_jar_reader.cpp` | 7 |
| `tests/test_png.cpp` | 5 |
| `tests/test_class_file.cpp` | 11 |
| `tests/test_runtime.cpp` | 14 |
| `tests/test_interpreter.cpp` | 8 |
| `tests/test_recordstore.cpp` | 5 |
| **Total** | **55** |

### CMake

```bash
cmake -S . -B build
cmake --build build --target j2me_tests
ctest --test-dir build --output-on-failure
```

La configuration CMake principale demande SDL2 avant de définir la cible de tests. Pour compiler uniquement les tests sans SDL2, utiliser la commande directe suivante.

### Sans SDL2

```bash
g++ -std=c++17 -O0 -g -I. -Ihal -Ivm \
    tests/test_*.cpp \
    hal/inflate.cpp hal/jar_reader.cpp hal/file.cpp hal/png.cpp \
    vm/class_file.cpp vm/runtime.cpp vm/interpreter.cpp vm/natives.cpp \
    -o build/j2me_tests
./build/j2me_tests
```

### Tests d’intégration

Il n’existe pas encore de tests d’intégration automatisés pour :

- `vm/midp_*.cpp` ;
- le ordonnanceur de fibres ;
- SDL display/input/audio ;
- le cycle MIDlet complet ;
- les jeux de référence.

Une vérification manuelle minimale est :

```bash
SDL_VIDEODRIVER=dummy JME_AUDIO=stub JME_MAXFRAMES=5 \
    ./build/j2me_emu games/assasin.jar
```

Le résultat attendu contient l’identification du MIDlet, puis `Emulation terminee apres 5 frames`. `Assassin's Creed 2` demande généralement 800×480 et `JME_HEAP=4096`, mais son chargement ne termine pas encore complètement dans le portage actuel.

## Contraintes de conception

| Contrainte | État réel |
|---|---|
| Ne pas charger un fichier complet en RAM | Valide pour le JAR, pas pour les classes et images |
| Buffers de sortie fournis par l’appelant | Valide pour `JarReader` et `Inflate` |
| Heap stable, sans déplacement d’objets | Valide, mais le heap peut croître sans plafond |
| Rendu RGB565 déterministe | Valide sur PC |
| Noyau MCU portable | Valide pour `kernel/kernel.*` et le mélangeur audio |
| Aucun STL ou `new` dans les chemins chauds | Partiellement vrai, mais faux comme invariant global |
| `hal/file.cpp` seul dépendant de la libc | Faux : `main.cpp`, le RMS, les médias et les diagnostics utilisent aussi la libc |
| Budget total RP2040 de 264 Ko | Non atteint |

Les commentaires historiques « aucun new dans les chemins chauds » et « seul `hal/file.cpp` dépend de la libc » ne doivent pas être utilisés comme garanties. Plusieurs chemins d’initialisation et certains chemins média utilisent `new`, `std::vector`, `std::string`, `FILE*` ou `system()`.

## Points critiques

La priorité **P0** bloque la sécurité d’exécution ou le portage RP2040. **P1** peut provoquer une corruption mémoire, une perte de données ou une régression importante. **P2** affecte la compatibilité, les diagnostics ou la qualité.

### P0

1. **Le bytecode n’est pas validé avant exécution.**<br>
   `vm/interpreter.cpp:21-24` lit les opérandes sans vérifier la taille restante, et les cibles de branches ne sont pas bornées. La taille des trames est plafonnée par l’arène de 128 KiB (`vm/interpreter.cpp:195-205`, `frameAlloc()` échoue au-delà), mais cela ne protège ni les lectures de bytecode hors buffer, ni les indices de variables, de tableaux ou du pool de constantes. Un `.class` malveillant peut donc provoquer une lecture ou une écriture hors buffer.

2. **Les dimensions d’image et d’écran ont des multiplications non contrôlées.**<br>
   `vm/midp_graphics.cpp:795-823` calcule `w * h` dans un `int32_t` après seulement avoir ramené les dimensions négatives ou nulles à 1, sans plafond supérieur. `hal/display.cpp:35` alloue directement `width * height`. `JME_WIDTH` et `JME_HEIGHT` ne sont pas validés au démarrage. Des dimensions importantes, négatives ou nulles peuvent provoquer un débordement lors du calcul, une allocation invalide ou un framebuffer incohérent.

3. **`RecordStore` construit une commande shell sans échappement.**<br>
   `vm/natives.cpp:851-858` construit `mkdir -p '<JME_RMSDIR>'` puis appelle `system()`. Un chemin contenant une apostrophe permet d’injecter des commandes. Le RMS doit passer par une primitive de répertoire du HAL, par exemple `hal_file_mkdir`.

4. **Le portage RP2040 dépasse déjà le budget avant le chargement des ressources.**<br>
   Le buffer de classe statique vaut 1 MiB (`vm/runtime.cpp:352`), le heap initial vaut 512 KiB, l’arène de trames d’appel vaut 128 KiB et chaque fibre vaut 256 KiB. Ces allocations seules sont incompatibles avec les 264 Ko de RAM du RP2040.

5. **Le parseur WAV ne valide pas complètement les chunks.**<br>
   `vm/midp_media.cpp:209-230` peut lire le header `fmt ` sur `off + 24` alors que le chunk tronqué ne contient que huit octets disponibles. Il accepte également un chunk `data` dont l’offset dépasse la taille du buffer. Ces cas peuvent provoquer une lecture hors buffer ou un calcul de plage invalide.

### P1

1. **Le parseur de classes lit certains comptes avant de vérifier leur en-tête.**<br>
   `vm/class_file.cpp:255`, `vm/class_file.cpp:264`, `vm/class_file.cpp:289` et `vm/class_file.cpp:324` lisent un `uint16_t` sans contrôle préalable de `off + 2 <= len`. Un fichier tronqué peut provoquer un accès hors buffer.

2. **Le validateur de bytecode est absent.**<br>
   Même si tous les accès mémoire de l’interpréteur étaient bornés, le code ne vérifie ni les types, ni les cibles de branches, ni les poignées du pool de constantes, ni les tables d’exceptions. Les classes doivent être validées structurellement puis vérifiées par méthode avant `Runtime::buildFromClassFile()` (`vm/runtime.cpp:246-332`).

3. **DEFLATE n’est pas assez strict pour des données non fiables.**<br>
   `hal/inflate.cpp:11-23` représente la fin de flux par des octets nuls et `hal/inflate.cpp:91-109` ne teste l’EOF qu’après un décodage réussi. Les tables Huffman ne sont pas validées contre les arbres sur/sous-souscrits. Un flux tronqué peut être accepté ou mal interprété.

4. **La pile des fibres n’est pas protégée contre les dépassements.**<br>
   `vm/natives.cpp:169-178` alloue 256 KiB avec `std::vector<char>` et lance du bytecode sur cette pile sans garde ni détection de dépassement. Une récursion profonde peut corrompre silencieusement la mémoire.

5. **Le heap n’a pas de plafond strict.**<br>
   `vm/runtime.cpp:45-90` crée automatiquement de nouveaux segments. `JME_HEAP` ne fixe que la taille initiale. Les structures natives, les images et les caches peuvent donc croître sans limite définie par la plateforme.

6. **Un test de ressource PNG de débogage modifie le jeu au démarrage.**<br>
   `main.cpp:178-210` tente de charger `3`, `14` ou `dataIGP`, puis écrit du blanc dans le framebuffer. Le code suppose un framebuffer d’au moins 240×320. Ce test doit être déplacé dans `tests/` ou exécuté uniquement sous un drapeau explicite.

7. **La durée de vie des objets Player audio doit être durcie.**<br>
   `vm/midp_media.cpp:113-159` associe des `std::vector` à un emplacement fixe et conserve leur adresse dans le noyau. La fermeture arrête la voix, mais aucun `finalize` n’est disponible et les objets Player oubliés conservent leur emplacement. Les buffers audio doivent avoir une durée de vie explicitement garantie et testée.

### P2

- `Font.stringWidth(null)` alloue une chaîne vide avec `new` sans la libérer (`vm/midp_graphics.cpp:730-733`).
- `DirectGraphics.drawImage` est essentiellement un stub (`vm/midp_graphics.cpp:1194-1210`).
- Les conversions flottantes de `vm/interpreter.cpp:824-829` ne respectent pas la représentation et la conversion IEEE attendues.
- `kernelBoot()` ignore les erreurs d’initialisation des pilotes (`kernel/kernel.cpp:54-64`).
- Plusieurs chemins d’échec d’initialisation ne ferment pas tous les sous-systèmes déjà ouverts.
- Les diagnostics de compteurs d’écriture ne distinguent pas toujours le framebuffer réel du `GameCanvas`.
- La documentation historique est en retard sur le code actuel, notamment pour les sources MIDP, les variables, les tests et le budget mémoire.

## Priorités de correction

### 1. Sécuriser les entrées

- introduire un lecteur bytecode à lectures bornées ;
- valider toutes les structures de `.class` avant construction ;
- vérifier les cibles de branches, les tables de switch, les poignées du pool de constantes et les tables d’exceptions ;
- ajouter une multiplication sûre et des dimensions maximales pour display, images, tableaux et tuiles ;
- rendre DEFLATE, PNG, WAV et le répertoire central strictement invalides ou explicitement tolérants ;
- ajouter des tests de fuzzing et un corpus de fichiers tronqués.

### 2. Remplacer la persistance par le HAL

- ajouter une primitive `hal_file_mkdir` ;
- supprimer `system()` de `vm/natives.cpp` ;
- faire passer la lecture et l’écriture RMS par `hal::file_*` ;
- tester les chemins RMS contenant espaces, apostrophes, caractères Unicode et profondeur excessive.

### 3. Définir un vrai budget RP2040

- parser les classes en flux ou avec un petit buffer configurable ;
- distinguer le heap objet, les caches, les images, l’arène de trames d’appel et les piles de fibres ;
- ajouter un plafond strict indépendant de `JME_HEAP` ;
- supprimer ou remplacer `ucontext` par un ordonnanceur de bytecode portable ;
- stocker les buffers audio dans une mémoire dont la propriété est claire ;
- produire un tableau de budget avant toute allocation de ressources pour le jeu.

### 4. Renforcer les tests

- exécuter la suite sous ASan et UBSan ;
- ajouter des tests de JAR, PNG, WAV, manifestes et fichiers `.class` malformés ;
- ajouter des tests d’opcodes avec branches hors limites, débordements de pile, tables invalides et récursion profonde ;
- automatiser deux tests de fumée sans fenêtre avec captures déterministes ;
- comparer les sorties de deux builds avec `JME_INSTR_BUDGET` fixe.

### 5. Compléter la compatibilité

- corriger les opcodes JVM restants ;
- remplacer les stubs MIDP prioritaires ;
- couvrir les exceptions d’API, les dimensions négatives, les objets `Player` et les commandes ;
- séparer les tests unitaires des tests d’intégration SDL et audio.

## Documentation complémentaire

- `AGENTS.md` contient les règles de build et d’exécution utilisées par l’outillage.
- `CLAUDE.md` contient des détails historiques sur l’architecture, mais plusieurs contraintes doivent être mises à jour avec la section « Points critiques ».
- `INTEGRATION.md` décrit l’intention initiale du portage RP2040 et le mode de lecture JAR sans index RAM.
