# NOTES — SD / lib officielle / loaders du site (meta-emu-sdl)

> Fichier de travail : À LIRE EN PREMIER à la prochaine session sur ce sujet.
> Dernière mise à jour : 2026-10-04 (état = modèle précis par défaut, wasm inclus).

## Objectif

Faire démarrer les jeux du site (lib officielle Gamebuino META) dans
meta-emu-sdl : `wasm/games/*.bin` (Yatzy, Reuben Quest, Cats & Coins,
GB Theft Auto). Lapinou (homebrew sans lib) fonctionne depuis longtemps.

## ÉTAT ACTUEL (après toutes les corrections de cette session)

- **Yatzy / Reuben Quest** : « SD INIT... OK! » + création du fichier de
  sauvegarde (CMD24 vers FAT 2049 et répertoire 2179) — puis
  **« SAVE ERROR Invalid save file. »** : la couche FS du jeu boucle
  (réécritures identiques du secteur 2179/2049).  Le protocole SD est
  bon (mount OK) ; c'est la sémantique FS/geometry qui diverge.
- **Cats & Coins / GB Theft Auto (loaders)** : montent la carte
  (MBR+partition+FAT lues) puis gèlent après CMD8 dans leur init SD
  maison (pas d'ACMD41 derrière).  Leur écran « loading » (motif
  diagonal bleu/marron) est dessiné tel quel par le jeu — vérifié 3 fois
  (réalignement octet/ligne sans effet, pas de dérive constante, sprites
  intacts) : CE N'EST PAS un décalage du flux SPI.
- **Lapinou, GB loader Zelda** : non-régression OK (25 / 4 couleurs).
- wasm = même modèle précis que le natif (voir « modèle site » plus bas).

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

1. **Obtenir un .zip réel du site** (page de téléchargement du jeu,
   bouton « télécharger ») : il contient les assets
   (`CatsAndCoinsDemo/TITLESCREEN.BMP`…) et surtout la carte/structure
   telle que le site la sert.  Comparer la géométrie et les entrées
   (LFN présentes ?) avec notre constructeur.
2. **Yatzy save loop** : la couche FS du jeu réécrit des secteurs
   identiques → comparer notre secteur 2179 écrit avec ce que SdFat
   écrirait (entrées LFN « YATZY », « SETTINGS » présentes dans le
   flux !) — peut-être lié au (1).
3. **Loaders CMD8** : leur script n'enchaîne pas d'ACMD41 ; tracer
   l'interpréteur (0xa67c) avec la table de commandes construite
   dynamiquement (pas de table statique dans le .bin — vérifié).
4. **Skip du loader** (dernier recours, demandé par l'utilisateur mais
   « pas une vraie solution ») : nécessite l'analyse par binaire du
   point d'entrée du jeu derrière le loader.

## Outils de débogage (dans meta_emu.c, natif)

- `SD_DEBUG=1` : toutes les commandes SD.  `SD_DEBUG=2` : échange octet
  par octet (`[sdx]`) + contenu des secteurs écrits (`[wr-done]`).
- `NVM_DEBUG=1` : écritures flash.  `FLASH_DUMP=fichier` : dump flash
  fin de run.  `FAT_DUMP=fichier` : dump de la carte construite.
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
      ./meta_emu wasm/games/yatzy.bin wasm/games --frames 1800 \
      --shot /tmp/y.ppm
    # attendu : « GAMEBUINO / SD INIT... OK! » (+ SAVE ERROR, open item)
    SD_DEBUG=1 ./meta_emu wasm/games/yatzy.bin wasm/games --frames 900
    # attendu : CMD0, CMD8, CMD41, CMD58, CMD17 (0, 2048, 2179, 2049), CMD24…

Non-régression : lapinou 25 couleurs, GB loader Zelda (wasm/zedtest)
4 couleurs, Pokitto = parité baseline, `ctest -R meta` 8/8.
