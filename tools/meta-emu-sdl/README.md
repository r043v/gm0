# meta-emu-sdl — émulateur Gamebuino META en C/SDL2 (expérimental)

Port C du fork TypeScript (`output/gbemu/`) : interpréteur ARMv6-M Thumb,
périphériques (ports, SERCOM4, DMAC, SysTick, TC4+DAC), carte SD SPI
(image brute) et frontal SDL2 (fenêtre 320×256, clavier, audio 22 049 Hz).

## Compilation

    make        # (SDL2 via pkg-config)

## Usage

    ./meta_emu <firmware.bin> [carte] [--frames N] [--shot out.ppm] [--wav out.wav]

- `<carte>` = une **image .img** OU un **répertoire** (construit en FAT16 à
  la volée ; les .SAV écrits par le jeu sont réécrits dans les fichiers).
- Par défaut, la carte est **le répertoire du .bin** chargé.
- Lancé sans argument : la fenêtre s'ouvre vide et accepte les **dépôts**
  (.bin = firmware + carte = son répertoire ; .img ou dossier = carte).

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./meta_emu \
      out/<Jeu>/.pio/build/meta/firmware.bin --frames 300 --shot /tmp/shot.ppm

Touches : flèches, J=A, K=B, U=MENU, I=HOME.
`FAT_DUMP=/tmp/x.img` : écrit l'image FAT générée depuis un répertoire
(vérifiable avec mtools).

## État (préalpha)

- Le cœur démarre (vecteurs, init Arduino) mais diverge encore du fork
  TypeScript dans les premiers millions de ticks : les jeux ne bootent pas
  encore.  Débogage restant : comparaison instruction par instruction avec
  la référence (`EMU_TRACE=1` pour tracer le PC).
- Le port est fidèle au sémantique du TS (mêmes quirk : décodage paresseux
  remplacé par un switch, même motif d'injection d'interruptions, même
  hack SysTick à 20 000 ticks).
