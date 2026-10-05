# meta-emu-sdl — émulateur Gamebuino META **et Pokitto** en C/SDL2 + WebAssembly

Port C du fork TypeScript (`output/gbemu/`) : interpréteur ARMv6-M Thumb,
périphériques (ports, SERCOM4/5, DMAC, SysTick, TC4+DAC), carte SD SPI
(image brute ou dossier FAT16 construit à la volée) et frontal SDL2
(fenêtre 160×128, clavier, audio 22 049 Hz).  Se compile aussi en
**WebAssembly** (même cœur, navigateur).

**Jeux maison sans lib standard** (ex. lapinou : buffer demi-écran envoyé
en DMA blocs vers `SERCOM4->SPI.DATA`, pilote SD et audio PMF écrits à la
main) : pris en charge depuis la révision « compat lapinou » —

- DMAC : les canaux à destination **SERCOM4 DATA** (écran) sont
  **cadencés par le baud SPI** (registre BAUD, f = 48 MHz/(2×(b+1))) —
  le CPU continue de tourner pendant le transfert, comme sur hardware
  où le DMA écran prend ~6,8 ms par demi-frame à 24 MHz (d'où les
  40-55 fps réels).  L'interruption TCMPL part à la fin du bloc.
  Les autres canaux (memset/memcpy vers la SRAM) restent des transferts
  instantanés à l'écriture CHCTRLA, avec le descripteur honoré
  précisément : **BEATSIZE** octet/demi/mot et drapeaux **SRCINC/DSTINC**
  (adresses de fin SAMD21) ;
- CS périphériques (PA27 carte SD, PA25 boutons) **hauts au reset**
  (pull-ups réelles) : un firmware qui ne configure pas les broches ne
  voit plus son trafic SPI écran dévoré par la machine SD ;
- boutons répondant sur **PA25** en plus de PB03 (même registre à
  décalage, certains jeux pilotent ce CS en direct), avec **l'ordre
  physique du registre** tel que lu par les jeux : left, right, up, a,
  b, menu, down, home (bits 0→7, actifs bas) ;
- couleurs : un jeu dont l'init déclare MADCTL.BGR=1 (init custom, ex.
  lapinou 0xC8) reçoit l'inversion R/B du panneau à l'affichage ; la
  lib standard (jamais de BGR) reste affichée brute ;
- TC4 : `INTENSET` accepte OVF (0x01, jeux maison) autant que MC0 (0x10,
  lib standard), et `INTFLAG`/`INTENSET` sont **lisibles** (un handler
  qui teste `INTFLAG.bit.OVF && INTENSET.bit.OVF` fonctionne) ; la
  cadence des tirs est **dérivée de la config réelle du timer**
  (prescaler CTRLA × (CC0+1), générateur audio 48 MHz) — l'ancien
  gouverneur heuristique du TS accélérait indéfiniment un jeu qui sert
  chaque interruption immédiatement ;
- file de réponse SD à croissance dynamique (parité avec le tableau JS
  du TS) : une commande reçue avant que la réponse précédente soit
  drainée ne déborde plus d'un tampon de 528 octets ;
- écritures disque des secteurs carte : `fseek` avant `fwrite` (les
  .SAV/.STA multi-secteurs n'écrasent plus le début du fichier).

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

    make        # natif (SDL2 via pkg-config) — -O3 -flto par défaut

À vitesse native l'émulateur tourne à ~8 ms/frame pour un budget temps
réel de 16,7 ms (59,7 fps) ; la marge vise le wasm (~3-4x plus lent).
Profil gprof : ~84 % du temps dans le cœur CPU (step + incrementPc),
le reste dans le chemin DMA/SPI — le dispatch n'est pas refactorisé
sans garantie de parité TS.
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
Ordre du registre à décalage : il dépend de la **vitesse SPI de la
lecture du pad** — à 12 MHz (BAUD=1, jeux lib : Celeste, Reuben, GBTA...)
l'ordre est celui de la lib (down,left,right,up,a,b,menu,home), à
24 MHz (BAUD=0, ex. lapinou) l'ordre historique des jeux maison
(left,right,up,a,b,menu,down,home).  L'émulateur bascule automatiquement
d'après le registre BAUD du SERCOM4 au moment de la lecture ;
**EMU_BTN_ORDER=lapinou** force l'ordre 24 MHz si besoin.
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

**Dock de droite** (toujours visible) : 📄/📁 ouverture, ⏸ pause /
▶ reprise, ⏹ redémarrage du jeu, puis la **liste offline des jeux**
(`wasm/games.js` — une entrée `{ n: 'Nom', f: 'fichier.bin' }` par jeu,
fichier posé à côté de la page, .bin ou .zip).  Pause/stop appellent les
exports C `emu_pause_toggle`/`emu_restart` (l'état de pause vient du C).
Le **standalone embarque tous ces jeux en base64** (`make single`) — il
fonctionne en file:// sans aucun réseau ; les téléchargements
gamebuino.com étant derrière login (pas de fetch direct possible), la
liste offline est le chemin recommandé.

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

> **Suivi détaillé lib officielle / SD / loaders** :
> voir [NOTES-SD-LIB-OFFICIELLE.md](NOTES-SD-LIB-OFFICIELLE.md) — analyse
> de l'émulateur officiel du site (v12), causes racines, outils de
> débogage SD et prochaines étapes.

## État

**Jeux du site META (lib récente, cf. github.com/Gamebuino/Gamebuino-META)**
: le boot est **réparé** — écran « GAMEBUINO / SD INIT... » et écrans
loaders affichés, init SD complète (CMD0 → CMD8 → ACMD41 → CMD58 →
CMD17).  **Celeste (zip du site) : EN JEU** — boot, « SD INIT... OK! »,
save créée sans erreur, écran titre et niveau 1 jouable (appui A/B).
Quatre causes racines corrigées, toutes côté émulateur (détail dans
NOTES-SD-LIB-OFFICIELLE.md) :

- **carte SD sans table de partitions** : la carte construite depuis un
  dossier était une superfloppy (boot sector en LBA 0) ; la lib officielle
  (SdFat, partition 1) lit le secteur 0, ne trouve pas d'entrée à 0x1BE
  et boucle.  Le constructeur écrit maintenant un MBR (une partition
  FAT16 à LBA 2048) — le loader gbrecomp (meta_fat.c) gère les deux
  dispositions.  La Pokitto reste en superfloppy (son lecteur FAT attend
  le boot sector en LBA 0) : le décalage est choisi par cible au moment
  de la construction de la carte ;
- **écritures flash jetées** : les jeux auto-patchés (loaders) écrivent
  dans leur propre flash (compteur d'animation, flags d'installation) ;
  `writeWord/Half/Byte` les ignoraient → boucles infinies.  La
  programmation est modélisée par effet net (`flash_store`, NVM_DEBUG=1
  pour tracer, FLASH_DUMP=fichier pour dumper la flash en fin de run) ;
- **paires 32 bits Thumb-2 exécutées comme deux 16 bits** : MRS
  (`0xF3EF 0x8xxx`, « suis-je dans une ISR ? ») laissait son second
  demi-mot s'exécuter en STRH fantôme (écritures sauvages en flash,
  registres écrasés), et MSR/DSB étaient avalés par la branche BL
  (code mort pour DMB).  L'espace 32 bits (0xE800-0xFFFF hors BL) est
  maintenant consommé proprement, MRS pose Rd=0 (mode thread).

**Build web** : les écritures flash y sont simplement ignorées
(`__EMSCRIPTEN__`) — la distribution privilégie le démarrage partout ;
les écrans de boot sont identiques, seuls les auto-patchs des loaders
restent inertes.  Le standalone embarque les jeux de la liste offline en
base64 et, au chargement d'un jeu, peuple la carte avec les autres
(parité avec le natif : carte = répertoire du firmware).

**La pile SD de la lib récente fonctionne** (Yatzy, Reuben Quest :
« SD INIT... OK! », sauvegarde écrite via CMD24).  Les cinq pièces qui
manquaient, toutes côté émulateur :

- **registres DMAC indexés** : la lib adresse Channel[n] à
  0x41004840 + n*16 (CHCTRLA@0, CHCTRLB@4, CHINTENCLR@C, CHINTENSET@D,
  CHINTFLAG@E) sans jamais écrire CHID — ces écritures étaient
  silencieusement jetées, le canal RX ne démarrait jamais ;
- **INTPEND lu en demi-mot** : le handler DMAC du jeu retrouve le canal
  à service par `ldrh 0x41004820` — fetchHalf ne consultait pas
  periph_read (héritage TS : « mot et octet, jamais demi-mot ») et
  renvoyait 0 ;
- **interruption DMAC level-triggered** : le handler ne service qu'un
  canal par entrée ; sur circuit le NVIC ré-entre tant qu'un drapeau
  pend — l'émulateur ré-arme maintenant dmacInterrupt à chaque
  acquittement si d'autres canaux attendent ;
- **collision display-DMA / SD** : les beats DMA de l'écran
  horlogenaient la machine SD et vidaient sa file au milieu des
  échanges CPU — un beat n'horloge plus la carte que pour un dummy
  0xFF pendant une transaction active ;
- **acquittement fenêtre** : le clear CHINTFLAG par la fenêtre CHID
  (0x4100484E, octet) était jeté lui aussi.

Session Celeste (zip du site, lib officielle en mode INDEX) — le
protocole DMA réel de la lib, désormais modélisé (datasheet SAM-D21) :

- **CHCTRLA relu** : la lib lit `CHCTRLA.bit.ENABLE` avant de réarmer ;
  renvoyer 0 la faisait réarmer en plein vol → bloc tronqué, lignes
  décalées, écran figé.  Lecture = état réel ; CHCTRLA=2 sur un canal
  déjà actif = ignoré (no-op matériel) ;
- **BLOCKACT=suspend après bloc** (descripteurs 0x0419/0x04f9 de la
  lib) : le canal se suspend à chaque fin de bloc et attend
  **CHCTRLB.CMD=RESUME** — plus d'anneau libre entre les trames (fin de
  la dérive et des octets parasites) ;
- **CHCTRLB relu** : le RESUME du guest est un RMW ; la lecture à 0 lui
  faisait écrire une valeur sans TRIGSRC qui effaçait le déclencheur du
  canal (canal RX SD mort) ;
- **CMD13 (SEND_STATUS)** répondu (R2=00 00) : le SdFat interroge le
  statut après chaque écriture, « illegal command » bouclait le CMD24
  (« SAVE ERROR »).

Reste ouvert, deux couches :
- **Yatzy** : « SD INIT... OK! » s'affiche désormais mais le menu n'est
  toujours pas dessiné (open item « premier flush ») ;
- **les deux loaders** (Cats & Coins, GB Theft Auto) montent la carte
  puis leur init SD maison se fige après CMD8 (pas d'enchaînement
  ACMD41).  Leur écran de chargement (motif diagonal répété) est dessiné
  tel quel par le jeu : trois vérifications indépendantes (réalignment
  d'un octet par ligne sans effet, pas de dérive constante par ligne,
  sprites intacts) confirment que le flux SPI est fidèle à ce que le jeu
  dessine.  Leurs assets (`CatsAndCoinsDemo/`…) ne sont de toute façon
  pas distribués avec les .bin.  Le TS de référence reste pris en défaut
  sur ces jeux indépendamment (closures périmées après auto-patch
  flash) : le C est le seul des deux à pouvoir les exécuter.

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
- `TRACE_TAIL=<n>` (+ `TRACE_TAIL_OUT=<fichier>`) : garde les n
  dernières instructions en tampon circulaire et les déverse au
  **premier PC fou** (le tick du crash variant selon les runs, c'est le
  moyen fiable d'attraper la fenêtre avant le déraillement).
- `WILD_RESET=1` : sur PC hors flash, reprend sur le vecteur de reset au
  lieu d'exécuter les mauvaises herbes (comportement TS = exécuter).
  Les PC fous sont journalisés avec `pc/prev/lr/sp/r0-r3` (trois
  occurrences max, comportement TS sinon inchangé).
- `EMU_DMA_DEBUG=1` : chaque transfert DMA instantané (canal, src, dst,
  taille, beats, position LCD, hachure du contenu) — pour auditer le
  chemin d'affichage DMA d'un jeu maison.
- `EMU_DESC_DEBUG=1` : chargements de descripteurs DMAC (`[desc]`),
  armements CHCTRLA (`[arm]`) et écritures CHID (`[chid]`) — le protocole
  réel de la lib officielle (BLOCKACT=suspend après bloc, reprise par
  CHCTRLB.CMD=RESUME) se lit là.
- `EMU_FB_DUMP=<fichier>` (+ `EMU_FB_DUMP_START=<tick>`) : dump du flux
  RAMWR brut (pixels 565).  **`EMU_FB_FD=<n>`** : même dump via un
  descripteur hérité (`3>/tmp/dump.raw`) — à privilégier dans les
  environnements sableux où les fwrite vers un fichier créé par le
  processus lui-même sont avalées.  `EMU_WIN_DEBUG=1` (+ 
  `EMU_WIN_DEBUG_LIMIT=<n>`) : commandes/fenêtres LCD ;
  `EMU_BYTES_FROM=<tick>` (+ `EMU_BYTES_TO`) : octets du panneau avec
  l'état D/C ; `EMU_CHUNK_DEBUG=1` : premiers octets de chaque bloc.
- `EMU_BTN_DEBUG=1` : lectures du registre à décalage pendant un appui ;
  `EMU_BTN_ORDER=lapinou` : ordre boutons des jeux maison (défaut = ordre
  lib/TS, voir « Touches META »).
- `EMU_LCD_DEBUG=1` : trace les écritures MADCTL (ordre des composantes
  déclaré au panneau et verrou d'inversion).
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
