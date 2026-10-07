# TODO

Travaux identifiés et non faits, par thème.  Chaque point dit ce qui est
su et ce qui reste à établir.

## Performance

- **Dispatch de l'interpréteur** — 14 % (META) à 25 % (Pokitto) des
  échantillons tombent sur le saut indirect du `switch (op >> 8)`.  Pistes :
  dispatch « threaded » (`goto *table[op >> 8]` répliqué en fin de chaque
  handler, éventuellement via `--param max-goto-duplication-insns`) ;
  garder `tickCount` et le PC en registres sur toute la boucle
  (`step_batch_*`), aujourd'hui relus en mémoire à la sortie de chaque
  handler.  À trancher d'abord avec `perf stat -e branch-misses` : si le
  saut indirect est bien prédit, le gain du threading sera faible.
- **Profil réel** — les profils de la session ont été faits par
  échantillonnage SIGPROF maison (sans `perf`) ; refaire avec
  `perf record` / `perf annotate` pour confirmer les attributions (celle
  du cache NVM était un artefact : 15 % affichés, ≤ 5 % réels).
- **Ordonnanceur des timers pendant le DMA écran** — un beat SPI tous les
  16 ticks (~8 instructions) repasse par `advance_slow`, `timers_process`
  et `timers_next_event` complets (≈ 10 % du temps sur lapinou).  Une voie
  rapide quand seul le beat SPI est dû éviterait le recalcul des autres
  échéances.

## Fidélité matérielle (META)

- **DMAC INTPEND** — encodé TCMPL bit 4, SUSP bit 5, TERR bit 6, réglé
  empiriquement sur les jeux ; la datasheet SAMD21 (§20.8.12) donne a
  priori TERR 8, TCMPL 9, SUSP 10, FERR 13, BUSY 14, PEND 15.  Vérifier sur
  `DMAC_INTPEND_*_Pos` des en-têtes CMSIS SAMD21 et sur la lib officielle
  avant de toucher : le guest qui a motivé l'encodage actuel doit
  continuer de marcher.
- **Programmation flash** — les écritures sont stockées telles quelles ; le
  matériel ne fait que passer des bits de 1 à 0 (ET avec le contenu) et
  attend un effacement de rangée (ER, désormais décodé).  À faire avec un
  loader qui auto-patche réellement la flash comme cas de test.
- **CTRLB.RWS** — relu tel qu'écrit mais non relié au modèle du cache NVM
  (état d'attente fixé à 1) ; le brancher décalerait le minutage de tous
  les jeux pendant le boot (RWS=0 avant SystemInit) : à valider contre du
  vrai matériel.
- **Canaux DMAC « indexés »** (0x50-0xFF) — modèle empirique hérité (la
  lib adresserait les canaux comme sur un SAMD51) ; sur SAMD21 cette zone
  est réservée.  Identifier précisément quel code y écrit.

## Outillage et tests

- **Banc de parité dans le dépôt** — la parité bit à bit (traces d'écran
  toutes les 60 frames, WAV, capture finale, entrées scriptées par
  `EMU_INPUT`, dossiers jetables pour ne pas toucher aux `.eeprom`) n'existe
  que dans des scripts de session.  L'intégrer (`tests/`), avec les
  références, en ne versionnant que des empreintes (les jeux restent hors
  dépôt).
- **Valgrind / sanitizers** — passer les jeux de référence sous
  `valgrind --tool=memcheck` et en `-fsanitize=address,undefined` : la
  session a trouvé trois accès hors bornes à la main (CHID 12-15, IPR à
  0x41F) ; il en reste sans doute.
- **EEPROM Pokitto en lecture seule pour les tests** — chaque exécution
  réécrit `<jeu>.eeprom` à côté du firmware (ou dans le répertoire
  courant), et la suivante en hérite : deux runs identiques divergent.
  Une option (`--no-persist` ou variable d'environnement) rendrait les
  exécutions reproductibles sans copier les fichiers dans un dossier
  jetable.
- **CI multiplateforme** (Linux, macOS, Windows MSYS2) et releases de
  binaires.

## Fonctionnalités

- Export des sauvegardes et de l'EEPROM entre sessions en wasm.
- Écritures flash en wasm (parité avec le natif pour les loaders).
- FAT32 pour les grosses cartes.
- Captures d'écran réelles dans le README.
