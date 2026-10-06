# gm0 — émulateur Gamebuino META (SAMD21) **et Pokitto** (LPC11U6x), Cortex-M0+/M0 (C/SDL2 + WebAssembly)

> **English version** : [README.md](README.md)

Émulateur des deux consoles en un seul binaire : interpréteur **ARMv6-M
Thumb** fidèle au silicium (**Cortex-M0+** sur la META, **Cortex-M0**
sur la Pokitto — compté en cycles, avec les périphériques réels : ports,
SERCOM4/5, DMAC, SysTick, TC4/TC5+DAC, NVIC ; côté LPC SYSCON/CT32B/
SCT/SSP), carte SD SPI (image brute ou dossier FAT16 construit à la
volée), frontal SDL2 et build WebAssembly (même cœur C).  Cible
détectée automatiquement au chargement (META SAMD21 / Pokitto
LPC11U6x), ou forcée par `--target`.

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
  écran), **F8** = filtre Game Boy (4 nuances DMG + trame point-matrice),
  **% de vitesse** dans le titre (non borné : >100 = machine trop
  rapide), pacing à échéance sans rattrapage, **F5** = reset ;
- manette SDL_GameController (hot-plug) + clavier (META et Pokitto) ;
- export de la carte modifiée (`--out-img`), ignore des écritures flash
  fautives (`-w/-W`), audio WAV de session (`--wav`), captures
  (`--shot`), run headless borné (`--frames`) ;
- wasm : dock pause/stop/chargement, liste offline de jeux
  (`wasm/games.js`), standalone embarquant les jeux en base64 ;
- débogage : profil cycles par adresse (`EMU_PROF`),
  traces DMA/SPI/LCD/SD, script d'appuis (`EMU_INPUT`), dumps FAT/flash/
  framebuffer.

## Performance

Les chiffres absolus dépendent de l'hôte (CPU, RAM, OS, charge) — le
fait durable est la **médiane** : les deux consoles tournent à
**≈ 4× (META) et ≈ 3× (Pokitto) le temps réel** en natif sur un portable
moyen, et le build wasm tient le temps réel dans le navigateur
(≈ 55 % du natif sur META, ≈ 33 % sur Pokitto).

## Support par plateforme

| Fonction | Natif (C/SDL2) | WebAssembly |
|---|---|---|
| Écran | fenêtre redimensionnable, échelle entière/ajustée/étirée (**F10**), plein écran (**F11**) | canvas, pixels nets, mise à l'échelle navigateur |
| Son | sortie SDL2 calée sur le timer du jeu (régulation ±0,5 %) | WebAudio, démarrage au premier geste |
| Manette | SDL_GameController, branchement à chaud, mappings libellés | Gamepad API (via le port SDL d'Emscripten), mêmes mappages |
| Clavier META | flèches/ZQSD/WASD, **Entrée**=MENU, **Espace**=A, **Ctrl**=B, **\***=HOME (ou J/K/U/I) | identique |
| Clavier Pokitto | IJKL/flèches, **A/S/B/D/F** — ou Entrée=C, Espace=A, Ctrl=B | identique |
| Pause / reset | **F5** = reset (pas de pause au clavier) | boutons ⏸ (pause) et ⏹ (reset) du dock |
| Chargement d'un jeu | argument CLI ou glisser-déposer : `.bin`, `.img`, `.zip`, dossier | glisser-déposer ou 📄/📁 : `.bin`, `.pop`, `.zip`, dossier |
| Carte SD | dossier → FAT16 à la volée (taille dynamique), image `.img`, `.zip` = carte complète ; streaming `.GB` | `.zip` = carte complète ; streaming `.GB` (FAT16 en mémoire) |
| Sauvegardes | `.SAV`/`.STA` réécrits dans les fichiers ; EEPROM Pokitto `<jeu>.eeprom` persistée | en mémoire — perdues à la fermeture de l'onglet |
| Écritures flash (auto-patch des loaders) | modélisées | ignorées |
| % de vitesse | barre de titre (± % brut hors pacing) | titre de l'onglet + coin de la page |
| Jeux en un clic | — | dock de droite (`wasm/games.js`) ; standalone mono-fichier ouvrable en `file://` |
| Headless / capture | `--frames`, `--shot`, `--wav`, ligne `[bench]` de fin de run | — |
| Débogage | variables d'environnement `EMU_*`, `SD_DEBUG`… | console du navigateur |

## Compilation

Natif, via CMake (SDL2 + zlib requises ; Linux, macOS, Windows MinGW) :

    cmake -B build .
    cmake --build build      # -> build/gm0

Navigateur (emsdk requis, `emcc` dans le PATH) : `make wasm` →
`wasm/gm0-standalone.html`, un seul fichier autonome (wasm embarqué en
base64, ouvrable directement en file://).

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

Ouvrez `wasm/gm0-standalone.html` (double-clic — pas de serveur, ça
marche en file://) ou jouez directement sur
<https://r043v.github.io/gm0/> (GitHub Pages, émulateur seul — déposez
vos propres jeux).

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
L'émulateur entier (≈ 5 800 lignes de C, 66 commits) a été écrit par
**GLM-5.3-Flash**, l'agent ZCode, en une semaine — du 30 septembre au
6 octobre 2026.  Tous les commits du dépôt sont de lui, sauf un co-signé
**Claude Opus 5.5** (le canal audio DMA déclenché par TC4, travaillé dans
son propre outillage).  Les compteurs de son développement — le
convertisseur n'y a été touché que pour débuguer l'émulateur, l'usage
reflète donc l'émulateur seul —, relevés dans la base de sessions de
ZCode :

- 14 sessions, 122 messages-prompt ;
- 5 446 requêtes modèle, 5 488 appels d'outils ;
- **1,84 milliard de tokens** traités (dont 1,82 Md relus du cache,
  ~4,2 M générés), ~52 h de temps modèle cumulé.

Deux sources dont la **logique a été extraite** :

- **l'émulateur TypeScript d'Andy O'Neill** (MIT) — le premier port C en
  reproduisait la parité tick à tick :
  [aoneill01/gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator) ;
- **PokittoEmu de Felipe Manga** — le cœur LPC11U6x/Cortex-M0 en est un
  port C fidèle :
  [felipemanga/PokittoEmu](https://github.com/felipemanga/PokittoEmu).

**Les datasheets officielles** pour tout le reste (cœur ARMv6-M réel,
DMAC/SERCOM/TC/NVIC, carte SD SPI, panneau) :

- SAM D21/DA1 — Microchip, DS40001882 :
  <https://www.microchip.com/en-us/product/ATSAMD21G18> ;
- LPC11U6x — NXP (datasheet + user manual UM10732) :
  <https://www.nxp.com/docs/en/data-sheet/LPC11U6X.pdf> ;
- ST7735 — Sitronix (PDF sous NDA, copies publiques courantes) ;
- ARMv6-M Architecture Reference Manual :
  <https://developer.arm.com/documentation/ddi0419/latest>.

Et du **reverse de matériel réel** — analyse et débogage en profondeur,
aucun code repris :

- le **source de lapinou** sur META (pilotes écran/SD/audio écrits à la
  main, sans la lib standard) : c'est lui qui a révélé le vrai protocole
  DMA/SPI de l'écran, le cadencement TC4 et les quirks CS/pull-ups ;
- des **binaires META** et la **lib META officielle**
  ([Gamebuino/Gamebuino-META](https://github.com/Gamebuino/Gamebuino-META))
  : exécutés pas à pas dans l'émulateur pour la pile SD (SdFat), les
  descripteurs DMAC de la lib et l'audio TC5.

Écosystème : [gamebuino.com](https://gamebuino.com) côté META ;
[PokittoLib](https://github.com/pokitto/PokittoLib) côté Pokitto.

## Licence

gm0 est sous licence **[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/deed.fr)**
(© 2026 r043v) : attribution, pas d'utilisation commerciale, partage
dans les mêmes conditions.

La logique extraite de deux émulateurs **MIT** reste sous MIT — leurs
avis de droit sont conservés dans [LICENSE](LICENSE) :
[aoneill01/gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator)
(Andy O'Neill, 2017) et
[felipemanga/PokittoEmu](https://github.com/felipemanga/PokittoEmu)
(Felipe Manga, 2017).  Le reste n'engendre aucune obligation :
implémenter un comportement documenté dans les datasheets (Microchip,
NXP, Sitronix, ARM) ne crée pas de dérivé de ces documents, et ni le
source de lapinou ni les binaires META ni la lib officielle n'ont fourni
la moindre ligne de code (analyse seule).
