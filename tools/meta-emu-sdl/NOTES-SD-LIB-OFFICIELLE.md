# NOTES — SD / lib officielle / loaders du site (meta-emu-sdl)

> Fichier de travail : À LIRE EN PREMIER à la prochaine session sur ce sujet.
> Dernière mise à jour : 2026-10-04 (état = « SAVE ERROR Invalid save file. » CORRIGÉ).

## Objectif

Faire démarrer les jeux du site (lib officielle Gamebuino META) dans
meta-emu-sdl : `wasm/games/*.bin` (Yatzy, Reuben Quest, Cats & Coins,
GB Theft Auto). Lapinou (homebrew sans lib) fonctionne depuis longtemps.

## ÉTAT ACTUEL (après la correction « Invalid save file » du 2026-10-04)

- **« SAVE ERROR Invalid save file. » : CORRIGÉ** (voir cause racine plus
  bas).  **Reuben Quest : EN JEU** (scène de ville jouable, vérifié par
  capture 3600 frames).  **Yatzy** : SD INIT OK, crée son dossier
  `/YATZY` + `SAVE.SAV` (« YATZ », 64 blocs) + `/SETTINGS.SAV`
  (170 o, 32 blocs — cohérents avec la lib), atteint sa boucle de
  menu/titre (PC 0x26ce-0x29f6, E/S SD terminées en ~2 frames) mais
  **l'écran reste sur le logo boot** : le premier flush LCD du jeu ne
  passe pas (sujet OUVERT, pas lié au save — EMU_PRESS_A sans effet
  visible).  Cats & Coins / GB Theft Auto : inchangés (leur init SD
  maison sans ACMD41, sujet 3 plus bas).
- **Cause racine du SAVE ERROR** : `SdSpiCard::writeData` (SdFat) lit la
  réponse d'un CMD24 en **UN seul** échange après le CRC.  Le modèle SD
  (hérité de sdcard.ts) délivre l'octet « décidé » à l'échange N lors de
  l'échange N+1 : la réponse 0x05 poussée à la fin du 515e octet n'était
  lisible qu'à l'échange N+2 — le guest lisait 0xFF, jugeait l'écriture
  échouée (`error(SD_CARD_ERROR_WRITE)`) et la couche FS réécrivait le
  secteur 2179 en boucle (168 écritures identiques observées), puis
  `Save::openFile` relisait des checkbytes faux → « Invalid save file ».
  **Fix** : à la complétion CMD24, `sd_pending = 0x05` (le token conduit
  DO dès l'échange suivant) + busy [0x00,0x00,0xFF] en file.
- **Fix LFN (bonus, vérifié par le SdFat du guest)** : l'indicateur 0x40
  « dernière partie » était posé sur la DERNIÈRE entrée physique
  (ord=0x41 sur la partie 1) au lieu de la PREMIÈRE (ord=0x40|nents).
  FatFileLFN.cpp du guest refuse une chaîne sans le flag → tout open()
  par nom long aurait échoué (assets des jeux).  Corrigé dans
  `fat_write_lfn_entries`.
- **Lapinou (25 couleurs), GB loader Zelda (4 couleurs), `ctest -R meta`
  8/8** : non-régression OK.  ATTENTION invocation zedtest : firmware
  PUIS dossier — `./meta_emu wasm/zedtest/firmware.bin wasm/zedtest`
  (un seul arg dossier = carte sans firmware, écran noir).
- wasm et meta-emu-standalone.html reconstruits avec le fix (make wasm +
  make single).

## Les 3 causes racines historiques (toutes corrigées, à ne pas casser)

1. **Carte sans MBR** : la lib officielle (SdFat, partition 1) ne monte
   pas une superfloppy.  Le constructeur écrit un MBR (partition FAT16 à
   LBA 2048).  **Pokitto reste en superfloppy** (son lecteur attend le
   boot sector en LBA 0) — le décalage est choisi par cible dans
   `fat_bootstrap_for()` via `fatPartStart`.
2. **Écritures flash jetées** : les jeux auto-patchés (loaders) écrivent
   en flash.  `flash_store()` les applique (natif).  **En wasm c'est
   volontairement ignoré** (`#ifdef __EMSCRIPTEN__` — distribution).
3. **Paires 32 bits Thumb-2** : le second demi-mot de MRS (0xF3EF 0x8xxx)
   s'exécutait comme instruction 16 bits fantôme et MSR/DSB étaient
   avalés par la branche BL (code mort).  L'espace 32 bits (0xE800-0xFFFF
   hors BL) est consommé proprement ; MRS pose Rd=0.

## Les corrections « pile SD » (protocole OK au niveau mount+RW)

- **Registres DMAC indexés** : `Channel[n] = 0x41004840 + n*16`
  (CHCTRLA@0, CHCTRLB@4, CHINTENCLR@C, CHINTENSET@D, CHINTFLAG@E) — la
  lib officielle n'écrit jamais CHID.  Avant : écritures jetées.
- **INTPEND lu en ldrh** par le handler DMAC du jeu → `fetchHalf` route
  maintenant les périphériques vers `periph_read` (héritage TS : mot et
  octet seulement).  ATTENTION au bug d'implémentation : `*handled=1`
  AVANT le return dans le handler INTPEND.
- **Interruption DMAC level-triggered** : l'ISR ne service qu'un canal
  par entrée ; on ré-arme `dmacInterrupt` à chaque acquittement TCMPL
  (**TCMPL seulement — pas les SUSP périmés**, sinon tempête
  d'interruptions et écran noir général).
- **Collision display-DMA / SD** : les beats d'écran ne doivent
  horloger la machine SD que pour un dummy 0xFF pendant une transaction
  active, et pendant un CMD24 seuls les beats du **canal TX SD**
  (`sdTxCh`) horlogent (chaque beat porte un octet de données).
- **Acquittement CHINTFLAG par la fenêtre CHID (writeByte 0x4100484E)**
  existait et jetait tout — corrigé.

## L'émulateur officiel du site (VÉRIFIÉ à la source)

`https://gamebuino.com/themes/gamebuino/js/meta-emulatorv12.js` (27 Ko,
+ `emulator-start.js` = seulement keymap/boutons et loadFromUrl).
Copie locale d'analyse : /tmp/meta-emulatorv12.js (et .mod.js = version
CommonJS exécutable en node).  Modèle d'appareil EXACT :

- **écritures flash JETÉES** (writeWord : `if (e<536870912);` — no-op).
- **AUCUNE carte SD** : sercom DATA lit `data` (défaut 0x80, écrit
  0xFF par le listener boutons quand PB03 bas), INTFLAG lit toujours 7.
  Aucun module SD n'est chargé par la page (vérifié : la page démo ne
  charge que meta-emulatorv12.js + emulator-start.js).
- **DMA = copie instantanée** sur CHCTRLA==2 (fenêtre CHID 0x41004840,
  CHID@s+63) : BTCNT octets, src = adresse de FIN (`fetchByte(n+a-i)`),
  **dst constant** (pas de DSTINC !), suit DESCADDR, puis
  `dmacInterrupt()`.  CHCTRLA se lit = 2.  Pas de CHCTRLB, pas de
  TRIGSRC, pas de pacing.
- CPSID/CPSIE = no-ops ; dmac injecté en premier puis `else if`
  systick>=20000 ; **aucun TC4/audio**.
- Le décodeur traite CHAQUE adresse paire y compris les seconds
  demi-mots des paires 32 bits (MRS/DSB = no-ops MAIS le second
  demi-mot suivant se décode quand même génériquement → « instructions
  fantômes » présentes aussi chez eux !).
- SRAM 32 Ko à 0xFF, flash 256 Ko à 0xFF, reset à 16384.

**PREUVE PAR EXÉCUTION** (node, noyau officiel, même .bin, copie
CommonJS /tmp/meta-emulatorv12.mod.js) : le PC cycle dans le MÊME loader
(0x929x / 0x125xx) et l'écran reste sur le boot (échantillons nz=845
constants de 10 M à 60 M ticks).  **Le noyau v12 ne passe pas le loader
non plus.**  Le .bin servi par le site
(`/temp/emulator/3b1b95c92f3f8733641c58a2030fa9d5.bin`) est
**identique** à notre copie offline.  Conclusion : soit le site actuel
utilise un bundle plus récent pour le flux « PLAY », soit ces jeux y
restent au boot — mais NOTRE modèle précis affiche déjà plus
(SD INIT OK) que le v12.

## Mode « site » expérimental (EMU_SITE=1)

`emuSiteModel` (runtime, défaut 0) réplique le modèle v12 : flash
jettée, DMA instantané par fenêtre CHID (garde 200 blocs), pas de
machine SD (DATA = 0x80 / buttonData), pas d'injection TC4,
dmac/systick en if/else-if.  Résultat : écran noir (le loader ne
dessine qu'après une SD réussie) — conforme au noyau v12 lui-même.
GARDER mais ne pas activer par défaut ; le modèle précis est meilleur.

## Pistes pour la suite (ordre proposé)

1. **Yatzy — premier flush LCD** : le jeu atteint son menu (code de
   dessin + attente boutons en 0x26ce-0x29f6, plus aucune E/S SD après
   ~2 frames) mais l'écran reste sur le logo boot.  Chercher pourquoi le
   premier `display.update` du jeu ne sort pas (DMA display armé et
   déclenché ? attente TCMPL ?).  EMU_PRESS_A=<frame> existe pour
   piloter le menu en headless (6 frames d'appui).
2. **Obtenir un .zip réel du site** (page de téléchargement du jeu,
   bouton « télécharger ») : il contient les assets
   (`CatsAndCoinsDemo/TITLESCREEN.BMP`…) et surtout la carte/structure
   telle que le site la sert.  Comparer la géométrie et les entrées
   (LFN présentes ?) avec notre constructeur.
3. **Loaders CMD8** (Cats & Coins, GB Theft Auto) : leur script
   n'enchaîne pas d'ACMD41 ; tracer l'interpréteur (0xa67c) avec la
   table de commandes construite dynamiquement (pas de table statique
   dans le .bin — vérifié).
4. **Input dans Reuben (et les jeux lib officielle)** : vérifier que les
   boutons passent en jeu (le rendu est bon ; le chemin boutons est le
   même que lapinou mais non testé sur ces jeux).

## Outils de débogage (dans meta_emu.c, natif)

- `SD_DEBUG=1` : toutes les commandes SD.  `SD_DEBUG=2` : échange octet
  par octet (`[sdx]`) + contenu des secteurs écrits (`[wr-done]`).
- `NVM_DEBUG=1` : écritures flash.  `FLASH_DUMP=fichier` : dump flash
  fin de run.  `FAT_DUMP=fichier` : dump de la carte à la construction.
  `FAT_DUMP_EXIT=fichier` : dump de la carte EN FIN de run (état après
  les écritures du guest — c'est lui qui montre les fichiers créés).
- `EMU_PRESS_A=<frame>` : appuie sur A pendant 6 frames (menus headless).
- `EMU_SITE=1` : modèle d'appareil du site v12 (expérimental, noir sur
  les loaders).  `EMU_DMA_DEBUG=1` : beats DMA.  `EMU_TRACE=1` :
  échantillonnage PC ; `TRACE_ALL=1 TRACE_FROM=t` : trace 1:1 (format
  identique au harnais node /tmp/ts_hash.js).
- `WATCH_ADDR=adr` : écritures mot/demi-mot vers une adresse.
- Labels SD utiles : FAT = LBA 2049, racine = LBA 2179
  (partition à 2048 + rsvd 1 + 2×65 FAT), data à 2211, spc=8.

## Commandes de vérification rapides

    cd tools/meta-emu-sdl
    make && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      ./meta_emu wasm/games/reuben-quest-lost-between-times.bin wasm/games \
      --frames 3600 --shot /tmp/r.ppm
    # attendu : scène de ville jouable (le save se crée sans SAVE ERROR)
    SD_DEBUG=1 ./meta_emu wasm/games/yatzy.bin wasm/games --frames 900
    # attendu : mount, puis création /YATZY + SAVE.SAV + /SETTINGS.SAV
    # (CMD24 2179/2049/2114 + clusters data), PLUS AUCUNE boucle CMD24 2179
    # (le yatzy reste sur le logo boot — open item « premier flush LCD »)

Non-régression : lapinou 25 couleurs, GB loader Zelda 4 couleurs
(`./meta_emu wasm/zedtest/firmware.bin wasm/zedtest`, firmware PUIS
dossier), `ctest -R meta` 8/8.
