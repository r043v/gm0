# TODO

Travaux identifiés et non faits, par thème.  Chaque point dit ce qui est
su et ce qui reste à établir.  Les pistes mesurées puis écartées sont
gardées avec le chiffre qui a tranché.

## Performance

- **Dispatch de l'interpréteur** — écarté après mesure.  Sur lapinou (600
  images), 6,0 M sauts indirects sont mal prédits sur 287 M
  (`ex_ret_brn_ind_misp` / `ex_ret_ind_brch_instr`, soit 2,1 %).  À ~15
  cycles par faute, c'est au plus environ 2 % du temps.  Un threading
  (`goto *table[op >> 8]` répliqué) ne peut gagner que cela : le reste du
  coût (chargement de l'opcode, lecture de la table, saut) est inchangé.
- **Profil réel** — refait avec `perf record` (cycles:u, lapinou 600 images) :
  `step_batch_meta` 62 %, `periph_write` 11 %, `timers` 8 %, `advance_slow`
  6 %, `dma_beat` 5 %.  Le pic sur l'écriture de tag du cache NVM (22 % des
  échantillons de `step_batch_meta`) se reproduit : c'est un décalage
  d'échantillonnage (pas de sampling précis sur ce CPU), le même artefact que
  la première fois.  L'expérience (≤ 5 %) reste la référence ; les
  attributions par adresse ne suffisent pas.
- **Ordonnanceur des timers pendant le DMA écran** — fait.  Les beats SPI ont
  leur propre horodatage, et une voie rapide (`spiOnly`) saute TC et SysTick
  tant que leur échéance absolue (`tcDeadline`) n'est pas atteinte.  Elle est
  invalidée par les écritures qui changent TC, SysTick, le DMAC ou le canal
  SPI ; une écriture APB du DMA SPI ne la retire pas (première version :
  chaque beat la retirait via `timers_sync`, et la version était plus lente,
  +8 % d'instructions mesurés).
  Mesuré sur 1500 images : lapinou −2,3 % d'instructions, −3 à −4 % de
  cycles ; sml −0,6 % et ≈ −2 % (bruit entre tours de 2 à 4 %).  Parité bit à bit sur 7
  jeux.  Gain modeste, de l'ordre de quelques pour cent.
- **Rendu des filtres d'affichage** — fait.  Les cinq filtres sont séparables
  (classe de ligne × classe de colonne) : la palette est calculée une fois
  par pixel émulé, une seule ligne de sortie est écrite par classe de ligne
  et les autres en sont des copies.  Mesuré sur 160×128 (natif, -O3), par
  image : LCD 8× 4,9 → 0,31 ms, 3× 0,76 → 0,18 ms ; DMG 8× 1,6 → 0,28 ms ;
  pixel 8× 0,64 → 0,30 ms ; à 2× inchangé (≈ 0,1 ms), sauf LCD (+0,05 ms).
  Sortie identique bit à bit à toutes les échelles 2 à 8.
- **Échéances des timers Pokitto** — mesuré, rien à gagner.
  `pk_timers_next` rend 0 tant qu'un IR de CT est levé (reste du modèle
  d'avant le NVIC commun), mais `pk_machine_step` n'est appelé que 1 000 à
  2 600 fois par image (file_gb, GalaxyFighters, Pandemic), soit environ
  0,3 % du temps ; l'interpréteur en prend 84 %.

## Fidélité matérielle (META)

- **DMAC INTPEND** — encodage ramené à la datasheet, d'après l'en-tête CMSIS
  du SAMD21 (`dmac.h`) : TERR bit 8, TCMPL 9, SUSP 10 ; FERR, BUSY et PEND
  (13-15) ne sont pas modélisés.  Parité identique sur les 7 jeux : le
  changement n'altère aucune sortie observée.  Reste ouvert : le guest qui
  motivait l'ancien encodage (bits 4-6) n'est pas dans le corpus local ; à
  identifier avant de considérer le point clos.
- **Programmation flash** — le ET avec le contenu est implémenté : une
  écriture ne retire que des bits, seul un effacement de rangée (ER) en remet
  (`tests/unit/test_flash.c`).  Parité identique sur les 7 jeux.  Reste : un
  loader qui auto-patche réellement la flash, à tester de bout en bout
  (aucun dans le corpus local).
- **CTRLB.RWS** — inchangé : relu tel qu'écrit mais non relié au modèle du
  cache NVM (état d'attente fixé à 1).  Le brancher décalerait le minutage
  de tous les jeux pendant le boot (RWS=0 avant SystemInit) : à valider
  contre du matériel réel, ce qui n'est pas possible ici.
- **Canaux DMAC « indexés »** (0x50-0xFF) — mesuré : aucun accès à cette zone
  dans les 7 jeux sur 1500 images, donc le modèle n'est exercé par aucun jeu
  local.  Il est conservé faute de preuve pour le retirer ; la zone est
  réservée sur SAMD21.  Reste : identifier le guest qui l'a motivé, ou
  retirer le modèle.

## Outillage et tests

- **Banc de parité** — intégré dans `tests/parity/` : `run.sh`, `games.txt`,
  `refs.txt` (SHA-256 de l'écran `EMU_TRACE`, du `--wav` et du `--shot`).  Les
  jeux restent hors dépôt (`GM0_GAMES`).  Les références sont celles de
  l'état d'avant cette série : chacun des changements ci-dessus leur est bit
  à bit identique.
- **Valgrind / sanitizers** — passés.  ASan + UBSan sur les 7 jeux, 600
  images : aucune erreur.  Valgrind memcheck (fuites et origines) sur lapinou
  et Pandemic, 200 images : aucune erreur.  Les trois accès hors bornes trouvés
  à la main (CHID 12-15, IPR à 0x41F) ne sont plus exposés par ce corpus ; un
  corpus plus large (autres jeux, cartes SD mal formées) reste à passer.
- **EEPROM Pokitto en lecture seule** — fait : `--eeprom-ro` charge le
  `.eeprom` sans jamais le réécrire (voir le README).  Le banc de parité
  s'en sert.
- **CI multiplateforme** (Linux, macOS, Windows MSYS2) et releases de
  binaires — non fait : rien ne permet de vérifier macOS et Windows ici.

## Fonctionnalités

- Export des sauvegardes et de l'EEPROM entre sessions en wasm.
- Écritures flash en wasm (parité avec le natif pour les loaders).
- FAT32 pour les grosses cartes.
- Captures d'écran réelles dans le README.
