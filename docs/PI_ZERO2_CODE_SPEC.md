# Spécification de modification du code — Pi Zero 2 W 64 bits

## Statut

Ce document décrit la cible logicielle du Raspberry Pi Zero 2 W. Il complète `docs/HARDWARE_TARGET.md` en détaillant les changements nécessaires au build, à la mémoire, aux assets, au rendu et aux entrées.

La cible de référence est :

- Raspberry Pi Zero 2 W avec 512 Mo de RAM ;
- Raspberry Pi OS Lite 64 bits ;
- C++17, CMake et SDL2 ;
- résolution logique principale 240×320 ;
- profil expérimental 800×480 ;
- rendu à 30 images/s ;
- heap JVM initial de 64 MiB et plafond de 128 MiB ;
- caches d’assets et de rendu limités ;
- JAR lu par seeks, sans chargement intégral en RAM.

## Décision d’architecture

Le pipeline actuel reste conservé :

```text
JAR sur microSD → JarReader → classes et assets → VM MIDP
VM MIDP → framebuffer RGB565 logique → renderer SDL2 → écran
```

Le rendu ne doit pas pré-calculer toutes les images possibles. Une image 800×480 RGB565 occupe environ 750 KiB ; dix images représentent déjà environ 7,3 MiB, avant les sprites et les transformations.

Le renderer doit :

- convertir une fois les images en RGB565 ou en tuiles ;
- mettre en cache les sprites, tuiles et arrière-plans stables ;
- exécuter les transformations MIDP dynamiques au moment du rendu ;
- composer la scène dans un framebuffer logique ;
- envoyer une seule texture à SDL2 lorsque nécessaire.

## Objectifs

1. Compiler et exécuter le projet en ARM64 sur Raspberry Pi OS Lite.
2. Donner au Pi Zero 2 W une marge mémoire suffisante pour les jeux J2ME.
3. Éviter les conversions PNG, alpha et scaling répétées à chaque image.
4. Conserver une fluidité stable à 30 images/s pour les jeux ciblés.
5. Supporter les profils 240×320 et 800×480 avec des configurations distinctes.
6. Ajouter un gamepad USB HID sans casser le clavier ni le tactile.
7. Mesurer la mémoire, le temps de rendu et les images manquées.
8. Préserver le chargement séquentiel du JAR et les buffers fournis par l’appelant.
9. Fournir un launcher SDL2 et un overlay tactile sans desktop.

## Non-objectifs

Cette première version ne doit pas :

- faire tenir la VM sur un RP2040 ;
- charger la totalité d’un JAR arbitraire dans un buffer RAM ;
- stocker des séquences complètes d’images vidéo pré-calculées ;
- remplacer Linux par un kernel bare-metal ;
- réécrire le décodeur DEFLATE ou le lecteur ZIP ;
- faire du gamepad la source unique des entrées ;
- faire correspondre une softkey à la commande quitter ;
- garantir la fluidité de tous les JAR sans mesurer chaque jeu.

## État actuel du code

| Domaine | État actuel | Modification attendue |
|---|---|---|
| Build | CMake, C++17 et SDL2 dans `CMakeLists.txt` | Ajouter un profil ARM64 sans casser le build PC |
| Heap | `JME_HEAP` fixe seulement le segment initial dans `main.cpp:149-152` | Ajouter une limite maximale configurable |
| Croissance du heap | Auto-croissance dans `src/core/runtime.cpp:53-90` | Empêcher les allocations non bornées sur Pi |
| Classes | Buffer statique de 1 MiB dans `src/core/runtime.cpp:364` | Le garder hors du cache d’assets |
| JAR | Seek/read et buffers appelant dans `src/hal/jar_reader.h:48-76` | Ne pas slurper le JAR |
| Affichage | Framebuffer RGB565 et mise à jour SDL dans `src/hal/display.cpp:17-84` | Séparer taille logique et taille physique |
| Rendu MIDP | Opérations graphics dans `src/midp/midp_graphics.cpp` | Ajouter des chemins rapides et des statistiques |
| Entrées | Clavier, souris et tactile dans `src/hal/input.cpp:61-147` | Ajouter SDL GameController |
| Cadence | Budget fixe de 33 ms dans `main.cpp:458` | Le rendre configurable et mesurer les retards |
| Audio | Noyau audio et backend SDL existants | Réutiliser sans réécriture |
| RMS | Répertoire persistant à côté du JAR | Garder le chemin Pi et isoler les tests |

## Cible matérielle et système

- Raspberry Pi Zero 2 W, quatre cœurs Cortex-A53 à 1 GHz ;
- 512 Mo de RAM ;
- carte microSD A2/U3 de 32 à 64 Go ;
- alimentation micro-USB 5 V avec une source fiable ;
- écran HDMI 800×480 ou écran SPI/HDMI pour le prototype ;
- gamepad USB OTG avec hub alimenté si nécessaire ;
- clavier et souris USB pour le développement.

Raspberry Pi OS Lite 64 bits, SDL2 et `pkg-config` sont utilisés. Le premier build se fait nativement sur le Pi ; la cross-compilation AArch64 viendra ensuite.

## Budget mémoire

Les valeurs suivantes sont des objectifs de conception. Elles doivent être confirmées avec `getrusage`, `/proc/<pid>/status` ou `smem` sur le Pi réel.

| Budget | Valeur cible | Rôle |
|---|---:|---|
| Heap JVM initial | 64 MiB | Objets, chaînes, tableaux et allocations MIDP |
| Heap JVM maximal | 128 MiB | Plafond strict de la croissance automatique |
| Cache d’assets | 32 MiB | Images et ressources fréquemment utilisées |
| Cache de rendu | 16 MiB | Sprites, tuiles et résultats réutilisables |
| Buffer de classes | 1 MiB | Extraction et parsing d’une classe |
| Framebuffers | 1,5 MiB à 800×480 | Deux copies RGB565 |
| Audio, piles et buffers natifs | 8 MiB | Marge de sécurité |
| Runtime, STL, SDL et métadonnées | 32 MiB | Estimation à ajuster par profiling |
| Budget processus cible | Environ 224 MiB | RSS recommandé |
| Seuil d’alerte | 256 MiB | Investiguer avant d’aller plus haut |

Le système, le pilote vidéo, la pile réseau et les services Linux doivent conserver environ 256 Mo disponibles. Le processus ne doit pas chercher à consommer toute la RAM physique.

## Politique de heap

Le heap actuel est un bump allocator sans GC. La croissance automatique est utile sur PC mais dangereuse sur un Pi avec 512 Mo.

Ajouter les paramètres suivants, exprimés en KiB :

```text
JME_HEAP=65536
JME_HEAP_MAX=131072
JME_ASSET_CACHE_MAX=32768
JME_RENDER_CACHE_MAX=16384
```

Le comportement attendu est le suivant :

- `JME_HEAP` est la capacité initiale ;
- `JME_HEAP_MAX` est la capacité totale maximale ;
- une allocation au-delà de `JME_HEAP_MAX` met le heap en état OOM ;
- la croissance ne doit jamais provoquer d’écriture hors buffer ;
- `reset()` conserve le segment initial et libère les segments ajoutés ;
- les erreurs de configuration sont refusées au démarrage ;
- l’absence de `JME_HEAP_MAX` conserve le comportement actuel de développement.

La première modification d’API est :

```cpp
Heap(size_t initialSize, size_t maximumSize = 0);
size_t used() const;
size_t capacity() const;
size_t maximumCapacity() const;
bool outOfMemory() const;
```

`Runtime` doit transmettre la limite au `Heap`. Les tests doivent vérifier le refus d’une allocation qui dépasse la limite, la croissance jusqu’à la limite, le nettoyage après `reset()` et l’absence d’écriture hors segment.

## Build ARM64

La modification 64 bits concerne principalement le build et les types de configuration, pas le format JAR ou le bytecode MIDP.

Ajouter un profil CMake `pi-zero2` ou un fichier de toolchain séparé :

```text
cmake/toolchains/aarch64-linux-gnu.cmake
```

Le toolchain doit définir une architecture AArch64, le préfixe du compilateur et le mode release. Le build natif sur Raspberry Pi OS reste la méthode de validation initiale.

Le projet doit conserver :

- C++17 ;
- SDL2 ;
- les mêmes formats de fichiers ;
- le même comportement du bytecode ;
- le build PC indépendant.

Ajouter une vérification au démarrage ou dans les tests :

```cpp
static_assert(sizeof(void *) == 8, "Pi Zero 2 build must be ARM64");
```

Le format des assets et les indices de classes doivent continuer à utiliser des types fixes (`uint16_t`, `uint32_t`, `uint64_t`) quand leur taille est partie du format. Les tailles de mémoire et les offsets natifs peuvent utiliser `size_t` et `off_t`.

## Chargement des assets

Le JAR reste la source de vérité. Le code ne doit pas faire :

```text
fichier JAR complet → buffer RAM → decompression de toutes les classes
```

Le chemin cible est :

```text
JarReader.findEntry()
→ extractEntry() dans un buffer appelant
→ AssetCache ou décodeur d’image
→ framebuffer ou rendu
```

### Cache d’assets

Ajouter un cache de ressources avec les règles suivantes :

- capacité fixe définie au démarrage ;
- stockage fourni par l’appelant ;
- pas de `malloc` interne dans `JarReader` ou `inflate` ;
- index limité et eviction LRU ;
- ressources épinglées pour le fond, le joueur et le niveau courant ;
- checksum ou taille pour rejeter une ressource incompatible ;
- compteur d’octets utilisés, de hits et de misses.

Les classes ne doivent pas entrer dans ce cache. Elles extraient une classe à la fois dans le buffer de 1 MiB existant.

### Pré-traitement hors exécution

Un outil externe, Python en première version, pourra produire un pack d’assets versionné. Le pack contiendra les ressources sélectionnées et, si cela réduit la charge, des images converties en RGB565 ou en tuiles. Il ne contiendra pas une vidéo de frames complètes.

Le chargeur devra accepter deux modes :

1. mode JAR direct pour le développement ;
2. mode pack pour un jeu optimisé, avec repli sur le JAR si une ressource manque.

Le pack est un cache de déploiement, pas un remplacement du format JAR. Il doit pouvoir être régénéré sans modifier le code du jeu.

## Cache de rendu

Le cache de rendu doit stocker des résultats réutilisables, pas des frames complètes :

- sprites RGB565 ;
- tuiles et feuilles de sprites ;
- images opaques ;
- versions mises à l’échelle pour les résolutions autorisées ;
- résultats de rotation ou de miroir lorsque la transformation est stable ;
- arrière-plans statiques.

Une ressource mutable MIDP doit rester liée à son image source. Les transformations dynamiques ne doivent pas être mises en cache au-delà d’un petit cache indexé par la transformation et la taille. Les images sources et les formats originaux restent disponibles pour les opérations qui modifient l’image.

## Renderer et affichage

### Séparation des dimensions

Le code doit distinguer :

- la résolution logique du MIDlet ;
- la taille de la fenêtre SDL ;
- la résolution physique de l’écran ;
- le mode plein écran ;
- l’échelle et le recadrage.

`JME_WIDTH` et `JME_HEIGHT` conservent leur rôle de résolution logique. Ajouter des options pour la fenêtre :

```text
JME_WINDOW_WIDTH=800
JME_WINDOW_HEIGHT=480
JME_FULLSCREEN=1
JME_SCALE=integer
JME_VSYNC=1
```

Le renderer doit utiliser un scaling entier ou un letterbox déterministe. Le filtrage bilinéaire est interdit par défaut pour préserver le rendu rétro et réduire la charge GPU.

### Composition

La première optimisation doit garder un framebuffer logique RGB565 et effectuer un blitter CPU vers ce framebuffer. SDL2 reçoit ensuite une seule mise à jour de texture à la présentation.

Optimiser les chemins MIDP suivants :

- remplissage de rectangles ;
- copie opaque ;
- copie alpha ;
- lignes et clipping ;
- texte bitmap ;
- transformation d’images ;
- vidage des graphics.

Ajouter des statistiques de rendu activables par `JME_RENDER_STATS=1` :

```text
framesAffichées
pixelsDessinés
pixelsCopiés
octetsTexture
cacheHits
cacheMisses
tempsInterpréteur
tempsRendu
tempsPrésentation
imagesManquées
```

Mesurer séparément le CPU et la présentation SDL. Le GPU ne doit pas devenir la source de chaque petite opération MIDP.

### Cadence

Le budget actuel de 33 ms doit devenir configurable :

```text
JME_FRAME_BUDGET=33
```

Note : `JME_FRAME_TIME` existe déjà et désigne la durée *virtuelle* d'une trame (horloge du jeu) ; le budget réel s'appelle `JME_FRAME_BUDGET`.

La valeur par défaut sur Pi peut être 33 ms pour 30 images/s. Le mode PC peut conserver une cadence différente si nécessaire. Le code doit mesurer le temps réellement écoulé et ne pas ajouter un second délai fixe.

## Entrées, gamepad et audio

### Gamepad USB

Ajouter un backend SDL2 GameController :

- initialiser `SDL_INIT_GAMECONTROLLER` ;
- ouvrir le gamepad au démarrage ;
- gérer le branchement à chaud ;
- lire boutons, axes et hat directionnel ;
- appliquer une zone morte ;
- convertir les événements en `KeyMask` ;
- conserver `pressed`, `justPressed` et `justReleased` ;
- accepter un mapping global ou un fichier `.keys` par jeu.

Le mapping de départ est :

| Commande physique | Touche logique |
|---|---|
| Croix directionnelle | `UP`, `DOWN`, `LEFT`, `RIGHT` |
| Bouton A ou bouton inférieur | `FIRE` |
| Bouton X ou L1 | `SOFT1` |
| Bouton B ou R1 | `SOFT2` |
| Start | Pause ou menu |
| Select | Sélection ou commande secondaire |
| Boutons supplémentaires | `*`, `#` ou actions configurables |

Une softkey ne doit jamais quitter le jeu. La commande quitter doit rester séparée et demander une confirmation si elle est liée à un bouton physique.

### Audio

Réutiliser le kernel audio et le backend SDL existants. Ajouter seulement les paramètres nécessaires au Pi :

```text
JME_AUDIO=sdl
JME_AUDIO_DEVICE=default
JME_AUDIO_LATENCY=16
```

Le format interne reste mono 16 bits à 22 050 Hz avec huit voix. Le renderer audio ne doit pas devenir synchrone avec le thread de rendu vidéo.

## Launcher et contrôles tactiles

Pi OS Lite ne fournit pas de bureau. Le produit doit donc embarquer son propre launcher SDL2, sans Qt, GTK, X11 ni fenêtre de gestion.

### Launcher natif SDL2

Le binaire `j2me_emu` doit pouvoir fonctionner dans deux états :

```text
launcher → sélection d’un jeu → émulation → retour au launcher
```

Le launcher doit :

- scanner un dossier de JAR configurable ;
- lire `META-INF/MANIFEST.MF` pour afficher le nom et la version ;
- présenter une liste ou une grille avec le bitmap font existant ;
- accepter clavier, gamepad et tactile ;
- lancer le JAR sélectionné ;
- revenir au menu après la fermeture du jeu ;
- proposer résolution, audio, volume, heap et commandes touch ;
- conserver un profil `.conf` ou `.keys` par jeu.

Le launcher et l’émulation doivent rester dans le même processus pour éviter de dupliquer le runtime et le heap. Le changement d’état doit détruire ou réinitialiser proprement le `Runtime`, les caches, l’audio et les pointeurs MIDP avant de charger le jeu suivant. Un superviseur de processus pourra être ajouté plus tard pour isoler les crashs.

### Overlay tactile pour les jeux

Un joystick physique ou virtuel ne remplace pas le tactile pour un MIDlet qui attend des événements de pointeur. Les deux modes doivent rester disponibles.

Le renderer doit proposer un overlay tactile optionnel, dessiné dans le framebuffer logique :

- joystick virtuel avec zone morte ;
- bouton `FIRE` ;
- softkeys `SOFT1` et `SOFT2` ;
- bouton pause/menu ;
- zones touch libres pour les jeux qui utilisent l’écran comme interface ;
- position, taille, opacité et inversion de l’axe configurables ;
- affichage possible ou masquable par profil de jeu.

L’overlay ne doit pas utiliser une fenêtre séparée. Il doit produire les événements du même chemin que le tactile physique :

```text
overlay ou écran tactile
→ InputState.pointer[]
→ jvm::midp::pointerEvent()
→ MIDlet
```

Les coordonnées doivent être converties vers la résolution logique après rotation, letterbox et mise à l’échelle. Le nombre de pointeurs simultanés doit rester compatible avec `hal::input.h:42` et pouvoir être augmenté si un jeu nécessite le multitouch.

Un gamepad USB ou un contrôleur GPIO peut être ajouté en parallèle. Ses boutons et axes sont convertis en `KeyMask`, tandis que l’overlay tactile produit les événements de pointeur. Un jeu peut donc choisir automatiquement son mode :

```text
JME_TOUCH_CONTROLS=auto
JME_TOUCH_CONTROLS=virtual
JME_TOUCH_CONTROLS=off
```

`auto` active l’overlay si le jeu a déclaré un profil tactile ou si aucune entrée physique n’est disponible.

### LVGL

LVGL reste une option pour un menu de paramétrage riche ou une interface tactile avancée. Il ne doit pas être requis pour le premier launcher et ne doit pas remplacer SDL2 dans le renderer du jeu. Le premier boîtier doit pouvoir fonctionner avec SDL2, le framebuffer RGB565, le bitmap font et les assets MIDP existants.

### Fichiers du launcher

| Fichier | Travail |
|---|---|
| `src/app/launcher/launcher.h` | États, callbacks et configuration du menu |
| `src/app/launcher/launcher.cpp` | Découverte des JAR, manifeste, sélection et lancement |
| `src/hal/touch_overlay.h` | Définition des zones et de la configuration tactile |
| `src/hal/touch_overlay.cpp` | Dessin du joystick virtuel et conversion des événements |
| `src/hal/input.h` | État tactile et mode des contrôles virtuels |
| `src/hal/input.cpp` | Intégration du tactile physique, de l’overlay et du gamepad |
| `src/app/main.cpp` | Boucle d’états launcher/émulation et retour au menu |

## Configuration et fichiers à modifier

### Configuration

Les nouvelles options Pi doivent être documentées dans `README.md` et `docs/AGENTS.md` :

| Option | Valeur Pi | Rôle |
|---|---:|---|
| `JME_HEAP` | `65536` | Heap initial en KiB |
| `JME_HEAP_MAX` | `131072` | Plafond du heap en KiB |
| `JME_ASSET_CACHE_MAX` | `32768` | Cache d’assets en KiB |
| `JME_RENDER_CACHE_MAX` | `16384` | Cache de rendu en KiB |
| `JME_FRAME_BUDGET` | `33` | Budget réel d’une image en ms (`JME_FRAME_TIME` = durée virtuelle, inchangé) |
| `JME_WINDOW_WIDTH` | `800` | Largeur de fenêtre physique |
| `JME_WINDOW_HEIGHT` | `480` | Hauteur de fenêtre physique |
| `JME_FULLSCREEN` | `1` | Plein écran |
| `JME_LAUNCHER` | `1` | Afficher le launcher au démarrage |
| `JME_TOUCH_CONTROLS` | `auto` | Mode `auto`, `virtual` ou `off` |
| `JME_TOUCH_LAYOUT` | `default` | Position et taille de l’overlay |
| `JME_EXIT_TO_LAUNCHER` | `1` | Revenir au menu après un jeu |
| `JME_RENDER_STATS` | `1` | Statistiques de rendu |
| `JME_AUDIO` | `sdl` | Backend audio |

Les valeurs existantes de `JME_WIDTH`, `JME_HEIGHT`, `JME_AUTOKEY`, `JME_AUTOTAPS`, `JME_KEYMAP`, `JME_DUMP` et `JME_MAXFRAMES` doivent continuer à fonctionner.

### Fichiers principaux

| Fichier | Travail |
|---|---|
| `CMakeLists.txt` | Ajouter le profil ARM64, les tests et les définitions de cible |
| `cmake/toolchains/aarch64-linux-gnu.cmake` | Cross-compilation AArch64 |
| `src/app/main.cpp` | Parser les limites, choisir le profil, configurer la cadence et afficher le rapport mémoire |
| `src/core/runtime.h` | Ajouter la limite maximale et les statistiques du heap |
| `src/core/runtime.cpp` | Refuser la croissance au-delà du plafond |
| `src/hal/asset_cache.h` | Définir l’API du cache d’assets |
| `src/hal/asset_cache.cpp` | Implémenter l’index, l’éviction et les compteurs |
| `src/core/render_cache.h` | Définir les sprites, tuiles et transformations réutilisables |
| `src/core/render_cache.cpp` | Implémenter le cache avec une capacité fixe |
| `src/hal/display.h` | Séparer dimensions logiques, fenêtre et plein écran |
| `src/hal/display.cpp` | Appliquer scaling entier, letterbox et mise à jour SDL unique |
| `src/midp/midp_graphics.cpp` | Ajouter les chemins rapides, le clipping et les statistiques |
| `src/hal/input.h` | Exposer le mapping gamepad et les états associés |
| `src/hal/input.cpp` | Ajouter SDL GameController, hotplug et zone morte |
| `tests/test_runtime.cpp` | Tester la limite du heap et les erreurs d’allocation |
| `tests/test_asset_cache.cpp` | Tester l’éviction, l’intégrité et les compteurs du cache |
| `tests/test_render_cache.cpp` | Tester les sprites, transformations et dimensions invalides |
| `tools/` | Ajouter le générateur de pack d’assets, sans dépendance runtime |

Les nouveaux modules doivent être ajoutés à `CMakeLists.txt` et à la commande de compilation directe. Le code ne doit pas dépendre de Python ou Rust à l’exécution.

## Plan d’implémentation

### Phase 1 : mesures et garde-fous

1. Ajouter les statistiques mémoire et rendu.
2. Ajouter `JME_HEAP_MAX` sans changer le comportement par défaut du PC.
3. Ajouter un profil `.conf` Pi Zero 2.
4. Mesurer le RSS, le chargement et le temps par image.
5. Valider les deux résolutions en headless.

### Phase 2 : build ARM64

1. Ajouter le toolchain AArch64.
2. Vérifier la compilation native sur Raspberry Pi OS Lite 64 bits.
3. Ajouter la vérification `sizeof(void *) == 8`.
4. Vérifier les chemins ARM64 des accès fichier et des assets.
5. Produire un binaire release reproductible.

### Phase 3 : caches

1. Ajouter le cache d’assets à capacité fixe.
2. Ajouter le cache d’images RGB565.
3. Ajouter l’éviction LRU et les ressources épinglées.
4. Pré-calculer les packs de quelques jeux de référence.
5. Vérifier que le JAR reste la source de secours.

### Phase 4 : renderer

1. Séparer framebuffer logique et fenêtre physique.
2. Ajouter le scaling entier et le letterbox.
3. Optimiser les blits opaques, alpha et rectangles.
4. Ajouter le cache de tuiles et sprites.
5. Activer les statistiques de rendu.
6. Comparer CPU, GPU et chemin hybride sur le Pi.

### Phase 5 : entrées et intégration

1. Ajouter le backend SDL GameController.
2. Ajouter le mapping par jeu.
3. Tester clavier, tactile et gamepad simultanément.
4. Vérifier l’audio SDL et la persistance RMS.
5. Ajouter un script de déploiement sur microSD.

## Tests et validation

### Tests automatisés

Conserver la suite CTest existante et ajouter :

- test de construction du heap avec limite maximale ;
- test d’OOM sans écriture hors buffer ;
- test de `reset()` avec plusieurs segments ;
- test de parsing des valeurs `JME_HEAP*` ;
- test d’éviction LRU du cache d’assets ;
- test d’intégrité d’un pack versionné ;
- test de scaling, rotation et letterbox ;
- test de clipping et de copie alpha ;
- test de mapping gamepad vers `KeyMask` ;
- test de conservation de `justPressed` et `justReleased` ;
- test de présentation sans framebuffer supplémentaire inutile.

Le build PC doit continuer à passer les tests existants. Les tests de rendu peuvent utiliser un renderer logiciel sans fenêtre SDL.

### Tests sur Pi Zero 2 W

Pour chaque jeu de référence :

1. vérifier le chargement du manifeste et de la classe principale ;
2. lancer 240×320 pendant au moins 300 frames ;
3. lancer le profil 800×480 si le jeu le demande ;
4. mesurer le RSS et le pic mémoire ;
5. compter les images manquées et la latence ;
6. tester un gamepad et un clavier ;
7. tester une sauvegarde RMS après redémarrage ;
8. tester le mode audio SDL puis le mode stub.

### Critères d’acceptation

La cible est acceptable si :

- le binaire démarre en ARM64 sur Raspberry Pi OS Lite 64 bits ;
- le heap ne dépasse jamais `JME_HEAP_MAX` ;
- le cache d’assets et le cache de rendu respectent leurs limites ;
- le JAR n’est jamais slurpé dans un buffer unique ;
- 240×320 reste jouable à 30 images/s pour les jeux ciblés ;
- 800×480 est au minimum fonctionnel pour les jeux compatibles ;
- le profil d’un jeu tient dans le budget RSS de 224 MiB ;
- le seuil de 256 MiB déclenche un rapport et une revue ;
- les softkeys ne quittent jamais le jeu ;
- le gamepad produit les mêmes états logiques que le clavier ;
- le tactile physique et l’overlay virtuel produisent des `PointerEvent` valides ;
- le launcher démarre sans desktop et revient au menu après un jeu ;
- l’audio ne produit pas de claquements prolongés ;
- les tests PC et ARM64 passent.

Ces critères sont des objectifs de validation, pas une garantie pour chaque MIDlet.

## Risques et décisions

### Risques

- le CPU du Pi Zero 2 W peut rester le facteur limitant même avec assez de RAM ;
- la conversion 64 bits augmente la taille des pointeurs et de certaines structures STL ;
- le cache peut consommer plus de mémoire si les clés ne sont pas bornées ;
- la décompression d’une grosse image peut provoquer un pic temporaire ;
- les jeux qui créent beaucoup d’objets restent limités par l’absence de GC ;
- SDL2 peut choisir un backend vidéo différent selon l’écran et le driver ;
- le gamepad USB OTG peut nécessiter un hub alimenté ;
- les jeux 800×480 peuvent dépasser le budget de temps même si la mémoire est disponible.

### Décisions

- Le Pi Zero 2 W est une cible de prototype et de jeux 2D, pas une garantie pour les MIDlets AAA.
- Le Pi 4 reste la cible de comparaison pour les jeux lourds.
- Le 64 bits est activé pour le système et le binaire, mais aucune optimisation ne doit supposer un gain automatique.
- Le renderer optimise les images et sprites, pas des frames complètes.
- Le pack d’assets est optionnel et doit pouvoir être régénéré.
- Toute augmentation au-delà de 128 MiB de heap doit être justifiée par des mesures et validée sur le Pi.

## Définition de terminé

La modification est terminée quand :

- le profil Pi Zero 2 W est construit en ARM64 ;
- les limites mémoire sont configurables et testées ;
- le cache d’assets respecte son budget ;
- le renderer utilise le framebuffer logique avec scaling déterministe ;
- le gamepad est reconnu et configurable ;
- les stats de mémoire, cache et rendu sont disponibles ;
- les tests PC et les smoke tests Pi passent ;
- la documentation `README.md` et `docs/AGENTS.md` décrit les nouvelles options ;
- aucun changement non lié n’est inclus dans le même livrable.

## Références du dépôt

- `docs/HARDWARE_TARGET.md` : choix de la carte, de l’écran, de l’audio et des commandes ;
- `docs/PYTHON_RUST.md` : outillage externe optionnel ;
- `docs/AGENTS.md` : build, tests et contraintes de portage ;
- `docs/INTEGRATION.md` : contrats JAR, buffers et intégration ;
- `CMakeLists.txt` : sources et tests actuels ;
- `src/app/main.cpp` : configuration et boucle principale ;
- `src/core/runtime.h` et `src/core/runtime.cpp` : heap et chargement des classes ;
- `src/hal/jar_reader.h` : lecture JAR sans slurp ;
- `src/hal/display.cpp` : framebuffer et présentation SDL ;
- `src/hal/input.cpp` : état des entrées ;
- `src/midp/midp_graphics.cpp` : rendu MIDP ;
- `src/kernel/audio/` : mixer et backends audio.
