# NOTES — SD / lib officielle / loaders du site (meta-emu-sdl)

> Fichier de travail : À LIRE EN PREMIER à la prochaine session sur ce sujet.
> Dernière mise à jour : 2026-10-04 (nuit) — état = **Celeste, GBTA en jeu ;
> picomon.zip réparé ; Reuben OK ; INTPEND/NVMCTRL/SysTick corrigés ;
> Yatzy + Cats&Coins = assets manquants ; Picomon post-A documenté**.

## Objectif

Faire démarrer les jeux du site (lib officielle Gamebuino META) dans
meta-emu-sdl : `wasm/games/*.bin` (Yatzy, Reuben Quest, Cats & Coins,
GB Theft Auto) et les .zip du site (Celeste, META-Picomon/picomon).
Lapinou (homebrew sans lib) fonctionne depuis longtemps.

## ÉTAT ACTUEL (après la session « Celeste + Picomon + banc » du 2026-10-04)

- **GB Theft Auto : EN JEU** (réparé par les mêmes fixes) — menu
  « GTA DEMAKE / Choisis un profil » puis niveau jouable après A
  (`EMU_PRESS_A=1500`).
- **Celeste (celeste.zip) : EN JEU** — boot complet : logo GAMEBUINO,
  « SD INIT... OK! », création du save sans erreur, écran titre
  (logo montagne + A+B), et niveau jouable après appui sur A
  (`EMU_PRESS_A=1200`, vérifié par capture).  Le .zip contient
  `Celeste/{Celeste.bin,ICON.BMP,TITLESCREEN.BMP}` — le .bin est le
  firmware, le dossier est la carte SD (rien d'autre à charger : pas
  d'assets à la SD).
- **picomon.zip : ÉCRAN TITRE RÉPARÉ** (cause : chemins relatifs, plus
  bas).  picomon.bin + dossier : titre animé « PICO MONSTERS »
  également ; `EMU_PRESS_A` (la validation du titre) bascule sur un
  **écran noir qui streame en continu** — analyse en fin de fichier.
- **Huit causes racines corrigées dans meta_emu.c** (protocole réel,
  datasheet SAM-D21 à l'appui) :
  0. **INTPEND mal encodé** : bit4 doit être TCMPL, bit5 SUSP, bit6 TERR
     — l'ancien code mettait TCMPL en bit6, l'ISR du guest y lisait
     « TERR » et partait dans son chemin d'erreur (dérive CPU, écran
     noir).  Fix : encodage datasheet.
  0b. **NVMCTRL (0x41004000+) absent** : ADDR retenu, commande EP
     (effacement page 64 o) appliquée, INTFLAG relu avec READY=1 — les
     installeurs sondent READY avant de réactiver les interruptions.
  0c. **SysTick système (0xE000E010-1C) absent** : CSR (COUNTFLAG posé
     au wrap), RVR=19999, CVR décomptant — le micros() du core Arduino
     est lu par les moteurs son maison (GSFX de Picomon).
  1. **CHCTRLA relu à 0** : la lib lit `CHCTRLA.bit.ENABLE` avant de
     réarmer (`sendBuffer : start = !(CHCTRLA.bit.ENABLE)`) ; l'émulateur
     répondait 0 pour tout canal non-TC4 → la lib réarmait EN PLEIN VOL,
     le modèle rechargeait le descripteur de TÊTE et tronquait le bloc en
     cours (octets perdus → « buffer décalé entre ses lignes », écran
     figé).  Fix : lecture = état réel (`dmaOn ? 2 : 0`) + écriture
     CHCTRLA=2 ignorée si le canal est déjà actif (matériel : no-op).
  2. **BLOCKACT ignoré** : les descripteurs de la lib (0x0419/0x04f9)
     portent BLOCKACT=0x3 = **suspend après le bloc** (datasheet
     20.6.3.2) ; l'émulateur chaînait automatiquement → l'anneau tournait
     à vide entre les trames (dérive, octets parasites).  Fix : après le
     bloc, canal suspendu avec `dmaResumeAt = DESCADDR` ; reprise par
     **CHCTRLB.CMD=RESUME** (bits 25:24, valeur 0x2, écrite par la lib à
     chaque ligne, pc 8824) — en vol = « skip next suspend » (20.6.3.3).
     Le RESUME ré-arme aussi le cadencement des beats SPI.
  3. **CHCTRLB relu à 0** : le RESUME du guest est un RMW
     (« CHCTRLB.reg |= CMD_RESUME ») ; la lecture renvoyait 0 → l'écriture
     contenait TRIGSRC=0 et **effaçait le déclencheur du canal** (l'ISR
     écran, avec le CHID du canal SD resté en cache, tuait dmaTrig[1]) →
     l'armement RX (canal 1, TRIGSRC=SERCOM4_RX) tombait à travers les
     handlers et la lecture de secteur ne livrait jamais (poll mort sur
     les sémaphores 0x20001470/71).  Fix : CHCTRLB relu = TRIGSRC<<8.
  4. **CMD13 (SEND_STATUS) non implémenté** : après chaque CMD24 le
     SdFat du guest interroge le statut ; le modèle répondait 0x04
     (« illegal command ») → réécriture du secteur 2179 en boucle
     (« SAVE ERROR Invalid save file. »).  Fix : R2 = 00 00.  (Le fix
     précédent « réponse CMD24 en un échange » reste nécessaire.)
- **Ordre des boutons = f(speed SPI du pad) (résolu)** : le fork TS et
  la lib lisent le pad à 12 MHz → ordre **down,left,right,up,a,b,menu,
  home** (bits 0→7) ; lapinou lit à 24 MHz → ordre historique
  **left,right,up,a,b,menu,down,home**.  L'émulateur répond selon le
  registre BAUD du SERCOM4 au moment de la lecture (BAUD=1 → ordre lib,
  BAUD=0 → ordre maison) — automatique, plus besoin de variable.
  ATTENTION : le BAUD SPI-mode est au 0x4200180C (l'émulateur capturait
  le 0x0A, adresse du mode I2C — d'où « baud=0 » pour tous et l'ancienne
  impasse).  **EMU_BTN_ORDER=lapinou** force l'ordre maison.
- **Yatzy** : « SD INIT... OK! » + pile FS complète (/YATZY +
  SETTINGS.SAV créés et relus).  Le jeu streame ensuite **une ligne de
  128 px NOIRE par trame** (dump EMU_FB_FD : 40960/40960 pixels 0x0000)
  sans jamais ouvrir d'asset.  Pas de source publique : **cause la plus
  probable = assets du site absents** (notre carte n'a que le .bin) —
  **obtenir le .zip officiel de Yatzy** avant d'instrumenter plus.
  OUVERT.
- **Reuben** : scène de ville inchangée (non-régression OK).
- **GB Theft Auto : EN JEU** (menu profil + niveau après A) — réparé
  par les mêmes fixes.
- **Cats & Coins : assets absents** — le mount passe désormais
  intégralement (CMD41/CMD58/CMD17, save écrite + relue via CMD13),
  puis le loader attend ses fichiers (CatsAndCoinsDemo/…) qui ne sont
  pas distribués avec les .bin (source).  Il faut le .zip du site —
  même classe que Yatzy.
- **Picomon (picomon.zip = dossier binary de META-Picomon-master,
  source : github.com/deeph-z80/META-Picomon)** :
  - **.zip → bloqué à « SD INIT OK » (CORRIGÉ)** : le jeu ouvre ses
    assets en relatif à la RACINE de la carte ; le zip les range sous
    `/picomon/…` → re-lecture du dossier /PICOMON sans fin.  **Fix
    `zip_load_card`** : le dossier du .bin est retiré du chemin de
    TOUTES les entrées (aplatissement) → écran titre immédiat.
  - **.bin/.zip → titre animé puis NOIR après validation A (OUVERT)** :
    le source montre le flux title_screen() → fade_in (3×
    `while(!gb.update()); delay(70)`) → choose_music() (ouvre
    `musics/outside.wav`) → fill(BLACK) + intro typée (gsfx.play par
    lettre, timing micros()).  Le gel observé : le driver d'affichage
    n'est plus appelé (free_count du driver = 0, flag done = 1,
    request = 1 — l'ISR a servi mais le main loop ne re-arme plus) et
    le PC dérive en marche linéaire dans la flash.  Trois corrections
    candidates essayées sans effet sur ce point (INTPEND, NVMCTRL,
    SysTick CSR/RVR/CVR — conservées : fidèles au matériel et utiles
    ailleurs).  Prochaine piste : inverser le chemin d'installation du
    loader (CPSID à bin 0x4ffc) avec le flux du source.
- **META-Picomon-master.zip / picomon.zip** : banc d'essai FS complet
  (assets BMP/GMV/WAV, sous-dossiers) — serviront à valider la couche
  fichiers après chaque évolution SD.
- Non-régressions vérifiées : lapinou 25 couleurs (avec
  EMU_BTN_ORDER=lapinou, l'appui A agit bien sur « a »), GB loader Zelda
  4 couleurs, `ctest -R meta` 8/8, wasm + meta-emu-standalone.html
  reconstruits.

## AUDIO (2026-10-04 — lapinou : FAUX DÉPART, retour à l'identique)

Une session a tenté de corriger « le son pas dingue » de lapinou par
trois changements (cadence TC4 sur tickCount, période DMA dérivée de la
config, consommation SDL adaptative) : **le pitch est devenu 7 % grave —
REVERT COMPLET, l'audio est rétabli à l'identique** (cadence TC4 par
compteur incrementPc, 907 ticks fixes pour le chemin DMA, consommation
SDL 1:1 à 22049 Hz, décroissance /2 en sous-débit).

Ce que la session a établi (utile pour la suite) :
- **le pitch d'origine est correct** : le jeu vise le 22 kHz du META et
  la lecture 1:1 à 22049 Hz le respecte ;
- **la famine (~7 %) est inhérente à l'horloge émulée** : le mixeur PMF
  du jeu alimente le DMA à ~20480 échantillons/s sur notre CPU émulé
  (modèle TS, ~2,4× plus lent que le vrai 48 MHz) alors que la sortie
  consomme à 22049 — le déficit se manifeste en tenues/décroissances,
  pas en pitch.  Sur la vraie console (48 MHz) le mixeur tient 22050.
  Toute « correction » du déficit par le débit de lecture change le
  pitch — la seule vraie parade serait un CPU émulé plus rapide, hors
  sujet (parité TS).
- Lapinou configure TC4 en MFRQ presc=16, CC0=136 → 21904 Hz attendus.

## AUDIO (2026-10-05 — famine de lapinou : cause racine mesurée et corrigée)

La conclusion « famine inhérente » du 2026-10-04 était **fausse**.  Preuves
et correction (tout est mesuré, scripts de mesure reproduisibles) :

**1. Le mixeur n'est pas en déficit.**  Rendu hors-ligne de la chanson
(`music.h` du source lapinou, `pmf_player` compilé sur hôte, mêmes
dynamique de double tampon 2×1280) : **2,93 % de vrais zéros mixés**,
14935 runs, trou max 1730 échantillons.  WAV de l'émulateur : **2,94 %**,
~14880 runs, trou max 1730.  La signature « v=512 → 96 » que l'on prenait
pour de la famine est le **silence du morceau lui-même** (les cases lues
avant écriture et un échantillon mixé à 0 donnent la même valeur DAC).
L'ISR sert chaque tir : FIRE == DACW sur toute la trace (873985 == 873985).

**2. Le 48 MHz n'est pas la solution.**  Passer le domaine à 48 M ticks/s
(horloge réelle du SAMD21) fait tomber l'hôte à **72 % du temps réel**
(2400 frames = 56 s de mur au lieu de 40,2 ; le domaine TS tient 100 %).
Le jeu produit alors son audio à 72 % du débit de consommation : famine
pire qu'avant.  Domaine TS conservé (EMU_TICKS_HZ reste disponible,
expérimental).

**3. La vraie cause : dérive production/consommation + trim brutal.**
Production = cadence TC4 du domaine TS = 20e6/913 = **21907 éch/s** ;
consommation SDL fixe = **22049 éch/s** → déficit de 142 éch/s en
continu : le prébuffer (600) est mangé en ~4 s, puis sous-débittest
permanent — chaque case vide joue `audioHold/2` (crépitement) et le
garde-fou AQ_LATENCY jetait d'un coup des centaines d'échantillons
(clics).  C'est LA famine audible, indépendante de la charge hôte.

**4. Correction (meta_emu.c) :**
- la sortie SDL est (ré)ouverte à la **cadence TC4 réelle du jeu**,
  dérivée de sa config CTRLA/CC0 (`tc4_period_ticks`, arrondi au plus
  proche — lapinou : 913 ticks → 21907/s ; les chemins IRQ et DMA TC4
  partagent la même formule, le DMA n'est plus fixé à 907) ;
- consommation calée **250 ppm sous** la production
  (`AUDIO_CONSUME_SLIP 0.9975`) : l'anneau se réépaissit seul (~56
  éch/s) après toute perte hôte (stall, onglet wasm) — pitch +0,25 %,
  inaudible ;
- trim de latence **ramolli** : 2 échantillons par poussée au-delà du
  plafond (même rééchantillonnage ±0,25 %), reprise franche seulement
  au-delà de +8192 (stall catastrophe).

Résultat lapinou : production 21907/s, consommation 21851/s, hôte 100 %
du temps réel, contenu identique au rendu de référence.  Non-régressions
: `ctest -R meta` 8/8 ; banc couleurs inchangé (lapinou 25, celeste 6,
yatzy 4, reuben 11, theft-auto 5, cats-and-coins 4 — 900/2400/3600
frames selon le banc du 2026-10-04).  picomon.zip (le zip de ce dossier)
: 13 couleurs déterministes à 900 frames — le jeu ne configure pas TC4
avant cet instant et mes changements ne touchent pas son chemin (bench
antérieur « 16 » : refaire avec le zip d'origine du banc si besoin).
wasm + meta-emu-standalone.html reconstruits.

## BANC DE TEST COMPLET (2026-10-04, natif headless, captures /tmp/bench_*)

Méthode : `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./meta_emu …
--frames N --shot /tmp/bench_X.ppm` ; le score = couleurs distinctes +
pixels non noirs de la capture.

| Binaire / méthode | Frames | Résultat | État |
|---|---|---|---|
| lapinou.bin (seul, carte=son dossier) | 600 | 25 couleurs | ✓ référence |
| yatzy.bin + dossier wasm/games | 900 | 4 couleurs (boot+SD OK) | ⚠ menu noir, cf. analyse |
| reuben-…bin + dossier | 3600 | 11 couleurs | ✓ en jeu |
| cats-and-coins.bin + dossier | 900 | 4 couleurs | ⚠ mount+save OK, attend ses assets (zip requis) |
| gamebuino-theft-auto.bin + dossier | 2400 | 5 couleurs / 37 après A | ✓ **EN JEU** (menu profil) |
| celeste.zip (zip = carte+firmware) | 2400 | 6 couleurs | ✓ écran titre |
| celeste bin+dir | 2400 | 6 couleurs | ✓ identique au zip |
| celeste bin+dir + EMU_PRESS_A=1200 | 2400 | 11 couleurs | ✓ **EN JEU** |
| picomon.zip | 900 | 16 couleurs | ✓ titre (après fix aplatissement) |
| picomon bin+dir | 900 | 12 couleurs | ✓ titre animé |
| picomon bin SEUL (carte auto) | 900 | 12 couleurs | ✓ titre animé |
| picomon bin + .img (carte dir) | 900 | 12 couleurs | ✓ titre |
| picomon bin + EMU_PRESS_A | 1800 | 1 couleur | ✗ noir après validation (ouvert) |
| picomon bin + .img (carte zip non aplatie) | 600 | 4 couleurs | ✗ attendu (layout /picomon/) |

Lecture : tout ce qui marche par une méthode marche par les autres
(zip, dossier, .img, carte implicite) — les seuls écarts restants sont
des états de JEUX (Yatzy assets/état, Picomon post-Start, loaders
CMD8), pas des chemins de chargement.

## ANALYSES DÉTAILLÉES DES ÉTATS OUVERTS

### Yatzy — menu noir (pipeline OK, contenu vide) — RÉSOLU 2026-10-05

**Cause racine (voir « Le bug BLX » plus bas) : le décodeur `blx rm`
lisaient rm sur 3 bits — `blx ip` (r12) exécutait `blx r4`.** Avec le
fix, Yatzy affiche son menu (SOLO GAME / PLAYER GAME / SCORES) dès
900 frames (2 couleurs au banc : texte blanc sur noir, c'est l'écran
du menu).

- Séquence : boot lib (logo + « SD INIT... OK! ») → mount ✓ → création
  /YATZY + SETTINGS.SAV ✓ → relecture du save (LBA 3451..3458) ✓ →
  puis **une ligne noire 128 px par trame, indéfiniment** (le RASET
  avance de 1 à chaque trame, wrap à 160).
- Le flux RAMWR capturé (EMU_FB_FD, 40960 px) = 100 % de 0x0000.
- Aucune ouverture de fichier asset ; le PC n'est pas dans une boucle
  d'attente serrée.
- Conclusion : le jeu EST dans sa boucle de dessin mais peint du vide.
  Première hypothèse : assets du site manquants (notre carte n'a que
  les .bin des jeux) — **obtenir le .zip officiel de Yatzy** et
  re-tester avant d'instrumenter plus.

### Picomon — écran noir après validation du titre — RÉSOLU 2026-10-05

Deux causes, toutes côté émulateur :

1. **Le bug BLX** (voir plus bas) — l'appui sur A menait à un appel
   virtuel `blx ip` exécuté `blx r4` (r4=1 → PC=0 → flash effacée).
2. **TC5 non modélisé** : l'audio de la lib officielle tourne sur TC5
   (0x42003400, IRQ20 — `Sound::begin` → `tcConfigure` : COUNT16 MFRQ
   DIV1, CC0 = 48 MHz/SOUND_FREQ − 1 (44100), INTENSET.MC0 ;
   `TC5_Handler` = `Audio_Handler` qui mixe et **stream les WAV depuis
   la SD** dans l'ISR).  Sans TC5, plus aucun accès SD après l'appui et
   le jeu attend son tampon audio pour toujours.  Modélisé en miroir de
   TC4 (registres +0x400, cadence `tc5_period_ticks`, sortie SDL
   ré-ouverte à 44040 Hz = 44100 −250 ppm, trigger DMA TC5_OVF 0x1C).

Résultat : titre animé (16 couleurs au banc — la valeur de référence),
A → sauvegarde puis ÉCRAN DE JEU (« ×RINGRING× »), DAC à 44100/s.

- .bin ou .zip (réparé) : titre animé ✓ (sprites, crédits, 60 fps).
- `EMU_PRESS_A` (validation ; MENU est sans effet) → écran 100 % noir
  streame en continu, **zéro accès SD après l'appui**, boucle CPU saine.
- Le titre ne lisait déjà que quelques secteurs (pas de streaming wav) ;
  le mode jeu ne déclenche aucune lecture → ce n'est PAS un blocage de
  chargement d'assets mais un état de rendu vide du jeu (port Pico-8 :
  la « vm » de jeu démarre-t-elle ? attends-t-on l'audio ?).
- Prochaine étape : TRACE_ALL 1:1 autour de la boucle chaude
  0x177a4-0x177a8 après l'appui, et localisation du framebuffer jeu
  dans la SRAM (écritures de dessin absentes ? clear-only ?).
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

### Cats and Coins (démo) — gel après « SD INIT... OK! » — RÉSOLU 2026-10-05 (bug BLX)

**La plongée dans la flash effacée était un `blx ip` décodé `blx r4`
(le bug BLX, voir plus bas) — pas un état de jeu ni des assets
manquants.**  Avec le fix : écran titre complet (artwork « CATS and
COINS »), A → menu « PLAY MODE » (Move/Jump/Crouch/Drop down/Switch to
edit mode).  Le bin n'a jamais eu besoin d'autres fichiers.

L'analyse ci-dessous (conservée pour le journal) avait correctement
identifié le round-trip SD exact et le point de rupture, mais avait
mal interprété la destination du saut :

- Le bin du site est **identique** à notre copie (MD5
  00a9f5dca572c0a53e461346386aeab2, servi sans AUCUN asset :
  `emulator-start.js` n'appelle que `loadFromUrl(<bin>)`).
- Notre chaîne : mount ✓ → création /REC + REC.CACHE (secteur dir 3451
  avec « . »/« .. », données 3452..3458, en-tête **« GBMS »** à 3459) →
  relecture 3459 **identique octet pour octet** à l'écriture (vérifié)
  → 119 K ticks plus tard, **appel d'un pointeur de fonction nul**
  (objet 0x20000b14, champ +0x14 = 0x00000001, site d'appel 0xcc86,
  retour 0xcc89) → PC=0 → exécution de la flash effacée sous 0x4000 →
  écran boot figé pour toujours (200 hashes identiques sur 12 000
  frames).
- Le fork TS de référence (headless.js) gèle **pareil** au même état ;
  le noyau v12 du site aussi (cf. preuve par exécution plus bas).
  Sans carte : « SD INIT... FAILED! » puis le même figeage — EMU_NO_SD,
  pressions A/MENU précoces ou tardives : aucun changement.
- Conclusion : ce n'est PAS un périphérique manquant de notre côté
  (round-trip SD exact, TS identique) — le bin démo attend un état de
  jeu que ni lui ni personne ne crée hors du vrai matériel/site récent.
  Table d'API bootloader (0x3FD8..0x3FFC : version>0x10001, error,
  loader, game…) non consultée pendant le gel — en faire un stub reste
  utile pour Lapinou (Home → loader) mais ne change rien à Cats.
- État accepté : 4 couleurs au banc (logo + SD INIT OK), déterministe
  (3 runs, même hash). Réessayer si un zip officiel complet refait
  surface (le « Download » du site sert le projet source, pas le bin).

## Le bug BLX (2026-10-05 — cause racine de TOUS les « états ouverts »)

Le décodeur `BLX rm` (format T2, 0x47C0..0x47FF) lisait le registre sur
**3 bits** (`(op >> 3) & 7`) au lieu de **4** ([6:3]) : tout
`blx r8..r15` exécutait `blx r0..r7`.  L'idiome GCC des appels
virtuels — `ldr r1, [r3, #0x14] ; mov ip, r1 ; blx ip` — sautait donc
vers **r4** (souvent 1 ou une valeur sans rapport) : PC=0, puis
exécution de la flash effacée sous 0x4000, écran figé sans aucun
message.  BX haut (0x4D) et MOV haut étaient corrects ; seul BLX était
cassé.  Les jeux touchés : Cats (boot), Picomon (après A), Yatzy
(menu) — tous « bloqués » dans les analyses antérieures pour de
mauvaises raisons (assets, FS, géométrie carte).  Fix : `& 0xF`.

Enseignement : quand un jeu « se fige sans message », vérifier
d'abord PC < 0x4000 (flash effacée) — signature d'un saut folle — avant
d'incriminer les périphériques.

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
  NB : la fenêtre CHID (0x41004840-0x4100484F) et le canal 0 indexé
  partagent les adresses ; le guest peut écrire CHID via un demi-mot non
  aligné (0x4100483E) — le champ CHID en cache (`dmac_chid`) peut donc
  viser un autre canal que 0 au moment d'une écriture fenêtre.
- **INTPEND lu en ldrh** par le handler DMAC du jeu → `fetchHalf` route
  maintenant les périphériques vers `periph_read` (héritage TS : mot et
  octet seulement).  ATTENTION au bug d'implémentation : `*handled=1`
  AVANT le return dans le handler INTPEND.
- **Interruption DMAC level-triggered** : l'ISR ne service qu'un canal
  par entrée ; on ré-arme `dmacInterrupt` à chaque acquittement TCMPL
  (**TCMPL seulement — pas les SUSP**, désormais légitimes avec
  BLOCKACT=0x3, sinon tempête).
- **Collision display-DMA / SD** : les beats d'écran ne doivent
  horloger la machine SD que pour un dummy 0xFF pendant une transaction
  active, et pendant un CMD24 seuls les beats du **canal TX SD**
  (`sdTxCh`) horlogent (chaque beat porte un octet de données).
- **Acquittement CHINTFLAG par la fenêtre CHID (writeByte 0x4100484E)**
  existait et jetait tout — corrigé.
- **Bloqueurs résolus côté DMAC (session Celeste)** : lecture CHCTRLA
  (état réel), BLOCKACT=suspend + reprise CHCTRLB.CMD=RESUME avec
  ré-armement des beats, lecture CHCTRLB (TRIGSRC préservé dans les RMW),
  cf. les 4 causes racines en tête de fichier.

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

1. **Yatzy — obtenir le .zip officiel du site** (page du jeu, bouton
   « télécharger ») et re-tester : le menu streame du noir — si le zip
   (avec ses assets) l'affiche, la cause était les données manquantes ;
   sinon instrumenter le dessin (palette colorIndex, buffer d'image).
2. **Picomon post-validation** : TRACE_ALL 1:1 autour de la boucle
   chaude 0x177a4-0x177a8 après EMU_PRESS_A ; localiser le framebuffer
   du mode jeu (le dessin écrit-il ? clear-only ?) ; vérifier l'audio
   (attente d'un buffer wav ?).
3. **Loaders CMD8** (Cats & Coins, GB Theft Auto) : leur script
   n'enchaîne pas d'ACMD41 ; tracer l'interpréteur (0xa67c) avec la
   table de commandes construite dynamiquement (pas de table statique
   dans le .bin — vérifié).
4. **Input dans Reuben (et les jeux lib officielle)** : l'ordre boutons
   est désormais celui de la lib (Celeste répond à A/B) — re-tester le
   jeu au clavier et à la manette sur Reuben.
5. **Dérive résiduelle Celeste** : après ~15 s le jeu repose sa fenêtre
   LCD (cmd 0x2a à ~330M ticks, via le chemin SPI 9 bits du guest) —
   bénin mais visible (saut de 35 px) ; à rapprocher d'un re-init
   périodique du driver.  Vérifier à long terme sur d'autres jeux lib.

## Outils de débogage (dans meta_emu.c, natif)

- `SD_DEBUG=1` : toutes les commandes SD.  `SD_DEBUG=2` : échange octet
  par octet (`[sdx]`) + contenu des secteurs écrits (`[wr-done]`).
- `EMU_DESC_DEBUG=1` : chargements de descripteurs DMAC (`[desc]`),
  armements CHCTRLA (`[arm]` : canal, TRIGSRC, resume en attente) et
  écritures CHID (`[chid]`) — l'outil qui a percé le protocole DMA de la
  lib (BLOCKACT=suspend + RESUME).
- `EMU_FB_DUMP=<fichier>` (+ `EMU_FB_DUMP_START=<tick>`) : dump du flux
  RAMWR brut (pixels 565) — comparer stream vs framebuffer SRAM.
- `EMU_WIN_DEBUG=1` : commandes/fenêtres LCD (`[win]` CASET/RASET/valeurs).
- `EMU_BYTES_FROM=<tick>` (+ `EMU_BYTES_TO`) : trace octet par octet du
  panneau avec l'état D/C (`[b]`) — voit le SPI 9 bits du guest.
- `EMU_CHUNK_DEBUG=1` : premiers octets de chaque bloc d'affichage armé.
- `EMU_BTN_DEBUG=1` : lectures du registre à décalage pendant un appui
  (`[btnread]`, avec le baud SPI du moment).
- `EMU_BTN_ORDER=lapinou` : ordre boutons historique des jeux maison
  (défaut = ordre TS/lib : down,left,right,up,a,b,menu,home).
- `NVM_DEBUG=1` : écritures flash.  `FLASH_DUMP=fichier` : dump flash
  fin de run.  `FAT_DUMP=fichier` : dump de la carte à la construction.
  `FAT_DUMP_EXIT=fichier` : dump de la carte EN FIN de run (état après
  les écritures du guest — c'est lui qui montre les fichiers créés).
- `EMU_PRESS_A=<frame>` / `EMU_PRESS_B=<frame>` : appuie sur A / B
  pendant 6 frames (menus headless — « A+B » du titre Celeste = A ou B).
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
      ./meta_emu /tmp/celeste/Celeste/Celeste.bin /tmp/celeste/Celeste \
      --frames 2400 --shot /tmp/ce.ppm
    # attendu : écran titre Celeste (montagne, A+B, crédits) ;
    # avec EMU_PRESS_A=1200 : niveau 1 en jeu (Madeline à l'écran)
    make && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      ./meta_emu wasm/games/reuben-quest-lost-between-times.bin wasm/games \
      --frames 3600 --shot /tmp/r.ppm
    # attendu : scène de ville jouable (le save se crée sans SAVE ERROR)
    SD_DEBUG=1 ./meta_emu wasm/games/yatzy.bin wasm/games --frames 900
    # attendu : mount + « SD INIT... OK! » affiché (menu toujours absent,
    # open item « premier flush »)

Non-régression : lapinou 25 couleurs, GB loader Zelda 4 couleurs
(`./meta_emu wasm/zedtest/firmware.bin wasm/zedtest`, firmware PUIS
dossier), `ctest -R meta` 8/8.
