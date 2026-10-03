# meta-emu-sdl — émulateur Gamebuino META **et Pokitto** en C/SDL2 + WebAssembly

Port C du fork TypeScript (`output/gbemu/`) : interpréteur ARMv6-M Thumb,
périphériques (ports, SERCOM4/5, DMAC, SysTick, TC4+DAC), carte SD SPI
(image brute ou dossier FAT16 construit à la volée) et frontal SDL2
(fenêtre 160×128, clavier, audio 22 049 Hz).  Se compile aussi en
**WebAssembly** (même cœur, navigateur).

Depuis l'ajout Pokitto, le même binaire émule aussi la **Pokitto**
(LPC11U68, Cortex-M0 — port C fidèle du PokittoEmu de felipemanga) :
périphériques LPC (SYSCON, IOCON, CT32B0/1, SysTick, SCT, SSP0/1, ADC,
RTC, USART0, GPIO 4 banques), écran 220×176 bit-bang GPIO, audio R2R 8
bits sur les ports (+ audio HLE si la signature du firmware stock est
reconnue), carte SD SPI sur SSP0, EEPROM 4 Ko persistée (`<jeu>.eeprom`),
API ROM (IAP + division).  La cible est **détectée automatiquement** au
chargement (mot 0 = SP initial dans la SRAM LPC 0x1000xxxx → Pokitto),
ou forcée par `--target meta|pokitto` (env `EMU_TARGET`).  Le conteneur
**.pop** du loader Pokitto est lu nativement (les enregistrements
métadonnées sont ignorés, le programme est flashé).

## Compilation

    make        # natif (SDL2 via pkg-config)
    make wasm   # navigateur (nécessite emsdk : source ~/emsdk/emsdk_env.sh)

## Usage (natif)

    ./meta_emu [firmware.bin] [carte] [--frames N] [--shot out.ppm] [--wav out.wav]

- Lancé **sans argument**, la fenêtre s'ouvre vide : **déposez** un
  **.bin** (le firmware ; la carte SD devient son répertoire), une
  **image .img**, un **.zip** (le contenu devient la carte SD
  complète ; son premier .bin devient le firmware) ou un **dossier**
  (la carte seule).
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

Exemple Pokitto (détection automatique ; la carte est un argument
explicite — image .img ou dossier —, jamais le répertoire du .bin) :

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./meta_emu \
      jeu.bin carte --frames 300 --shot /tmp/shot.ppm
    # .pop accepté tel quel ; --out-img out.img exporte la carte modifiée ;
    # -w/-W ignorent les écritures flash fautives (comme la référence)

Options : `--target meta|pokitto`, `--out-img <fichier>`, `-w [n]`, `-W`
en plus des options META existantes (`--wav`, `--frames`, `--shot`).

Touches META : flèches, ZQSD/WASD, **Entrée**=Start (MENU), **Espace**=A,
**Ctrl**=B, **\***=Select (HOME), ou J=A, K=B, U=MENU, I=HOME.
Touches Pokitto : **I/K/J/L** ou flèches = directions, **A**=A,
**S/B**=B, **D/C**=C, **F**=D (éclairage).  **F5** redémarre le jeu
(les deux cibles ; la carte SD et l'EEPROM Pokitto sont conservées).

**Manette** (SDL_GameController ; bascule automatique sur joystick brut) :
A=A, B=B, Start=MENU, Back/Guide=HOME, croix directionnelle et stick
gauche = directions.  Joystick sans mapping : boutons 0=A, 1=B, 2=MENU,
3=HOME, chapeau 0 = directions.  Branchement/débranchement à chaud géré.

- Carte SD à **taille dynamique** : la géométrie FAT16 (clusters 4-32 Ko,
  volume jusqu'à ~511 Mo) est calculée depuis le contenu — une
  bibliothèque complète tient sur la carte virtuelle.

## Usage (navigateur)

    cd wasm && python3 -m http.server 8000
    # ouvrir http://localhost:8000/

**Version mono fichier distribuable** :

    make single      # produit wasm/meta-emu-standalone.html (~1,1 Mo)

Un unique `.html` avec le wasm embarqué en base64 (`-sSINGLE_FILE=1`) :
à ouvrir directement (file:// compris), à envoyer tel quel — mêmes
fonctionnalités que la version servie.

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

## Audio par DMA (firmwares gbrecomp 0.4.0+)

Depuis la 0.4.0, les firmwares gbrecomp envoient le son au DAC par le
DMAC (canal 1, déclencheur `TC4_DMAC_ID_OVF`, descripteurs chaînés),
l'écran gardant le canal 0.  L'émulateur suit à part tout canal déclenché
par TC4 : un beat par débordement TC4 (907 ticks, ~22 kHz), descripteur
suivant lu en fin de bloc, canal arrêté sur un descripteur invalide ;
`CHCTRLA` relu donne l'état ENABLE.  Avant ce suivi, l'activation du
canal audio détournait le descripteur courant de l'écran : écran figé
sur une couleur unie et aucun son.  Les autres canaux gardent le
transfert immédiat hérité du TS (firmwares 0.1.0 : sortie identique).

## État Pokitto

- Boot complet validé sur le binaire de test du dépôt PokittoEmu
  (`HelloWorld`) : écran LCD bit-bang, timers (SysTick + CT32B1 à
  625 kHz), constructeurs C++ (itération `.init_array`), attentes
  `wait_us` — le texte « Pokitto » s'affiche ; la sortie d'écran est
  stable et l'audio R2R est câblé sur les écritures GPIO.
- Régressions META : **sortie d'écran bit-à-bit identique** à l'émulateur
  d'avant l'ajout (firmware de test, 400 frames, hachages identiques) ;
  deux bogues préexistants corrigés au passage (débordement de la table
  FAT sur les gros répertoires ; surlecture `FAT_SPC` vs `fatSpc`).
- `Pandemic` (PokittoLib récent, fourni en `.bin` et `.pop`) : écran
  titre complet rendu (nom, auteur, menu) — boot, init horloges 72 MHz,
  timers, constructeurs C++, IAP et API ROM (division) opérationnels.
  L'émulateur de référence C++ ne bootait pas ce fichier dans le même
  environnement.
- Quirks CPU divergents par cible (volontaire) : la META garde la
  sémantique exacte du TS (MUL non masqué via ombres flottantes,
  décalages par registre au compteur, ROR absent, CPS no-op) ; la
  Pokitto suit la sémantique ARM réelle de la référence C++ (MUL 32
  bits, ROR, shifts, CPSIE/CPSID, xPSR aux positions ARM, BLX rm avec
  lien + API ROM 0x1fff1ffx = IAP/division à 42 ticks).
- Débogage Pokitto : `EMU_PK_DEBUG=1` (compteurs d'interruptions + état
  timers/SysTick à la sortie) ; `EMU_FIXED_RTC=<s>` rend le RTC
  déterministe (les traces deviennent reproductibles).

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
