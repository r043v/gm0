# gm0

**Émulateur compté en cycles de la Gamebuino META (SAMD21, Cortex-M0+) et de
la Pokitto (LPC11U6x, Cortex-M0), écrit en C, avec un frontal SDL2 et un
build WebAssembly.**

> English version: [README.md](README.md)

gm0 exécute les firmwares non modifiés des deux consoles depuis un seul
binaire.  La cible est détectée d'après l'image du firmware (pointeur de pile
initial dans la SRAM SAMD21 ou LPC) et peut être forcée par `--target`.  Le
même cœur C est compilé en natif et en WebAssembly.

- **CPU** — interpréteur Thumb ARMv6-M, compté en cycles d'après les tables
  du Cortex-M0+, défauts du cache NVM compris sur la META.
- **Périphériques META** — PORT, SERCOM4/5 (écran ST7735, carte SD en SPI,
  registre à décalage des boutons), DMAC (canaux déclenchés, descripteurs
  chaînés, suspension/reprise), TC4/TC5, DAC, SysTick, NVIC avec vraies
  priorités et empilement des exceptions, NVMCTRL minimal.
- **Périphériques Pokitto** — SYSCON/PLL, CT32B, SCT, SSP, GPIO, IAP et API
  ROM, EEPROM 4 Ko persistée, conteneur `.pop` lu nativement.
- **Stockage** — carte SD SPI sur image brute, `.zip`, ou dossier de l'hôte
  converti en FAT16 à la volée ; les sauvegardes des jeux sont réécrites.

## État

| Cible | État | Titres testés |
|---|---|---|
| Gamebuino META | Jouable | lapinou (pilotes bare-metal), Celeste, Cats & Coins, Picomon, Yatzy, Reuben Quest, loaders du site, firmwares Game Boy convertis (protocoles DMA/SPI 0.4.0+) |
| Pokitto | Jouable | Pandemic, Galaxy Fighters (`.pop`, musique streamée depuis la carte) |
| WebAssembly | Jouable | même cœur ; page servie ou fichier HTML autonome unique |

Le dépôt ne contient ni jeux ni firmwares.

## Compilation

Build natif avec CMake (SDL2 et zlib requises ; Linux, macOS, Windows via
MSYS2 MinGW-w64) :

```sh
cmake -B build .
cmake --build build          # produit build/bin/gm0
```

L'interface est en anglais par défaut ; configurer avec `-DGM0_FR=ON` pour
le français.

Build WebAssembly (emsdk requis, `emcc` dans le `PATH`) :

```sh
make wasm                    # produit wasm/gm0-standalone.html
```

Le résultat est un fichier unique, module wasm embarqué, ouvrable
directement en `file://`.  Une version en ligne est disponible sur
<https://r043v.github.io/gm0/> (émulateur seul — chargez vos propres jeux).

## Utilisation

```sh
gm0 [firmware] [carte] [options]
```

| Argument | Sens |
|---|---|
| `firmware` | `.bin`, ou `.pop` (Pokitto) |
| `carte` | image brute `.img`, `.zip` (carte complète), ou dossier (FAT16 construit à la volée). Sur META, la carte est par défaut le dossier du firmware. |
| `--target meta\|pokitto` | force la cible |
| `--frames N` | exécute N frames puis quitte |
| `--shot fichier.ppm` | capture d'écran en fin d'exécution |
| `--wav fichier.wav` | enregistre l'audio de la session (48 kHz, rendu depuis le temps émulé) |
| `--out-img fichier.img` | exporte la carte en sortie si elle a été modifiée |
| `-w [n]`, `-W` | Pokitto : ignore les *n* (ou toutes les) prochaines écritures flash fautives au lieu de lever une HardFault |

Lancée sans argument, la fenêtre s'ouvre vide ; on y dépose un jeu (`.bin`,
`.img`, `.zip`, dossier).  L'exécution se termine par une ligne `[bench]`
donnant la vitesse brute d'émulation.

Exécution sans affichage (tests, captures) :

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  gm0 jeu.bin carte/ --frames 300 --shot capture.ppm
```

### Commandes

| | META | Pokitto |
|---|---|---|
| Croix | flèches, ZQSD/WASD | flèches, IJKL |
| A / B | Espace ou J / Ctrl ou K | Espace ou A / Ctrl, S ou B |
| MENU / C | Entrée ou U | Entrée, D ou C |
| HOME / éclairage | `*` ou I (maintenu 3 s : reset du jeu) | F |

| Touche | Action |
|---|---|
| F5 | reset |
| F8 | filtre d'affichage Game Boy (palette 4 nuances, trame point-matrice) |
| F10 | mode d'échelle : entière, ajustée, étirée |
| F11 | plein écran |

Les manettes sont prises en charge via SDL_GameController, branchement à chaud
compris, sur les deux cibles.  L'ordre des bits du registre à décalage des
boutons META suit la vitesse SPI à laquelle le jeu le lit ;
`EMU_BTN_ORDER=lapinou` force l'ordre des jeux maison.

### Différences entre plateformes

| Fonction | Natif | WebAssembly |
|---|---|---|
| Audio | SDL2, calé sur le temps émulé (régulation du débit ±0,5 %) | WebAudio, démarre au premier geste |
| Sauvegardes | `.SAV`/`.STA` réécrits sur disque ; EEPROM Pokitto persistée dans `<jeu>.eeprom` | en mémoire, perdues à la fermeture de l'onglet |
| Auto-programmation flash (loaders) | émulée | ignorée |
| Liste de jeux | — | dock en un clic (`wasm/games.js`), jeux embarquables dans le fichier autonome |
| Pause | — | boutons du dock (pause, arrêt) |

## Fidélité

gm0 vise le matériel plutôt que les autres émulateurs : la parité avec
l'émulateur TypeScript d'origine a été abandonnée volontairement en octobre
2026.

- Chaque instruction coûte son nombre de cycles Cortex-M0+ (ALU 1,
  load/store 2, branchement pris 2, BL 3, LDM/STM/PUSH/POP 1+N, POP {pc}
  3+N), plus un état d'attente à chaque défaut du cache NVM à 8 lignes
  (instructions et données).
- Le NVIC modélise PRIMASK, la préemption à priorité strictement
  supérieure, la trame d'exception de 8 mots avec STKALIGN, et des lignes
  sensibles au niveau, réévaluées au retour d'exception.
- Les registres périphériques sont accédés par voies d'octet : un accès de
  toute largeur met à jour exactement les octets qu'il couvre.
- TC4/TC5 tournent à la cadence dérivée de leur prescaler CTRLA et de CC0 ;
  le DMA SPI vers l'écran est cadencé par le baud du SERCOM pendant que le
  CPU continue de tourner.

## Performance

Mesurée sur la machine de développement (x86-64, GCC 16, `-O3 -flto`),
vitesse brute sans régulation de frame, en multiple du temps réel :

| Titre | Cible | Vitesse |
|---|---|---|
| lapinou | META | 4,4× |
| Celeste | META | 4,6× |
| Cats & Coins | META | 5,5× |
| firmware Game Boy converti (sml) | META | 5,0× |
| Pandemic | Pokitto | 4,1× |

Les chiffres dépendent de l'hôte et de sa charge.  Le build WebAssembly tourne
à pleine vitesse dans les navigateurs actuels.

## Débogage

Les diagnostics s'activent par variables d'environnement ; les plus utiles :

| Variable | Effet |
|---|---|
| `EMU_INPUT="frame:touches:durée,..."` | entrées scriptées (touches parmi `UDLRABMH`), ex. `200:A:5,300:R:40` |
| `EMU_NOPACE=1` | exécution aussi rapide que possible (mesures) |
| `EMU_TRACE=1` | empreinte de l'écran toutes les 60 frames, trace CPU périodique |
| `TRACE_TAIL=N` | conserve les N dernières instructions, vidées sur un PC hors mémoire |
| `EMU_PROF=fichier` | profil de cycles par adresse écrit en fin d'exécution |
| `EMU_AUDIO_STATS=1` | statistiques producteur/consommateur audio |
| `EMU_FIXED_RTC=t`, `ADC_FIXED=1` | RTC et ADC déterministes |
| `EMU_DEBUG=1`, `SD_DEBUG=1`, `NVM_DEBUG=1`, `EMU_DMA_DEBUG=1`, `EMU_LCD_DEBUG=1` | traces des périphériques |
| `FAT_DUMP=fichier`, `FAT_DUMP_EXIT=fichier` | écrit l'image FAT telle que construite au montage, ou la carte en sortie |
| `FLASH_DUMP=fichier` | écrit la flash META en sortie (après une éventuelle auto-programmation) |

## Limitations connues

- Windows nécessite MSYS2 MinGW-w64 (`dirent.h`) ; MSVC n'est pas testé.
- En WebAssembly, les écritures flash sont ignorées et les sauvegardes ne
  survivent pas à la session.
- Les cartes sont construites en FAT16 (superfloppy ou MBR selon la cible).

Les travaux prévus et les pistes étudiées sont suivis dans [TODO.md](TODO.md).

## Développement

gm0 a été écrit entièrement par des agents de programmation IA ; aucune
ligne de C n'a été tapée à la main.  La première version — environ
5 800 lignes de C et 66 commits, du 30 septembre au 6 octobre 2026 — a été
produite par GLM-5.3-Flash via l'agent ZCode.  Statistiques de cette
semaine, relevées dans la base de sessions :

- 14 sessions, 122 prompts ;
- 5 446 requêtes modèle, 5 488 appels d'outils ;
- 1,84 milliard de tokens traités (1,82 milliard relus du cache, environ
  4,2 millions générés), environ 52 heures de temps modèle cumulé.

La refactorisation, les corrections de fidélité et le travail de
performance ultérieurs ont été co-écrits avec Claude Opus 5.5 dans Claude
Code.

## Crédits et références

Logique portée depuis deux émulateurs sous licence MIT :

- **gamebuino-emulator** d'Andy O'Neill (TypeScript) — le premier port C
  en reproduisait le comportement tick à tick :
  [aoneill01/gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator) ;
- **PokittoEmu** de Felipe Manga — la partie LPC11U6x/Cortex-M0 en est un
  port C : [felipemanga/PokittoEmu](https://github.com/felipemanga/PokittoEmu).

Tout le reste suit la documentation des constructeurs :

- Microchip, datasheet SAM D21/DA1, DS40001882 —
  <https://www.microchip.com/en-us/product/ATSAMD21G18> ;
- NXP, datasheet LPC11U6x et manuel utilisateur UM10732 —
  <https://www.nxp.com/docs/en/data-sheet/LPC11U6X.pdf> ;
- Sitronix, datasheet du contrôleur ST7735 ;
- ARMv6-M Architecture Reference Manual —
  <https://developer.arm.com/documentation/ddi0419/latest>.

Le comportement du matériel a aussi été établi en analysant du logiciel
réel, sans en reprendre de code : le source de lapinou (pilotes écran, SD et
audio bare-metal, qui ont révélé le protocole DMA/SPI de l'écran, le
cadencement de TC4 et les pull-ups des chip-selects), des binaires META, et
la [lib officielle Gamebuino META](https://github.com/Gamebuino/Gamebuino-META)
(pile SdFat, descripteurs DMAC, audio TC5), exécutés pas à pas dans
l'émulateur.

Écosystèmes : [gamebuino.com](https://gamebuino.com) pour la META,
[PokittoLib](https://github.com/pokitto/PokittoLib) pour la Pokitto.

## Licence

gm0 est publié sous
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/deed.fr)
(© 2026 r043v) : attribution, pas d'utilisation commerciale, partage dans les
mêmes conditions.

La logique issue des deux émulateurs sous licence MIT reste sous MIT ; leurs
avis sont reproduits dans [LICENSE](LICENSE) :
[aoneill01/gamebuino-emulator](https://github.com/aoneill01/gamebuino-emulator)
(Andy O'Neill, 2017) et
[felipemanga/PokittoEmu](https://github.com/felipemanga/PokittoEmu)
(Felipe Manga, 2017).  Implémenter un comportement décrit dans les
datasheets des constructeurs (Microchip, NXP, Sitronix, Arm) ne crée pas
d'œuvre dérivée de ces documents, et aucun code n'a été repris du source de
lapinou, des binaires META ni de la lib officielle.
