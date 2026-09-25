# Cible matérielle et kernel — J2ME MIDP Emulator

## Décision courte

La cible recommandée pour faire tourner la VM actuelle est :

- **Raspberry Pi 4 Model B 4 Go** sous Raspberry Pi OS Lite 64 bits ;
- exécutable C++17 construit avec CMake et SDL2 ;
- écran tactile **800×480 via DSI** ou écran HDMI tactile 800×480 ;
- gamepad USB HID reconnu par SDL2 ;
- sortie audio analogique du Pi ou périphérique USB ;
- carte microSD de 32 à 64 Go ;
- alimentation officielle 5,1 V/3 A et refroidissement actif.

Le Pi 4 2 Go peut être utilisé pour un prototype. Le 4 Go est la référence recommandée pour laisser de la place au système, à SDL, aux assets, aux sauvegardes et aux jeux qui utilisent `JME_HEAP=4096`.

| Besoin | Choix recommandé | Raison |
|---|---|---|
| Hôte | Raspberry Pi 4 Model B 4 Go | ARM 64 bits, Linux, SDL2 et mémoire suffisante |
| Écran | Touch Display 7 pouces 800×480 DSI, ou HDMI tactile 800×480 | Correspond aux jeux paysagés et reste simple à intégrer |
| Audio | Sortie 3,5 mm ou USB | Réutilise le backend SDL sans nouveau driver |
| Commandes | Gamepad USB HID | SDL2 fournit une base standard et mappings configurables |
| Stockage | microSD A2/U3 32–64 Go | Système, JAR et RMS sur le même support |
| Alimentation | 5,1 V/3 A avec refroidissement | Évite les chutes de tension pendant le décodage |

Le **RP2040 ne peut pas exécuter la VM actuelle**. Il peut rester un contrôleur d’entrées, un module audio ou un coprocesseur, mais pas la machine hôte de l’émulateur sans une refonte majeure.

## Portée du document

Ce document répond à quatre questions :

1. Quelle carte peut réellement exécuter le runtime actuel ?
2. Quel kernel et quels pilotes sont nécessaires ?
3. Quel écran et quel audio utiliser ?
4. Comment fournir les touches J2ME et les commandes de jeu ?

Les chiffres de mémoire sont des estimations structurelles issus du code actuel. Ils ne remplacent pas une mesure réelle sur la carte cible.

## Pourquoi le RP2040 ne convient pas

Le Pico/RP2040 possède 264 Ko de SRAM interne et un processeur Cortex-M0+ à deux cœurs, avec une fréquence maximale de 133 MHz. Le flash externe ne change pas cette limite de SRAM.

La VM actuelle possède déjà plusieurs allocations structurelles importantes :

| Élément | Taille actuelle | Emplacement |
|---|---:|---|
| Buffer d’extraction de classe | 1 MiB | `vm/runtime.cpp:364` |
| Heap objet initial | 512 KiB | `vm/runtime.h:175` |
| Arène des frames d’appel | 128 KiB | `vm/interpreter.cpp:186` |
| Deux framebuffers 240×320 RGB565 | environ 300 KiB | `hal/display.cpp:35` et layer MIDP |
| Pile d’une fibre `Thread.start()` | 256 KiB | `vm/natives.cpp:170` |

Le cumul de base dépasse déjà **1,9 MiB**, avant les images, les métadonnées de classes, les chaînes STL, le JAR, l’audio et les sauvegardes. Une seule fibre ajoute 256 KiB. Le jeu en mode paysage `800×480` demande environ 750 KiB par framebuffer RGB565, donc environ 1,5 MiB pour les deux copies.

Le heap peut être configuré avec `JME_HEAP=4096`, soit 4 MiB pour le segment initial. Le code peut ensuite s’étendre automatiquement ; cela augmente la pression mémoire au lieu de résoudre le problème.

Le projet utilise aussi plusieurs dépendances Linux ou POSIX :

- C++17, STL et `ucontext` pour les fibres ;
- SDL2 pour l’affichage, le son et les événements ;
- accès fichier de type `FILE*` ;
- threads et callbacks audio du système.

Le runtime n’est pas encore un runtime MCU minimal : le heap est un bump allocator sans GC et les structures du runtime utilisent notamment `std::vector` et `std::unique_ptr`. La mémoire, les segments et la stratégie de libération doivent donc être mesurés avant tout portage vers un microcontrôleur.

Un Pico avec le Pico SDK ne fournit pas ces services par défaut. Ajouter de la PSRAM, une carte SD, un écran et un codec ne suffit pas : il faudrait également réécrire le modèle mémoire, l’ordonnanceur, les backends HAL et plusieurs chemins d’E/S.

### Conclusion RP2040

Le RP2040 est exclu comme hôte de la VM actuelle. Il peut être réutilisé dans trois rôles utiles :

1. contrôleur de boutons USB HID pour un Pi ;
2. contrôleur de joystick et de matrice de touches ;
3. module audio ou afficheur secondaire avec une interface précise.

## Cartes comparées

| Carte | Mémoire | CPU | Verdict |
|---|---:|---|---|
| Raspberry Pi Pico/RP2040 | 264 Ko SRAM | Cortex-M0+ | Impossible pour la VM actuelle |
| Raspberry Pi Zero 2 W | 512 Mo | Cortex-A53 quad-core 1 GHz | Prototype très contraint, non recommandé comme référence |
| Raspberry Pi 4 Model B 2 Go | 2 Go | Cortex-A72 quad-core | Minimum plausible pour les jeux légers |
| Raspberry Pi 4 Model B 4 Go | 4 Go | Cortex-A72 quad-core | **Cible recommandée** |
| Raspberry Pi 4 Model B 8 Go | 8 Go | Cortex-A72 quad-core | Utile pour les assets lourds et les évolutions futures |
| Raspberry Pi 5 4 ou 8 Go | 4 à 8 Go | Cortex-A76 quad-core 2,4 GHz | Choix performance, avec alimentation 5 V/5 A |
| Raspberry Pi Compute Module 5 4 Go | 4 Go + eMMC optionnel | Cortex-A76 quad-core 2,4 GHz | Meilleur choix pour un produit embarqué compact |

### Choix conseillé

- **Prototype économique** : Pi 4 2 Go, écran HDMI et gamepad USB.
- **Référence de développement** : Pi 4 4 Go, écran DSI 800×480, gamepad USB.
- **Produit final** : Compute Module 5 4 Go avec IO board, stockage eMMC et carte porteuse.
- **Option haute performance** : Pi 5 4 Go ou 8 Go avec refroidissement actif.

Le Pi Zero 2 W possède assez de RAM physique pour charger le processus, mais son CPU et son interface graphique sont moins adaptés aux pics de bytecode, de DEFLATE et de décodage d’images. Il ne doit pas être considéré comme équivalent au Pi 4.

## Recommandation matérielle de référence

### Carte et système

- Raspberry Pi 4 Model B 4 Go ;
- Raspberry Pi OS Lite 64 bits ;
- carte microSD A2/U3 de 32 à 64 Go ;
- alimentation officielle 5,1 V/3 A ;
- dissipateur ou refroidisseur actif ;
- Gigabit Ethernet pour le développement, Wi-Fi seulement si nécessaire.

Le système Linux fournit la gestion des processus, de la mémoire, de la pile réseau, des périphériques USB, de l’audio, de la carte SD et du framebuffer. Le projet n’a pas besoin de réimplémenter ces services.

### Stockage

Pour un prototype :

```text
microSD → Raspberry Pi OS + binaire + games/
```

Pour un produit :

```text
CM5 eMMC → système et données
SPI/QSPI ou USB → stockage des JAR et sauvegardes
```

Le contrat de stockage du projet existe dans `kernel/drivers/storage/storage.h`, mais le backend SD/QSPI doit être implémenté si l’on quitte Linux. Sur Pi, `hal/file.cpp` peut rester le point d’accès `FILE*`, avec un chemin RMS contrôlé et sans appel shell fragile.

## Quel écran choisir

### Choix recommandé : écran tactile 800×480

Pour une première machine autonome, l’écran doit avoir une résolution physique de 800×480 en paysage. Cette résolution correspond au mode connu pour Assassin’s Creed 2 :

```bash
JME_WIDTH=800 JME_HEIGHT=480 JME_HEAP=4096 ./build/j2me_emu games/assasin.jar
```

Deux interfaces sont possibles :

| Interface | Avantage | Inconvénient | Verdict |
|---|---|---|---|
| DSI | Compacte, pas de câble HDMI, officielle | Résolution et câblage propres à l’écran | Recommandé pour un boîtier |
| HDMI + USB touch | Très flexible, facile à remplacer | Deux câbles et besoin d’un écran tactile compatible | Recommandé pour le prototypage |
| SPI | Peu de broches et modules abordables | Débit insuffisant pour le confort de la VM | À éviter pour le format principal |

L’écran tactile officiel Raspberry Pi Touch Display 7 pouces est une base 800×480 adaptée au Pi 4. La Touch Display 2 de 5 ou 7 pouces offre une résolution supérieure de 1280×720 en paysage et peut être envisagée pour un produit plus spacieux, mais elle demande une configuration d’affichage différente. Le Raspberry Pi Zero 2 W n’est pas compatible avec l’écran DSI officiel et doit utiliser une solution HDMI ou un autre module d’affichage.

### Point important dans le code actuel

`hal/display.cpp:17-20` crée une fenêtre SDL de `2 × largeur` par `2 × hauteur`. Avec `JME_WIDTH=800` et `JME_HEIGHT=480`, la fenêtre demandée fait donc 1600×960, même si le framebuffer émulé fait 800×480.

Avant d’intégrer un écran physique, il faut séparer :

- la résolution logique du MIDlet ;
- la résolution physique de la fenêtre ;
- le mode plein écran ;
- l’échelle d’affichage.

Sans cette séparation, un écran 800×480 peut afficher une fenêtre rognée ou laisser une grande zone noire. Le framebuffer VM peut rester 800×480 même si l’écran est 1280×720.

### Tactile

Le tactile peut être traité par SDL comme des événements de souris sur les écrans compatibles, mais il faut vérifier la rotation, les coordonnées et le multitouch. Le code actuel convertit les événements de souris en coordonnées logiques ; un écran DSI doit donc être testé dans les orientations portrait et paysage.

## Quel module audio

### Première solution : sortie audio du Pi

Le Pi 4 possède une sortie audio analogique 3,5 mm. Le backend SDL peut donc utiliser le périphérique audio Linux sans ajouter de module :

```bash
JME_AUDIO=sdl ./build/j2me_emu games/assasin.jar
```

Cette solution est la plus simple pour :

- un casque ;
- des écouteurs ;
- un petit haut-parleur amplifié ;
- un test de développement.

### Module DAC externe

Si la qualité analogique ou la connectique doit être meilleure, un Raspberry Pi DAC+ peut être envisagé. Il fournit une sortie DAC et un amplificateur pour casque via le GPIO 40 broches. Le processus devra sélectionner le périphérique ALSA correspondant.

### Module I2S

Pour un produit avec un codec et un amplificateur contrôlés, une carte I2S comme Codec Zero peut être utilisée. Le backend devra alors passer par ALSA/I2S ou ajouter un périphérique audio au kernel projet.

Le kernel audio actuel est déjà défini pour :

- mono 16 bits ;
- 22 050 Hz ;
- huit voix ;
- mélangeur sans allocation ;
- tons et séquences tonales.

Ce choix réduit la charge du backend, mais ne dispense pas de vérifier la latence, le reséchantillonnage, le volume et la stabilité des callbacks SDL.

### Recommandation audio

1. sortie 3,5 mm ou USB pour le prototype ;
2. DAC+ si une sortie ligne propre est nécessaire ;
3. I2S uniquement pour le produit final ;
4. éviter une sortie PWM comme qualité audio principale.

## Touches et commandes de jeu

### Touches logiques existantes

Le projet attend les touches suivantes dans `hal/input.h:8-30` :

```text
UP DOWN LEFT RIGHT
FIRE SOFT1 SOFT2
0 1 2 3 4 5 6 7 8 9
* #
```

Les états sont déjà structurés en `pressed`, `justPressed` et `justReleased`. Il faut conserver ces trois notions pour que les MIDlets qui utilisent `keyPressed` et `keyReleased` fonctionnent correctement.

### Solution recommandée : gamepad USB HID

Le gamepad USB HID est la solution la plus simple sur Pi. SDL2 peut ouvrir un contrôleur reconnu comme gamepad et convertir ses boutons et axes en événements logiques.

Le code actuel de `hal/input.cpp` ne traite encore que le clavier, la souris et les événements tactiles indirects. Il n’y a pas encore de backend `SDL_GameController`.

Le travail à prévoir est le suivant :

- ouvrir le gamepad au démarrage ;
- accepter le branchement à chaud ;
- lire les boutons, axes et hat ;
- appliquer une zone morte sur les sticks ;
- convertir les événements en `KeyMask` ;
- conserver `justPressed` et `justReleased` ;
- fournir un mapping par jeu ou global ;
- mapper une commande de sortie qui ne soit jamais une softkey.

### Mapping de départ

| Fonction physique | Touche logique proposée |
|---|---|
| Croix directionnelle | `UP`, `DOWN`, `LEFT`, `RIGHT` |
| Bouton A ou bouton inférieur | `FIRE` |
| Bouton X ou L1 | `SOFT1` |
| Bouton B ou R1 | `SOFT2` |
| Start | Pause ou menu, jamais quitter par un appui simple |
| Select | Sélection ou commande secondaire |
| Touches d’épaule ou secondaires | `*` et `#`, si le jeu les utilise |
| Touche dédiée | Quitter ou retour hardware, avec confirmation |

Le mapping doit être configurable, car les jeux Nokia et les jeux J2ME n’utilisent pas les mêmes touches pour les actions et les menus.

### Compatibilité avec les jeux de téléphone

Pour les MIDlets qui affichent un clavier numérique, il faut aussi fournir `0` à `9`, `*` et `#`. Un gamepad classique ne possède pas ces 12 touches du pavé numérique. Trois solutions sont possibles :

- gamepad avec boutons supplémentaires ;
- pavé numérique USB séparé ;
- combinaison de touches pour le mode téléphone.

Le mode téléphone doit être activable sans rendre les boutons de menu ambigus.

### Solution GPIO pour un boîtier compact

Si le boîtier ne doit pas avoir de gamepad USB, les touches peuvent être lues sur le GPIO du Pi :

- D-pad en croix ou matrice ;
- bouton FIRE central ;
- deux boutons d’épaule ;
- Start et Select ;
- boutons dédiés pour `*` et `#` ;
- joystick analogique via ADC si nécessaire.

Le contrat input du kernel prévoit déjà un balayage périodique, mais le backend GPIO doit être ajouté. Le balayage doit inclure :

- anti-rebond de 20 à 30 ms ;
- état pressed stable ;
- événement pressed et released une seule fois ;
- détection de combinaisons ;
- sécurité électrique des entrées.

Les GPIO du Pi sont en logique 3,3 V et ne doivent pas recevoir directement un signal 5 V. Un convertisseur de niveaux est nécessaire pour une matrice ou un contrôleur externe alimenté en 5 V.

### RP2040 comme coprocesseur de touches

Un RP2040 peut être utilisé pour :

- parcourir une grande matrice de boutons ;
- lire un joystick analogique ;
- gérer l’anti-rebond des touches ;
- envoyer un rapport USB HID au Pi.

Le Pi reste alors responsable de la VM, de l’écran, du stockage et de l’audio. Cette séparation est plus réaliste que de faire tenir toute la VM sur le RP2040.

## Kernel nécessaire

### Le kernel projet

`kernel/kernel.h` n’est pas un kernel Linux. C’est une petite couche portable qui fournit :

- une horloge monotone ;
- un registre de pilotes ;
- l’initialisation et l’arrêt des drivers ;
- un contrat audio ;
- des contrats display, input et storage.

Le kernel projet ne doit pas allouer dynamiquement ses tables de drivers. Sa taille est négligeable comparée à la VM ; le problème principal est la mémoire de la VM, pas cette couche.

### Kernel Linux sur Raspberry Pi

Sur Pi, le kernel Linux doit gérer :

| Domaine | Backend initial |
|---|---|
| Processus et mémoire | Noyau Linux et Raspberry Pi OS |
| Affichage | SDL2 avec X11, KMSDRM ou framebuffer |
| Entrée clavier/souris | SDL2 |
| Gamepad | SDL2 `SDL_GameController` |
| Tactile | SDL2 ou pilote d’entrée Linux |
| Audio | SDL2/ALSA |
| Stockage JAR | `FILE*` sur microSD ou SSD |
| RMS | Répertoire contrôlé sur microSD |
| Horloge | `SDL_GetTicks()` ou horloge Linux monotone |
| Réseau | Wi-Fi ou Ethernet, optionnel |

Le projet peut conserver `hal/file.cpp` et les backends SDL pour une première version Pi. Le kernel projet devient alors une couche d’abstraction au-dessus des services Linux, plutôt qu’un noyau bare-metal.

## Adaptation de l’affichage

La cible physique doit gérer deux résolutions :

```text
résolution émulée : 240×320, 800×480 ou autre
résolution écran  : 800×480, 1280×720 ou autre
```

Le flux recommandé est :

```text
MIDlet → framebuffer RGB565 logique → scale/letterbox → framebuffer physique
```

Au lieu d’allouer un framebuffer physique de la taille de l’écran pour chaque test, le Pi peut garder le framebuffer logique et laisser SDL mettre à l’échelle. Pour les jeux 800×480, une seule copie 800×480 est déjà préférable à deux copies si le code peut être simplifié.

## Plan de portage vers Pi

### Phase 1 : prototype headless

1. Installer Raspberry Pi OS Lite 64 bits.
2. Installer le compilateur, CMake, SDL2 et `pkg-config`.
3. Compiler le projet directement sur le Pi pour éviter les erreurs de cross-compilation.
4. Tester en 240×320 avec les JAR existants.
5. Tester `JME_WIDTH=800 JME_HEIGHT=480 JME_HEAP=4096`.
6. Mesurer la RSS, le temps de chargement, le nombre de frames et les erreurs de heap.

Commande de référence :

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
SDL_VIDEODRIVER=dummy JME_AUDIO=stub JME_MAXFRAMES=120 \
    JME_WIDTH=800 JME_HEIGHT=480 JME_HEAP=4096 \
    ./build/j2me_emu games/assasin.jar
```

### Phase 2 : affichage et audio réels

1. Activer un écran HDMI 800×480.
2. Séparer la taille logique de la taille de la fenêtre.
3. Ajouter le plein écran et une mise à l’échelle letterbox.
4. Tester un écran DSI 800×480.
5. Tester la sortie 3,5 mm ou USB.
6. Vérifier la latence audio et la stabilité sur plusieurs minutes.

### Phase 3 : gamepad

1. Ajouter `SDL_INIT_GAMECONTROLLER` ou l’équivalent SDL2 utilisé par le projet.
2. Ajouter le mapping gamepad vers `KeyMask`.
3. Tester pressed, justPressed et justReleased.
4. Ajouter un fichier de mapping par jeu.
5. Ajouter une commande Start longue pour quitter ou revenir au launcher.

### Phase 4 : boîtier portable

1. Remplacer le clavier par un gamepad ou une matrice GPIO.
2. Ajouter un amplificateur et un haut-parleur si nécessaire.
3. Ajouter une carte de charge et de protection de batterie.
4. Remplacer le chemin RMS shell par une API de fichiers contrôlée.
5. Ajouter des limites de taille, de temps et de mémoire pour les entrées.
6. Tester les coupures d’alimentation, la température et le stockage plein.

## Critères de validation

La cible Pi ne sera jugée compatible qu’après validation des points suivants :

- le binaire démarre sans erreur de segmentation ;
- le manifeste et la classe principale sont chargés ;
- 240×320 et 800×480 sont rendus correctement ;
- le heap ne devient pas le premier facteur d’échec ;
- le jeu tient la cadence de 30 images/s ou la limite est documentée ;
- les softkeys ne quittent pas le jeu ;
- les événements pressed/released sont corrects ;
- l’audio ne produit pas de claquement prolongé ;
- la sauvegarde RMS est persistante ;
- le boot et les jeux fonctionnent avec le stockage choisi ;
- la température reste acceptable dans le boîtier.

## Recommandation finale

Commencer avec un **Pi 4 Model B 4 Go**, un écran **800×480 DSI ou HDMI tactile**, un **gamepad USB HID**, une sortie **3,5 mm/USB**, une carte **microSD 32–64 Go** et une alimentation **5,1 V/3 A**.

Utiliser un **Pi 5** lorsque la performance, le PCIe ou le produit final le justifie. Utiliser un **Compute Module 5** lorsque l’intégration dans un boîtier compact et le stockage eMMC deviennent importants.

Conserver le **RP2040 uniquement comme coprocesseur d’entrée ou module spécialisé**. Le déplacer vers une architecture ARM/Linux avec beaucoup plus de RAM est beaucoup plus raisonnable que de tenter de faire tenir la VM actuelle dans ses 264 Ko de SRAM.

## Sources matérielles

Les spécifications suivantes ont été consultées le 25 septembre 2026 pour cette décision. Elles n’ont pas encore été validées par des mesures sur le matériel cible ; les disponibilités et les références commerciales peuvent changer.

- [Raspberry Pi 4 Model B — spécifications](https://www.raspberrypi.com/products/raspberry-pi-4-model-b/specifications/)
- [Raspberry Pi Zero 2 W](https://www.raspberrypi.com/products/raspberry-pi-zero-2-w/)
- [Raspberry Pi Pico / RP2040](https://www.raspberrypi.com/products/raspberry-pi-pico/)
- [Raspberry Pi Touch Display](https://www.raspberrypi.com/documentation/accessories/display.html)
- [Raspberry Pi Touch Display 2](https://www.raspberrypi.com/documentation/accessories/touch-display-2.html)
- [Raspberry Pi 5](https://www.raspberrypi.com/products/raspberry-pi-5/)
- [Raspberry Pi Compute Module 5](https://www.raspberrypi.com/products/compute-module-5/)
- [Raspberry Pi DAC+](https://www.raspberrypi.com/products/dac-plus/)
- [Raspberry Pi Codec Zero](https://www.raspberrypi.com/products/codec-zero/)
- [SDL2 `SDL_GameControllerOpen`](https://wiki.libsdl.org/SDL2/SDL_GameControllerOpen)
- [Raspberry Pi : installation et stockage](https://www.raspberrypi.com/documentation/computers/getting-started.html)
