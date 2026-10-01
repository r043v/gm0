# meta-emu-sdl — émulateur Gamebuino META en C/SDL2 + WebAssembly

Port C du fork TypeScript (`output/gbemu/`) : interpréteur ARMv6-M Thumb,
périphériques (ports, SERCOM4/5, DMAC, SysTick, TC4+DAC), carte SD SPI
(image brute ou dossier FAT16 construit à la volée) et frontal SDL2
(fenêtre 160×128, clavier, audio 22 049 Hz).  Se compile aussi en
**WebAssembly** (même cœur, navigateur).

## Compilation

    make        # natif (SDL2 via pkg-config)
    make wasm   # navigateur (nécessite emsdk : source ~/emsdk/emsdk_env.sh)

## Usage (natif)

    ./meta_emu [firmware.bin] [carte] [--frames N] [--shot out.ppm] [--wav out.wav]

- Lancé **sans argument**, la fenêtre s'ouvre vide : **déposez** un
  **.bin** (le firmware ; la carte SD devient son répertoire), une
  **image .img** ou un **dossier** (la carte seule).
- `<carte>` = une **image .img** OU un **répertoire** (construit en FAT16 à
  la volée ; les .SAV écrits par le jeu sont réécrits dans les fichiers).
- **La carte SD est par défaut le répertoire contenant le firmware** ;
  une carte passée explicitement en ligne de commande reste prioritaire,
  et tout drop de firmware rebascule la carte sur son répertoire.
- La fenêtre est **redimensionnable** (échelle entière, pixels carrés) et
  la **barre de titre** affiche le **% de vitesse** (ticks émulés / temps
  réel, mis à jour toutes les 500 ms, borné à 100 %).  Le pacing est une
  échéance par frame sans dette : jamais plus vite que le temps réel, et
  un ralentissement (drag de fenêtre, stall) ne déclenche pas de
  rattrapage accéléré.

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./meta_emu \
      out/<Jeu>/.pio/build/meta/firmware.bin output/sd-card \
      --frames 300 --shot /tmp/shot.ppm

Touches : flèches, ZQSD/WASD, **Entrée**=Start (MENU), **Espace**=A,
**Ctrl**=B, **\***=Select (HOME), ou J=A, K=B, U=MENU, I=HOME.

**Manette** (SDL_GameController ; bascule automatique sur joystick brut) :
A=A, B=B, Start=MENU, Back/Guide=HOME, croix directionnelle et stick
gauche = directions.  Joystick sans mapping : boutons 0=A, 1=B, 2=MENU,
3=HOME, chapeau 0 = directions.  Branchement/débranchement à chaud géré.

## Usage (navigateur)

    cd wasm && python3 -m http.server 8000
    # ouvrir http://localhost:8000/

- **Déposez** un .bin (firmware), une image .img, ou un **dossier de jeu**
  (traversé récursivement) — ou cliquez pour choisir un dossier
  (`webkitdirectory`) : un .bin unique du dossier devient le firmware,
  les autres fichiers construisent la carte SD en mémoire (comme le fork
  TS).  Les .SAV écrits en jeu modifient les tampons en mémoire
  (persistance inter-sessions non exportée pour l'instant).
- L'audio WebAudio démarre au premier chargement (geste navigateur) ;
  le % de vitesse s'affiche dans le titre de l'onglet.
- Les `.GB` streamés depuis la carte fonctionnent (FAT16 virtuelle).
- `index.html?test` : charge `./test/firmware.bin` + fichiers de
  `./test/` servis à côté (hook de test local ; `wasm/test/` est
  ignoré par git — y déposer un firmware et des jeux pour essayer).

## État

Le boot est **paritairement validé contre le fork TypeScript** : mêmes
hachages d'état (registres + SRAM) tick par tick jusqu'à ~6,9 M ticks,
même splash Gamebuino au même tick (proportions de couleurs identiques),
Millis avancant exactement d'1 ms par entrée SysTick, audio TC4/DAC actif
(369 échantillons/frame), carte SD lue par le loader.

Corrections par rapport au précédent WIP (les quatre causes racines du
blocage « f1 / Millis gelé ») :
- **masque push/pop** : `0xfe00` laissait tous les POP (0xbcxx-0xbdxx)
  hors du décodeur (traités en no-op → effondrement de pile, chutes dans
  le code suivant, faux « f1 retourne non-zéro »).  Le TS utilise 0xf600 ;
- **retenue des additions** : calculée sur la somme tronquée 32 bits
  (C=0 pour toute comparaison d'égalité) au lieu de la somme 64 bits ;
- **MUL non masqué** : le TS laisse le produit en double JS (arrondi
  au-delà de 2^53) — répliqué via des ombres flottantes des registres
  (`regD`) et `fmod` exact ;
- **BL** : LR doit pointer après la paire (l'ancien code laissait
  LR = PC + off1<<12, corrompant tout retour `bx lr`) ; la paire coûte
  3 ticks comme les deux demi-mots du TS.

Écart résiduel connu : le compteur interne TC4 peut dériver de quelques
ticks vers 6,9 M ticks (état interne de l'émulateur, invisible côté
SRAM) ; sans effet observé sur l'écran ni l'audio.

## Débogage (variables d'environnement)

- `FAT_DUMP=/tmp/x.img` : écrit l'image FAT générée depuis un répertoire
  (vérifiable avec mtools).
- `EMU_TRACE=1` : échantillonne le PC tous les 0x40000 ticks.
- `TRACE_ALL=1` (+ `TRACE_FROM=<tick>`) : trace instruction par
  instruction (pas, tick, pc, inst, sp, r0-r12, lr), même format que le
  harnais TS `/tmp/ts_steptrace.js` — diff 1:1 avec la référence.
- `STATE_HASH=1` (+ `HASH_INTERVAL=<ticks>`) : hachage FNV-1a des
  registres + SRAM toutes les N ticks, avec dump des registres —
  comparable à `/tmp/ts_hash.js`.
- `SRAM_DUMP_AT=<tick>` (+ `SRAM_DUMP=<fichier>`) : dump de la SRAM à un
  tick donné (vs `/tmp/ts_sramdump.js` côté TS).
- `WATCH_ADDR=<hex>` : journalise les écritures mot vers cette adresse.
- `MILLIS_ADDR=<hex>` (défaut 0x20002c48) : adresse de la variable Millis
  affichée par frame ; `MILLIS_WATCH=<hex>` : compteur d'écritures.
- `ADC_FIXED=1` : ADC RESULT constant (comparaison de trajectoires avec
  le TS piloté au même ADC).

Le port est fidèle à la sémantique du TS (mêmes quirks : décodage
paresseux remplacé par un switch, même motif d'injection d'interruptions
avec chaînage `else if`, même hack SysTick à 20 000 ticks, gouverneur
TC4, lectures périphériques par largeur d'accès).
