# gm0 — émulateur Gamebuino META **et Pokitto** (C/SDL2 + WebAssembly)

Émulateur des deux consoles en un seul binaire : interpréteur ARMv6-M
Thumb fidèle au silicium, périphériques réels (ports, SERCOM4/5, DMAC,
SysTick, TC4/TC5+DAC, NVIC), carte SD SPI (image brute ou dossier FAT16
construit à la volée), frontal SDL2 et build WebAssembly (même cœur C).
Cible détectée automatiquement au chargement (META SAMD21 / Pokitto
LPC11U6x), ou forcée par `--target`.

> Le fond technique (jeux maison, SD/lib officielle, fidélité matérielle,
> variables de débogage) : [DETAILS.md](DETAILS.md) et
> [NOTES-SD-LIB-OFFICIELLE.md](NOTES-SD-LIB-OFFICIELLE.md).

## État du projet

- **META (SAMD21)** : jouable — jeux maison sans lib standard (lapinou :
  DMA écran cadencé par le baud SPI, SD et audio maison), jeux de la lib
  officielle (Celeste en jeu, Cats & Coins, Picomon, Yatzy, Reuben Quest ;
  « SD INIT OK », sauvegardes écrites), loaders du site ;
- **Pokitto (LPC11U6x)** : boot complet (LCD bit-bang, timers, IAP, API
  ROM, EEPROM 4 Ko persistée), conteneur `.pop` lu nativement, Pandemic
  rendu, Galaxy Fighters (.pop + musique streamée depuis la carte) ;
- **firmwares convertis** (convertisseur = projet séparé) : les
  protocoles DMA/SPI 0.4.0+ (son par DMA TC4, streaming SD) sont suivis ;
- **web** : le même cœur compile en wasm — page servie en HTTP ou
  standalone mono-fichier (file://, sans réseau).

## Possibilités

- **drop** d'un `.bin` (firmware), d'une image `.img`, d'un `.zip` (=
  carte SD complète, `.pop` accepté comme firmware côté Pokitto) ou d'un
  **dossier** (carte seule) dans la fenêtre ; carte FAT16 à taille
  dynamique, `.SAV` écrits par le jeu réécrits dans les fichiers ;
- fenêtre redimensionnable (**F10** : modes d'échelle, **F11** : plein
  écran), **% de vitesse** dans le titre (non borné : >100 = machine trop
  rapide), pacing à échéance sans rattrapage, **F5** = reset ;
- manette SDL_GameController (hot-plug) + clavier (META et Pokitto) ;
- export de la carte modifiée (`--out-img`), ignore des écritures flash
  fautives (`-w/-W`), audio WAV de session (`--wav`), captures
  (`--shot`), run headless borné (`--frames`) ;
- wasm : dock pause/stop/chargement, liste offline de jeux
  (`wasm/games.js`), standalone embarquant les jeux en base64 ;
- débogage : profil cycles par adresse (`EMU_PROF` + `prof_report.py`),
  traces DMA/SPI/LCD/SD, script d'appuis (`EMU_INPUT`), dumps FAT/flash/
  framebuffer — liste complète dans DETAILS.md.

## Performances attendues

(Ryzen 7 6850H, machine émulée à 48 MHz) : **1,7 à 2,3x le temps réel**
en natif, cœur wasm (V8) **~1,25 à 1,5x** — le budget temps réel de
16,7 ms est tenu en ~8 ms/frame.  Le cœur CPU (dispatch par table de
saut, coûts en cycles Cortex-M0+ réels + états d'attente du cache NVM)
tourne 1,5 à 1,9x plus vite que le décodeur hérité du port TypeScript.

## Compilation

Natif, via CMake (SDL2 + zlib requises ; Linux, macOS, Windows MinGW) :

    cmake -B build .
    cmake --build build      # -> build/gm0

Navigateur (emsdk requis, `emcc` dans le PATH) : `make wasm`.
Version mono fichier distribuable : `make single`
(`wasm/gm0-standalone.html`, ouvrable en file://).

## Usage (natif)

    gm0 [firmware.bin] [carte] [--frames N] [--shot out.ppm] [--wav out.wav]

- sans argument, la fenêtre s'ouvre vide : **déposez** un jeu ;
- `<carte>` = image `.img` OU répertoire (FAT16 à la volée) ; pour la
  META la carte est par défaut le répertoire du firmware, pour la Pokitto
  c'est toujours un argument explicite ;
- touches META : flèches/ZQSD/WASD, **Entrée**=Start (MENU),
  **Espace**=A, **Ctrl**=B, **\***=Select (HOME), ou J=A, K=B, U=MENU,
  I=HOME ; l'ordre du registre à décalage suit la vitesse SPI du jeu
  (`EMU_BTN_ORDER=lapinou` pour forcer l'ordre des jeux maison) ;
- touches Pokitto : I/K/J/L ou flèches, **A**=A, **S/B**=B, **D/C**=C,
  **F**=éclairage — ou comme sur META (Entrée=C, Espace=A, Ctrl=B) ;
- options : `--target meta|pokitto`, `--out-img <fichier>`, `-w [n]`,
  `-W`.

Headless (tests, captures) :

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy gm0 \
      jeu.bin carte --frames 300 --shot /tmp/shot.ppm

## Usage (navigateur)

    cd wasm && python3 -m http.server 8000    # http://localhost:8000/

## Limitations connues

- **Windows** : MSYS2 MinGW-w64 requis (`dirent.h`) ; MSVC non testé ;
- **wasm** : écritures flash ignorées (les auto-patchs des loaders du
  site restent inertes) et `.SAV` non exportés entre sessions (tampons
  mémoire) ;
- cartes construites en **FAT16** (superfloppy ou MBR selon la cible) ;
- le dépôt ne contient **ni jeux ni firmwares** (contenu utilisateur) ;
- la parité tick à tick avec l'émulateur TypeScript d'origine a été
  **abandonnée volontairement** (2026-10-05) au profit du vrai matériel.

## Todo

- export des sauvegardes/EEPROM entre sessions en wasm ;
- écritures flash en wasm (parité avec le natif pour les loaders) ;
- CI multiplateforme (Linux/macOS/Windows) et releases de binaires ;
- captures d'écran réelles dans ce README ;
- FAT32 pour les grosses cartes.

## Génèse et crédits

Ce projet est **100 % vibe-coded** : aucune ligne de C tapée à la main.
L'émulateur entier (≈ 5 800 lignes de C, 66 commits) a été écrit par des
agents de code IA en une semaine — du 30 septembre au 6 octobre 2026 ;
le nombre de tokens, lui, n'est pas journalisé.  Signature dans l'historique
git : 14 commits co-signés **ZCode**, et un co-signé **Claude Opus 5.5**
(le canal audio DMA déclenché par TC4).

Trois sources de vérité ont nourri le code :

- **l'émulateur TypeScript d'Andy O'Neill** (MIT) — le premier port C en
  reproduisait la parité tick à tick :
  [aoneill01/gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator) ;
- **PokittoEmu de Felipe Manga** — le cœur LPC11U6x/Cortex-M0 en est un
  port C fidèle :
  [felipemanga/PokittoEmu](https://github.com/felipemanga/PokittoEmu) ;
- **les datasheets officielles**, pour tout le reste (cœur ARMv6-M réel,
  DMAC/SERCOM/TC/NVIC, carte SD SPI, panneau) :
  - SAM D21/DA1 — Microchip, DS40001882 :
    <https://www.microchip.com/en-us/product/ATSAMD21G18> ;
  - LPC11U6x — NXP (datasheet + user manual UM10732) :
    <https://www.nxp.com/docs/en/data-sheet/LPC11U6X.pdf> ;
  - ST7735 — Sitronix (PDF sous NDA, copies publiques courantes) ;
  - ARMv6-M Architecture Reference Manual :
    <https://developer.arm.com/documentation/ddi0419/latest>.

Deux références maison ont également servi — **en analyse logique
uniquement, aucun code n'en est repris** :

- **gm0-gb** (ex-« gm0 »), l'émulateur GB maison de l'auteur (Pokitto,
  ESPboy, META) : consulté pour la logique d'ensemble, les séquences de
  boot et les comportements attendus du matériel ;
- le **source de lapinou** sur META (pilotes écran/SD/audio écrits à la
  main, sans la lib standard) : c'est lui qui a révélé le vrai protocole
  DMA/SPI de l'écran, le cadencement TC4 et les quirks CS/pull-ups
  décrits dans DETAILS.md.

Écosystème : [gamebuino.com](https://gamebuino.com) et
[Gamebuino-META](https://github.com/Gamebuino/Gamebuino-META) côté META ;
[PokittoLib](https://github.com/pokitto/PokittoLib) côté Pokitto.
