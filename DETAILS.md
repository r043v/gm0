# gm0 — détails et journal

Le fond technique du projet : comportements des jeux maison, implémentation
Pokitto, saga SD/lib officielle, fidélité matérielle et boîte à outils de
débogage.  L'essentiel est dans le [README](README.md).

## Jeux maison sans lib standard (ex. lapinou)

Jeux dont les pilotes sont écrits à la main (lapinou : buffer demi-écran
envoyé en DMA blocs vers `SERCOM4->SPI.DATA`, pilote SD et audio PMF écrits
à la main) : pris en charge depuis la révision « compat lapinou » —

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

## Pokitto — implémentation

Le même binaire émule aussi la **Pokitto** (LPC11U68, Cortex-M0 — port C
fidèle du PokittoEmu de felipemanga) : périphériques LPC (SYSCON, IOCON,
CT32B0/1, SysTick, SCT, SSP0/1, ADC, RTC, USART0, GPIO 4 banques), écran
220×176 bit-bang GPIO, audio R2R 8 bits sur les ports (+ audio HLE si la
signature du firmware stock est reconnue), carte SD SPI sur SSP0, EEPROM
4 Ko persistée (`<jeu>.eeprom`), API ROM (IAP + division).  La cible est
**détectée automatiquement** au chargement (mot 0 = SP initial dans la
SRAM LPC 0x1000xxxx → Pokitto), ou forcée par `--target meta|pokitto`
(env `EMU_TARGET`).  Le conteneur **.pop** du loader Pokitto est lu
nativement (les enregistrements métadonnées sont ignorés, le programme
est flashé).

## Audio par DMA (firmwares gbrecomp 0.4.0+)

Depuis la 0.4.0, les firmwares gbrecomp envoient le son au DAC par le
DMAC (canal 1, déclencheur `TC4_DMAC_ID_OVF`, descripteurs chaînés),
l'écran gardant le canal 0.  L'émulateur suit à part tout canal déclenché
par TC4 : un beat par débordement TC4 (2 177 cycles à 48 MHz, ~22 kHz), descripteur
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

## SD / lib officielle / loaders du site

> **Suivi détaillé** : voir
> [NOTES-SD-LIB-OFFICIELLE.md](NOTES-SD-LIB-OFFICIELLE.md) — analyse de
> l'émulateur officiel du site (v12), causes racines, outils de débogage
> SD et prochaines étapes.

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
  acquittement si d'autres canaux attendent, **filtré par CHINTENSET**
  (un canal dont l'interruption TCMPL n'est pas activée ne tire pas la
  ligne : le canal audio TC4 des firmwares gbrecomp, dont le flag TCMPL
  n'est jamais acquitté par le DMAC Handler, ré-armait l'interruption
  en boucle — tempête de réentrance, pile hors SRAM, « pc fou » vers
  t=7,4 M dans tout jeu converti 0.5.0).  CHINTENSET/CLR sont capturés
  sur toutes les largeurs d'accès (le mot indexé seul laissait les strb
  de la lib à la bande) ;
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

**Cats and Coins, Picomon, Yatzy : fonctionnels** (écrans titre/menus,
puis jeu — voir NOTES, « Le bug BLX »).  Deux correctifs décisifs du
2026-10-05 :

- **BLX rm** : le registre était lu sur 3 bits au lieu de 4 — tout
  `blx r8..r15` (idiome des appels virtuels GCC : `mov ip, r1 ;
  blx ip`) sautait vers r0-r7 et plongeait dans la flash effacée
  (écran figé sans message).  C'était LA cause des « jeux bloqués » ;
- **TC5 (0x42003400, IRQ20)** : l'audio de la lib officielle
  (`Sound::begin`, streaming WAV dans l'ISR) ; les jeux lib à musique
  attendaient leur tampon pour toujours.  Modélisé en miroir de TC4,
  sortie SDL calée sur sa cadence (44,1 kHz −250 ppm pour Picomon).

Historique : le port est parti d'une parité tick à tick avec le fork
TypeScript (mêmes hachages d'état jusqu'à ~6,9 M ticks).  Cette parité a
été abandonnée le 2026-10-05 au profit du vrai matériel : le TS reproduisait
des écarts (MUL en double JS, retenue des décalages sur le compteur, ROR
ignoré, flags des ADD sur registres hauts, interruptions injectées sous
CPSID, 1 instruction = 1 tick à 20 M/s…) qui cassaient des jeux réels.

## Fidélité matérielle (2026-10-05)

Principe : **le vrai SAMD21 d'abord**, plus de reproduction des écarts de
l'émulateur TS d'origine (sauf options explicites ci-dessous).

- **Temps en cycles Cortex-M0+ à 48 MHz** : 1 tick = 1 cycle, chaque
  instruction coûte ses cycles réels (modèle de `m0_estimate.py` : ALU 1,
  load/store 2, B/BX 2, BL 3, conditionnel pris 2, PUSH/POP/LDM/STM 1+N,
  POP {pc} 3+N, MRS/MSR 4) + les états d'attente des défauts du **cache
  NVM** (8 lignes de 64 bits, RWS=1).  L'ancien domaine TS (1 instruction =
  1 tick à 20 M/s, CPU ~1,5x trop lent face aux timers) a été retiré.
- **NVIC** : PRIMASK respecté (CPSID/CPSIE/MSR), priorités IPR/SHPR3,
  ISER/ICER/ISPR/ICPR, ICSR (PENDSTSET, VECTACTIVE), pas de préemption à
  priorité égale (plus de réentrance d'un handler sur lui-même),
  EXC_RETURN imbriqué, trame alignée 8 octets, xPSR réel ; MRS/MSR réels
  (IPSR, PRIMASK, MSP).  Causes corrigées : le freeze du mode SMOOTH
  (DMAC_Handler injecté dans la section critique de la file LCD).
- **Cœur ARMv6-M réécrit** (dispatch par table de saut, coûts en cycles
  intégrés, accès SRAM/flash directs) : MULS 32 bits exacts, décalages
  par registre et ROR réels, ADD/MOV/CMP sur registres hauts (sans flags,
  `add pc`, `mov pc`), REVSH, LDMIA sans écriture de base si Rn est
  chargé, BL complet (S:I1:I2), MRS/MSR réels.  1,5 à 1,9x plus rapide
  que le décodeur hérité.
- **DMAC** : IRQ levée seulement si CHINTENSET l'active ; SWRST remet le
  canal à zéro (flags, enable, FERR, TRIGSRC) ; CHSTATUS.FERR ; fin de
  chaîne DESCADDR=0 sans SUSP.  Causes corrigées : écran GB aux lignes
  dupliquées, son haché des firmwares 0.5.0 (relance du canal audio à
  chaque frame).
- **SPI (SERCOM4)** : durée d'octet exacte (16 x (BAUD+1) cycles) pour le
  DMA comme pour le CPU ; INTFLAG DRE/TXC/RXC selon le temps émulé ; tampon
  de réception à 2 niveaux (les octets laissés par l'écran DMA y restent,
  vidé par SWRST, ENABLE=0 ou RXEN=0 — l'Arduino SPI.config() en fait un à
  chaque changement de vitesse).  Les attentes RXC des firmwares coûtent
  donc leur vrai temps.  `EMU_SPI_INSTANT=1` rend le DMA écran quasi
  instantané (débogage, non fidèle).
- **Carte SD** : CMD18 (lecture multi-blocs) + CMD12 — sans eux le WAV de
  Picomon rejouait en boucle un tampon figé (claquements sans fin).
- **ST7735** : COLMOD 12 bpp (RGB444, 2 pixels / 3 octets), 16 et 18 bpp.
- **Audio hôte** : file dimensionnée sur le buffer SDL obtenu et
  régulation dynamique du débit (±0,5 %, interpolation) — plus de trous
  quand le callback SDL vide 1024 échantillons d'un coup ; DAC 10 bits
  mis à l'échelle sans écrêtage.
- **Noms longs FAT** : caractères 12-13 au bon endroit, cluster LFN à 0.
- **Carte FAT** : BPB étendu (signature 0x29, type « FAT16   » à l'offset
  54) — la PetitFatFs de PokittoLib n'accepte un volume qu'avec cette
  chaîne ; en superfloppy (Pokitto), plus d'entrée MBR écrite dans le
  secteur de boot.
- **Zips Pokitto** : un `.pop` (conteneur du loader) dans le zip est pris
  comme firmware (un `.bin` reste prioritaire), le reste du zip forme la
  carte — ex. `GalaxyFighters_POPandMusic.zip` (musique `music/*.raw`
  streamée depuis la carte).  Touches Pokitto : A, B, C = Entrée/U.

Vitesse (Ryzen 7 6850H, 48 MHz émulés) : natif 1,7 à 2,3x le temps réel,
cœur wasm (V8) ~1,25 à 1,5x.  Le wasm appelle `step` par pointeur : V8 ne
promeut sous TurboFan que les fonctions souvent appelées (pas d'OSR).

## Débogage (variables d'environnement)

- `EMU_NOPACE=1` : pas de cadencement temps réel (mesure de vitesse brute).
- `EMU_PROF=<fichier>` : profil en cycles par adresse (flash et SRAM) écrit
  en fin de run ; `./prof_report.py <fichier> <firmware.elf> [N]` agrège par
  fonction.  Estimation fidèle au modèle de l'émulateur, pas une mesure.
- `EMU_AUDIO_STATS=1` : bilan audio par seconde émulée (échantillons DAC,
  famine du canal DMA, relances du canal, sous-débits et file côté hôte).
- `EMU_INPUT="frame:touches:durée,..."` : script d'appuis (touches parmi
  U D L R A B M H), ex. `300:H:45,380:D:3,400:A:3` (menu d'options).

- `FAT_DUMP=/tmp/x.img` : écrit l'image FAT générée depuis un répertoire
  (vérifiable avec mtools).
- `EMU_TRACE=1` : échantillonne le PC tous les 0x40000 ticks.
- `TRACE_TAIL=<n>` (+ `TRACE_TAIL_OUT=<fichier>`) : garde les n
  dernières instructions en tampon circulaire et les déverse au
  **premier PC fou** (le tick du crash variant selon les runs, c'est le
  moyen fiable d'attraper la fenêtre avant le déraillement).  Un PC hors
  flash/SRAM est journalisé (`pc/prev/lr/sp/r0-r3`) puis la machine
  reprend sur le vecteur de reset.
- `EMU_SPI_INSTANT=1` : DMA SPI de l'écran quasi instantané (non fidèle).
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
  lib/TS, voir « Touches META » du README).
- `EMU_LCD_DEBUG=1` : trace les écritures MADCTL (ordre des composantes
  déclaré au panneau et verrou d'inversion).
- `ADC_FIXED=1` : ADC RESULT constant (trajectoires reproductibles).
