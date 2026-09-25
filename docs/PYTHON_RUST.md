# Pourquoi ajouter Python et Rust à ce projet

## Réponse courte

L’ajout de Python et de Rust n’est pas nécessaire pour compiler ou exécuter l’émulateur actuel. Il est pertinent si le projet veut améliorer son outillage, automatiser les tests, analyser des fichiers MIDP et fiabiliser progressivement le traitement des entrées non fiables.

Les rôles doivent rester séparés :

- **C++17** reste le cœur de l’émulateur, de l’interpréteur, du runtime et des pilotes.
- **Python** sert d’outillage hôte : analyse, génération de données, scripts, rapports et intégration CI.
- **Rust** sert d’outillage natif pour valider et tester les formats complexes au niveau de la frontière de confiance, avec une empreinte mémoire et un contrôle plus strict.

L’objectif n’est donc pas de remplacer le C++, mais de mieux le protéger et de réduire le temps de développement.

## Contexte du projet

Le projet actuel suit ce pipeline :

```text
JAR/ZIP → DEFLATE → fichiers .class → runtime → bytecode → natives MIDP → HAL
```

Il possède plusieurs caractéristiques qui rendent un outillage supplémentaire intéressant :

- le bytecode Java et les formats binaires proviennent de fichiers externes ;
- les entrées ne sont pas encore considérées comme sûres ;
- le portage RP2040 impose des contraintes fortes de mémoire et d’allocation ;
- les tests doivent couvrir les décodeurs, le runtime, le rendu, l’audio et les formats de ressources ;
- les deux émulateurs de référence sont utilisés dans des scénarios headless et avec des captures framebuffer.

La documentation signale déjà que l’émulateur ne doit pas exécuter de JAR non fiables tant que la validation du bytecode et des formats d’entrée n’est pas complète. C’est le principal domaine où Rust peut apporter une valeur immédiate.

## Pourquoi ajouter Python

Python est adapté aux outils qui doivent être faciles à écrire, faciles à maintenir et indépendants du code embarqué.

### Usages recommandés

| Besoin | Utilisation possible |
|---|---|
| Inspection de JAR | Lire le manifeste, lister les classes et les ressources, calculer des empreintes |
| Analyse de `.class` | Extraire la version, les méthodes, les attributs et les références |
| Génération de tests | Créer des petits JAR, des classes invalides et des cas limites |
| Vérification des ressources | Contrôler les dimensions PNG, les en-têtes WAV et les formats attendus |
| Tests de régression | Lancer l’émulateur en headless, capturer les logs et comparer les résultats |
| Analyse des captures | Inspecter les fichiers PPM, vérifier les dimensions et produire des statistiques |
| Génération de rapports | Créer des tableaux de compatibilité, des listes de méthodes natives manquantes |
| Automatisation CI | Préparer les jeux, envoyer les variables d’environnement et collecter les artefacts |

Python permet aussi de créer rapidement un prototype d’outil sans modifier le noyau C++. Un script peut utiliser la bibliothèque standard (`zipfile`, `struct`, `hashlib`, `json`, `pathlib`) sans imposer de dépendance lourde au projet.

### Exemples d’outils Python

```text
tools/python/
├── pyproject.toml
├── src/j2me_tools/
│   ├── inspect_jar.py
│   ├── inspect_class.py
│   ├── inspect_media.py
│   ├── generate_fixtures.py
│   └── run_smoke_tests.py
└── tests/
```

Un outil d’inspection pourrait accepter un JAR et produire un rapport JSON contenant :

- le manifeste MIDlet ;
- la version des classes ;
- les ressources ;
- les méthodes référencées mais non natives ;
- les tailles et empreintes des entrées ;
- les avertissements de compatibilité.

Ce type de rapport facilite le travail avec plusieurs MIDlets sans avoir à modifier l’émulateur pour chaque inspection.

### Limites de Python

Python ne doit pas devenir une dépendance du binaire `j2me_emu` ou du portage RP2040. Il est également moins adapté à un parseur qui doit rester dans une mémoire très contrainte ou à un composant embarqué temps réel.

## Pourquoi ajouter Rust

Rust est pertinent pour les outils qui manipulent des données non fiables et doivent détecter rapidement les erreurs de mémoire, les dépassements de tampon et les états invalides.

### Usages recommandés

| Besoin | Utilisation possible |
|---|---|
| Validation de JAR | Vérifier les structures ZIP, les offsets, les tailles et les relations entre entrées |
| Validation de `.class` | Contrôler les limites des attributs, du bytecode, des indices et des types |
| Fuzzing | Tester ZIP, DEFLATE, PNG, WAV et parseurs de classes avec des entrées mutées |
| Outil natif CI | Produire un binaire autonome avec un code de sortie et une sortie JSON déterministes |
| Référence indépendante | Comparer les résultats d’un parseur Rust avec ceux de l’implémentation C++ |
| Traitement contraint | Traiter de gros flux avec des limites explicites de taille, de temps et de mémoire |

Rust peut ainsi servir de première barrière avant l’exécution d’un JAR. Le programme principal resterait en C++, mais un validateur externe pourrait être appelé par la CI ou par un mode de diagnostic.

### Exemples d’outils Rust

```text
tools/rust/
├── Cargo.toml
├── crates/
│   └── j2me-validate/
│       ├── Cargo.toml
│       └── src/main.rs
└── fuzz/
    ├── zip_fuzzer.rs
    ├── class_fuzzer.rs
    ├── png_fuzzer.rs
    └── wav_fuzzer.rs
```

Le validateur devrait utiliser des limites explicites pour :

- la taille d’un JAR et de chaque entrée ;
- la profondeur d’un ZIP ou d’un DEFLATE ;
- la taille décompressée ;
- la longueur d’une classe et d’une méthode ;
- le nombre de références, d’attributs et d’images ;
- la durée et la mémoire d’une analyse.

Rust améliore la sécurité mémoire du nouvel outil, mais ne garantit pas à lui seul que le bytecode ou la logique J2ME sont corrects. Les tests de fuzzing, les limites de ressources et les tests de régression restent obligatoires.

### Limites de Rust

L’ajout de Rust introduit une seconde toolchain, un second système de dépendances et un second format de configuration. Le projet devient plus difficile à compiler sur les environnements minimalistes et les publications deviennent plus complexes.

Il ne faut donc pas remplacer le runtime C++ par Rust sans mesurer un gain concret. Le portage RP2040 ne doit pas embarquer de runtime Rust ni de bibliothèque Rust dans le cœur de l’émulateur sans démonstration claire de son bénéfice.

## Pourquoi utiliser les deux

Python et Rust sont complémentaires :

- Python réduit le coût de création des outils et facilite l’automatisation ;
- Rust fournit un meilleur environnement pour les validateurs et les fuzzers ;
- C++ conserve les performances, le contrôle de la mémoire et la compatibilité avec SDL2 et le matériel.

| Besoin | Outil recommandé |
|---|---|
| Script d’analyse rapide | Python |
| Génération de fixtures | Python |
| Rapport de compatibilité | Python |
| Validation d’entrée non fiable | Rust |
| Fuzzing de formats binaires | Rust |
| Émulation et rendu | C++17 |
| Pilotes PC et MCU | C++17 |
| Orchestration CI | Python ou script shell |
| Runtime sur RP2040 | C++ uniquement, sauf preuve contraire |

Une bonne séparation consiste à faire communiquer les outils par des fichiers ou une interface CLI stable, plutôt que par un langage interne commun. Par exemple :

```text
JAR → j2me-inspect (Python) → rapport JSON
JAR → j2me-validate (Rust) → rapport JSON ou code de sortie
C++ → tests d’intégration et captures PPM
```

## Ce qu’il ne faut pas faire

- Ne pas embarquer Python dans `j2me_emu`.
- Ne pas ajouter un runtime Rust au portage RP2040 par défaut.
- Ne pas réécrire l’émulateur en Python ou en Rust sans benchmark préalable.
- Ne pas faire coexister plusieurs parseurs sans vecteurs de test communs et règles de versionnement.
- Ne pas exécuter un validateur Rust non borné sur un JAR provenant d’Internet.
- Ne pas dépendre de paquets réseau non épinglés dans une build reproductible.
- Ne pas considérer un test réussi avec deux jeux comme une garantie de compatibilité ou de sécurité.
- Ne pas laisser les outils devenir la source de vérité à la place des APIs C++ : ils doivent produire des diagnostics reproductibles.

## Plan d’adoption recommandé

### Phase 1 : Python, sans modifier le cœur

1. Ajouter `tools/python/` avec un empaquetage minimal.
2. Créer `j2me-inspect` pour analyser les manifestes, classes et ressources.
3. Ajouter un générateur de petits JAR et de fixtures invalides.
4. Ajouter un runner de smoke tests utilisant `JME_MAXFRAMES` et `SDL_VIDEODRIVER=dummy`.
5. Publier les rapports dans la CI sans les intégrer à l’exécutable.

Cette phase est peu risquée et apporte immédiatement de la valeur pour le débogage et la documentation.

### Phase 2 : Rust pour la frontière de confiance

1. Définir un format JSON stable pour les résultats du validateur.
2. Implémenter un premier validateur de structure JAR et de limites de ressources.
3. Ajouter des tests avec des classes, offsets et tailles corrompus.
4. Ajouter des cibles de fuzzing séparées pour JAR, DEFLATE, PNG, WAV et `.class`.
5. Comparer les décisions du validateur avec celles de l’implémentation C++.

Cette phase doit commencer par les entrées les plus proches de la surface d’attaque, pas par une réécriture du runtime.

### Phase 3 : intégration facultative

1. Exécuter le validateur Rust dans la CI avant les tests d’intégration.
2. Ajouter une commande de diagnostic à l’émulateur, sans bloquer l’exécution normale par défaut.
3. Enregistrer la version de Python, de Rust et des dépendances dans les rapports.
4. Vérifier les licences, les sources des crates et la reproductibilité des artefacts.
5. Mesurer le temps et la mémoire supplémentaires avant toute activation par défaut.

## Critères de décision

Python et Rust valent l’effort si le projet veut :

- exécuter ou distribuer des outils en ligne de commande ;
- tester des JAR et des formats binaires de manière reproductible ;
- réduire le temps d’analyse d’une nouvelle MIDlet ;
- mettre en place du fuzzing ou une validation défensive ;
- générer des fixtures sans maintenir de gros fichiers binaires dans Git ;
- fournir des rapports exploitables par une CI ou par d’autres développeurs.

Ils ne sont probablement pas nécessaires si l’objectif immédiat est uniquement :

- lancer les deux MIDlets existants ;
- corriger quelques natives MIDP ;
- améliorer le rendu ou l’audio en C++ ;
- porter le cœur sur RP2040 sans nouvelle surface d’outils.

## Recommandation finale

La meilleure stratégie est **Python d’abord, Rust ensuite** :

- Python pour les outils de développement, les fixtures et l’automatisation ;
- Rust pour la validation bornée et le fuzzing des formats non fiables ;
- C++17 pour tout ce qui doit rester dans l’émulateur ou sur la cible MCU.

Ces deux langages sont donc utiles comme **outillage de développement et de sécurité**, pas comme dépendances obligatoires du cœur. Le meilleur moment pour les ajouter est lorsque le projet doit réduire le temps de diagnostic ou améliorer la sécurité des entrées ; le meilleur moment pour les retirer est lorsqu’ils ne reproduisent aucun outil existant ou ne réduisent aucun risque mesurable.
