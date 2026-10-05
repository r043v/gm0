# Handoff — « les jeux GB ne fonctionnent plus » (runtime meta 0.5.0)

> Document de passation rédigé le 2026-10-05, à donner tel quel à un agent
> (Claude ou autre) qui reprendrait le dossier.  Tout est reproductible depuis
> `/home/m/dev/gb-recompiled`, branche `meta-target`.

## 1. Le signalement

- Les ROMs GB converties avec le pipeline `compile.sh` (runtime **gbrecomp
  0.5.0**, firmware bare metal) ne fonctionnent plus dans `meta-emu-sdl`.
  Exemple : `roms/meta/Golf/` (GOLF.BIN 245 232 o, converti le 05/10 12:44),
  zippé en `roms/meta/Golf.zip`.
- L'utilisateur précise que Golf.bin **doit tourner sans carte SD** (les
  .BMP/.STA/.SAV du dossier sont des habillages du loader, pas des
  dépendances du jeu).
- Deux pistes envisagées au départ : le convertisseur ou l'émulateur.

## 2. Symptôme exact

```
[pc fou] tick=7740124 pc=fffffffe lr=0 sp=1ffe9214   (3 occurrences max)
```

- Écran réduit à la couleur de fond GB (pas un seul pixel du jeu).
- Crash vers t ≈ 7,4 M ticks (~150 ms de jeu), **au même tick pour tous les
  jeux 0.5.0** (Tetris reconverti comme Golf) → bug du chemin boot runtime,
  pas du code jeu.
- Les 3 occurrences de `[pc fou]` sont espacées de 20 002 ticks = la période
  du SysTick : après le déraillement, chaque tick ramène un PC fou.
- `sp` sous 0x20000000 (ex. `1fffe5a0`) : **la pile est sortie de la SRAM**
  (32 Ko à 0x20000000-0x20007fff).  Les écritures hors SRAM sont jetées par
  l'émulateur, les lectures renvoient 0 → les dépilations de retour
  d'exception récupèrent des ordures (`pc=fffffffe` = `0xffffffff & ~1`,
  le lr de reset posé par `boot_vectors()`).

## 3. Enquête (méthode, reproductible)

### 3.1 A/B convertisseur vs émulateur — l'étape décisive

1. Un firmware **ancien** (convertis le 30/09, runtime 0.4.x) tourne
   parfaitement sur l'émulateur **actuel** :
   `./meta_emu output/meta-sd/tetris/tetris.bin --frames 400 --shot /tmp/x.ppm`
   → écran titre Nintendo.
2. La **même ROM reconvertie** avec le pipeline actuel crashe au même tick :
   ```
   mkdir /tmp/ab_roms && cp "roms/gb/Tetris (World) (Rev A).gb" /tmp/ab_roms/
   NO_ICONS=1 ./compile.sh /tmp/ab_roms /tmp/ab_meta_out
   ./tools/meta-emu-sdl/meta_emu /tmp/ab_meta_out/Tetris/TETRIS.BIN ...
   ```
   → `[pc fou]` dès 7,38 M ticks, écran uni.
3. Conclusion : convertisseur sain, **l'interaction nouveau runtime / émulateur
   est en cause**.  (Le convertisseur n'a d'ailleurs pas de commit depuis le
   29/09 ; tout est passé côté meta-emu-sdl, sauf la réécriture bare metal
   b1c8d22 du 03/10 21:03.)

### 3.2 Symboliser le déraillement

- `TRACE_TAIL=300 TRACE_TAIL_OUT=/tmp/tail.txt` (env de meta_emu) : garde les
  300 dernières instructions avant le premier PC fou.
- La trace montre une boucle en 0x2d3d0-0x2e9b4 qui **réentre sans fin par
  réentrance d'exception** (sp descend de ~44 o par itération : 32 o de trame
  d'exception + `push {r4,r5,lr}` du handler), puis un `bx lr` avec
  `lr=0xfffffff9` (valeur **EXC_RETURN ARM**) qui atterrit sur pc=0xfffffff8.
- Symbolisation : le firmware de compile.sh est construit dans
  `out/<Jeu>/` — **OUT_ROOT = `out/` relatif au CWD du lancement de
  compile.sh** (piège : `/tmp/ab_meta_out` ne contient que le dossier du jeu,
  pas le projet).  L'ELF de débogage survit dans
  `out/Tetris/.pio/build/meta/firmware.elf`.
- Désassemblage (`arm-none-eabi-objdump` du toolchain pio
  `~/.platformio/packages/toolchain-gccarmnoneeabi/bin/` ; **addr2line ne
  marche pas sur ces ELF** — « 'a.out': No such file », utiliser objdump) :
  - `0x2d3d0` = **DMAC_Handler** (prologue : `movs r2,#63 ; movs r1,#0 ;
    strb …` = le parcours d'acquittement des canaux),
  - `0x2e9ac` = **SysTick_Handler** (une instruction `Millis++`, `bx lr`).
- Le DMAC_Handler était donc **réentré en tempête** ; le premier retour
  d'exception à se dépiler (celui du SysTick) a lu une trame située sous
  0x20000000 → PC fou.

### 3.3 Trouver la tempête

- `EMU_DESC_DEBUG=1` : `/tmp/desc.log` montre le protocole DMAC normal
  (`[chid]`/`[arm]`/`[desc]`, blocs écran de 1280 octets vers SERCOM4 DATA
  0x42001828 toutes les ~9 000 ticks), puis :
  - t=7045136 : sélection du **canal 1** (audio TC4) — le son démarre,
  - t=7372080 : **flood de `[chid] chid=0` toutes les 9 ticks** = le
    DMAC_Handler réentré en boucle.  Fin de tous les `[desc]` écran (le CPU
    est prisonnier).
- `FLASH_DUMP=/tmp/flash.bin` a servi à éliminer une fausse piste (exécution
  dans la zone boot 0x0000-0x4000) : cette zone reste du 0xff pur (l'émulateur
  flashe le firmware en 0x4000, pas de bootloader modélisé, vecteurs lus en
  0x4000 par `boot_vectors()`).

## 4. Cause racine (chaîne complète)

1. Le runtime 0.5.0 (`targets/meta/platform/meta/meta_audio.cpp`) envoie
   l'audio au DAC par le **canal DMAC 1 déclenché par TC4 OVF**
   (`channel_start()` : CHCTRLB TRIGSRC=TC4, TRIGACT=BEAT).  Il ne fait
   **jamais** de CHINTENSET : sur un vrai SAMD21, ce canal ne lève donc
   **aucune** interruption DMAC (sa fin de bloc est consommée par le TC4) ;
   le flag TCMPL du canal 1 reste posé pour toujours.
2. L'émulateur, lui, armait `dmacInterrupt = 1` à chaque fin de bloc
   (`dma_beat`, `dma_sercom4_rx_beat`) **sans regarder CHINTENSET**.
3. Le ré-armement *level-triggered* ajouté le 05/10 (commit 5222325, pour la
   lib officielle SdFat dont l'ISR ne service qu'un canal par entrée)
   ré-armait `dmacInterrupt` à chaque acquittement CHINTFLAG **si n'importe
   quel autre canal a son flag 0x02 (TCMPL) posé** — sans vérifier que
   l'interruption de ce canal est activée.
4. → dès que l'audio démarre (premier bloc canal 1 terminé), chaque
   acquittement du canal écran ré-arme l'interruption : **tempête de
   réentrance du DMAC_Handler**, la pile descend de 44 octets par niveau
   jusqu'à sortir de la SRAM, et le premier dépilement d'EXC_RETURN lit une
   trame perdue → `pc fou`, jeu mort.  Les vieux firmwares 0.4.x passaient
   car le ré-armement inconditionnel n'existait pas encore (un seul
   `dmacInterrupt` par bloc, consommé, pas de boucle).

### 4.1 Second défaut découvert au passage

La capture de `CHINTENSET`/`CHINTENCLR` n'existait que pour les **écritures
mot** de la zone indexée (`0x41004850+`, bloc dans `writeWord` seulement).
Tous les `strb` (registres 8 bits — ceux de la lib officielle **et** du
runtime gbrecomp, qui passe par la fenêtre CHID `0x4100484D`) étaient jetés
silencieusement : `dmacIntEn[]` restait à 0.  Un premier patch qui filtrait
le ré-armement par `dmacIntEn` **sans** corriger la capture bloquait les jeux
lib à « SD INIT... » (A/B vérifié : Picomon 16 couleurs avant / 3 après).
D'où la règle : **filtrer ET capturer, sur toutes les largeurs.**

## 5. Le correctif (tools/meta-emu-sdl/meta_emu.c)

Trois aides statiques posées avant `writeWord` :

- `dmac_rearm_from(done_ch)` : le ré-armement level-triggered, filtré —
  un autre canal ne ré-arme la ligne NVIC que si `(dmacIntFlag[k] & 0x02) &&
  (dmacIntEn[k] & 0x02)` (levé **et** activé, fidèle au SAMD21).
- `dmac_indexed_intwrite(ch, off, v, w)` : CHINTENCLR (off 0xC,
  `intEn &= ~v`), CHINTENSET (off 0xD, `intEn |= v` — sémantique SET/CLR
  matérielle, qui remplace les anciens `= ~v` / `= v` accidentellement
  corrects), CHINTFLAG (off 0xE, acquittement + ré-armement).
- `dmac_window_intwrite(a, v, w)` : pareil par la fenêtre CHID
  (`0x4100484C` mot = CLR octet bas | SET octet haut ; `0x4100484C`/`0x4100484D`
  octet).

Branchements :

- `writeWord` : la fenêtre (mot 0x4100484C) + les offsets 0xC-0xE indexés
  passent par les aides ; le bloc écran/CHCTRLA inchangé.
- `writeHalf` : zone indexée + fenêtre (après le garde `emuSiteModel > 0`).
- `writeByte` : **nouveau** — zone indexée (CHCTRLA 8 bits inclus, off 0, et
  0xC-0xE) + fenêtre 0x4100484C/4D, après le garde site.
- Les trois sites d'acquittement CHINTFLAG (mot fenêtre, octet fenêtre,
  mot indexé) appellent `dmac_rearm_from`.

Choix délibéré (patch **minimal**) : les **armements** de `dmacInterrupt`
en fin de bloc (`dma_beat`, `dma_sercom4_rx_beat`, chemin instantané,
`spiDmaCh`) restent inconditionnels — comportement historique des firmwares
0.4.x préservé, aucun dépendance à une capture parfaite de CHINTENSET.  Seul
le ré-armement (la boucle de tempête) est filtré.  Une fidélité matérielle
complète gaterait aussi ces armements sur `dmacIntEn[ch] & 0x02` — à
envisager seulement avec la validation des 8 jeux de la section 7.

## 6. Validation (tout vert avec le binaire final)

| Jeu | Commande (env : `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`) | Résultat |
|---|---|---|
| Golf 0.5.0 (bin) | `./meta_emu roms/meta/Golf/GOLF.BIN --frames 2000 --shot …` | écran titre « GOLF ©1989 Nintendo », 0 pc fou |
| Golf.zip (l'utilisateur) | idem sur `roms/meta/Golf.zip` | carte zip 6 fichiers + firmware, titre |
| Tetris reconverti 0.5.0 | `/tmp/ab_meta_out/Tetris/TETRIS.BIN` | écran titre |
| tetris.bin 0.4.x ancien | `output/meta-sd/tetris/tetris.bin` | inchangé (non-régression) |
| Picomon (lib, zip) | `picomon.zip --frames 1800` | titre « PICO MONSTERS » |
| Celeste (lib, zip) | `celeste.zip --frames 1800` | titre CELESTE |
| Yatzy (lib) | `wasm/games/yatzy.bin --frames 1800` | menu SOLO/2-3-4 PLAYER |
| Cats & Coins (lib) | `wasm/games/cats-and-coins.bin` | menu démo |
| lapinou (maison) | `lapinou.bin` | en jeu (25 couleurs) |

Plus : audio Golf via `--wav` (213 637 trames à 22 049 Hz, dynamique
±21 630 — signal réel, pas de DC ni de bruit collé) ; `make wasm` compile
(emsdk : `source ~/emsdk/emsdk_env.sh`).

## 7. Pièges et faits utiles pour la suite

- **Branche `meta-target`** : ne tracke que 18 fichiers (meta-emu-sdl,
  compile.sh, NOTES).  Le convertisseur (`recompiler/`, `targets/meta/`)
  n'est pas dans l'index **mais son historique existe** (`git show
  b1c8d22:targets/meta/platform/meta/meta_audio.cpp` fonctionne ; les
  commits « META : … » du 03/10 listent les vrais chemins).
- `compile.sh` : projets dans `out/<Jeu>/` **relatif au CWD** ; ELF conservé
  dans `out/<Jeu>/.pio/build/meta/firmware.elf` ; le firmware copié dans le
  dossier du jeu (TETRIS.BIN, ~240 Ko, code généré trié par chaleur) est un
  **lien différent** du `firmware.bin` pio d'un projet régénéré à la main
  (72 Ko sans couverture) — ne jamais symboliser l'un avec l'autre.
- La validation compile.sh crée `<Jeu>.STA` (slots d'états) et `.SAV`
  éventuels à côté du .bin — normaux, pas des déchets.
- L'émulateur flash le firmware en 0x4000 ; 0x0000-0x4000 = 0xff (pas de
  bootloader) ; vecteurs + SP lus en 0x4000 ; SysTick modélisé toutes les
  20 000 ticks ; retour d'exception = boucle `while (pc == 0xfffffff8)` qui
  dépile xPSR,pc,lr,r12,r3-r0 (même ordre que `irq_inject`).
- Débogage DMAC : `EMU_DESC_DEBUG=1` ([chid]/[arm]/[desc]), `EMU_DMA_DEBUG=1`,
  `TRACE_TAIL=<n>` + `TRACE_TAIL_OUT=`, `FLASH_DUMP=`.  `addr2line` est
  cassé sur ces ELF, utiliser `objdump -d --start-address=`.
- `FLASH_PHYS_BASE` 0x00400000 : alias physique de la flash consulté par
  `fetch*`.

## 8. Restes ouverts (pistes, non traités ici)

1. **Fidélité DMAC complète** : gater aussi les armements de fin de bloc sur
   `dmacIntEn[ch] & 0x02` (et SUSP sur 0x04) — à ne faire qu'avec la
   validation complète (les 8 jeux + capture audio identique).
2. **INTPEND** : le bit PEND matériel = flag & enable ; l'émulateur renvoie
   les flags bruts (nécessaire au polling du runtime gbrecomp).  Si un jour
   un guest s'y perd, distinguer PEND des bits de flag.
3. **Reboot sur PC fou META** : `regs[15] = vectorBase + fetchWord(vectorBase + 4)`
   additionne vectorBase **et** l'adresse absolue du handler (double ajout,
   atterrit dans la zone banques).  Vraisemblablement hérité du TS, sans
   effet observé, mais suspect.
4. **Reconversion du lot** : tous les jeux de `roms/meta/*/` convertis en
   0.5.0 (le lot « golf » du 05/10) doivent maintenant tourner — à relancer
   systématiquement (`for d in roms/meta/*/; do ./tools/meta-emu-sdl/meta_emu
   "$d"/*.BIN --frames 400 --shot …; done`) après tout changement DMAC/TC4.
5. Le standalone wasm (`make single`) n'a pas été régénéré après le fix.

## 9. Autres changements commités le même jour (travail parallèle, non
détaillés ici)

- **Zip sans aplatissement** : `zip_load_card` ne retire plus le préfixe de
  dossier du .bin (les assets gardent `PICOMON/MUSICS/...`, comme un dossier
  posé à la racine de la carte) + var d'audit `EMU_DUMP_CARD=<fichier>`.
- **Échéance de frame re-basée** : `emu_nextFrameTick = tickCount +
  frame_ticks()` au lieu de `+=` — l'ancienne forme franchissait 2^32 une
  frame avant tickCount et figeait l'émulateur ~12 825 frames (214 s de
  jeu).  Vérifié : 14 000 frames de Golf, 214,4 s d'audio continu, 0 pc fou.

Les validations de la section 6 ont été faites avec l'ensemble des trois
changements présents.


## 10. Suite (même jour) — graphismes GB corrompus, point 8.1 traité

Symptôme : jeux GB 0.5.0 qui tournent mais écran faux (Tetris : « ©1989
Nintendo » en double, restes de l'écran légal ; lignes de tiles dupliquées).
`EMU_WIN_DEBUG=1` : après ~3 trames de boot, plus aucun RASET pendant des
dizaines de millions de ticks — les bandes n'étaient presque jamais envoyées.

Cause : les armements de fin de bloc (laissés inconditionnels en §5) levaient
l'IRQ DMAC pour le canal audio TC4 (canal 1, aucun CHINTENSET).  Le
`DMAC_Handler` du runtime (`meta_lcd.cpp`) ne sert que le canal écran et
appelle `lcdq_on_complete()` sans vérifier : chaque fin de bloc audio passait
pour une fin de bande écran → `lcdq_done`/`lcdq_active` désynchronisés, bandes
perdues, et le saut de lignes inchangées (META_LINE_SKIP) gardait des lignes
périmées à l'écran.

Correctif (`meta_emu.c`) : `dmac_raise(ch, bits)` — le flag se pose toujours,
la ligne NVIC ne monte que si `dmacIntEn[ch] & bits` (TCMPL 0x02, SUSP 0x04).
Appliqué dans `dma_load` (SUSP), `dma_beat`, `dma_sercom4_rx_beat`, le chemin
instantané (qui pose maintenant aussi TCMPL dans `dmacIntFlag`) ; l'armement en
double de la fin de bloc `spiDmaCh` est retiré (déjà levé par `dma_beat`).
Le chemin « modèle du site » (`emuSiteModel > 0`) n'est pas touché.

Validation (1 500-1 800 trames, 0 pc fou) : Tetris, Super Mario Land, Golf
(bin et zip), Alleyway, Balloon Kid, Dr. Mario, Gargoyle's Quest, Pac-Man,
Tennis — écrans corrects ; Picomon, Celeste, Yatzy, Cats & Coins, lapinou,
tetris.bin 0.4.x inchangés ; audio Golf `--wav` OK (±21 961).  Le standalone
wasm n'est toujours pas régénéré.

## 11. Son, freeze SMOOTH, fidélité (même jour, suite)

Règle donnée par l'utilisateur : **vrai matériel en priorité**, ne plus
reproduire les écarts de l'émulateur TS.  Détail des mécanismes dans le
README (« Fidélité matérielle »).  Corrigé :

- son haché des firmwares 0.5.0 : SWRST ne remettait pas le canal DMAC à
  zéro, le SUSP d'un seul sous-débit restait collé → `channel_running()`
  faux à chaque frame, 60 relances/s ;
- trous côté hôte : file plafonnée à 900 sous un callback SDL de 1024
  (~15 % du son en relâches) → file calée sur le buffer obtenu +
  régulation dynamique ±0,5 % ;
- Picomon (claquements/saturation après la fin d'une musique) : CMD18/CMD12
  absents → `file.read()` échouait après le rembobinage, le canal WAV
  rejouait son tampon de 2047 octets ; DAC ×96 écrêtait → ×64 ;
- freeze du mode SMOOTH (~13 s) : IRQ injectées sous CPSID → DMAC_Handler
  au milieu de `lcdq_submit_locked`, complétion perdue, `claim_strip`
  bloqué.  NVIC fidèle (PRIMASK, priorités, pas d'auto-préemption) ;
- MULS faux (double JS du TS) : les signatures de lignes ×0x9E3779B1 du
  runtime perdaient leurs bits bas ;
- temps : cycles M0+ à 48 MHz + cache NVM au lieu de « 1 instruction =
  1 tick à 20 M » (lapinou perdait ~1 % de ses échantillons TC4).

Constats laissés au runtime (pas des bugs de l'ému) :
- Mario Land en démo/jeu : ~2 % des frames dépassent le budget
  (jusqu'à >21 ms) et la file DMA audio du runtime (LATENCY 256 éch. =
  11,6 ms, RING 768) se vide → quelques relances par seconde.  Piste :
  LATENCY/RING plus grands dans `meta_audio.cpp`.
- Gargoyle's Quest (firmware 0.4.x du 02/10, SPI écran 12 MHz) : trame
  pleine 27 ms > frame → famine audio fidèle au matériel ; reconvertir.
- builds streaming : TBLEND retombe en TILE (modes de fusion retirés,
  ~3 Ko de RAM libre).
- ST7735 : COLMOD 03h (12 bpp RGB444) = 25 % d'octets SPI en moins par
  trame (13,7 → 10,2 ms à 24 MHz) ; l'ému le gère, le runtime non.

Outils ajoutés : `EMU_NOPACE`, `EMU_AUDIO_STATS`, `EMU_INPUT`,
`EMU_SPI_INSTANT`.  wasm + standalone régénérés.

## 12. Cœur réécrit, nettoyage, runtime (même jour, suite)

- Cœur ARMv6-M neuf (dispatch `switch (op >> 8)`, cycles intégrés, accès
  SRAM/flash directs) : 1,5-1,9x plus rapide (natif 260-420 %, wasm
  160-220 %) avec le timing fidèle 48 MHz ; corrige les écarts TS restants
  (voir README).  Piège rencontré : `pk_interrupt` empilait PC+2 — avec le
  retour d'exception unifié, chaque IRQ Pokitto sautait une instruction
  (Alleyway Pokitto en écran rouge) ; corrigé.
- Retirés : modèle « site » (EMU_SITE), domaine TS (EMU_TS_TIMING,
  EMU_TICKS_HZ), MUL TS (EMU_TS_MUL, regD), STATE_HASH/ihash/TRACE_ALL,
  montres mémoire, `emu.c.txt`.  Le harnais Node garde `state_hash` final.
- Runtime : `META_TBLEND` (TBLEND aussi en build streaming : 672 o, table
  de 16 couleurs) ; `META_LCD_12BPP` (RGB444, `meta_lcd_pack.h`, défaut 0) :
  sur Mario Land, relances audio 82 -> 36 mais +3,6 % de CPU moyen.
