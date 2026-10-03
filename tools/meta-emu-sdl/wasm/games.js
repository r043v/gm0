/* Liste offline des jeux chargés en un clic depuis le dock de droite.
 * Ajoutez une entrée par jeu : { n: 'Nom affiché', f: 'fichier.bin' }
 * (fichier posé à côté de index.html, .bin ou .zip accepté).
 * Le standalone embarque automatiquement tous ces fichiers en base64. */
window.OFFLINE_GAMES = [
  { n: '🐰 Lapinou', f: 'lapinou.bin' },
  /* jeux du site META (bins de l'émulateur intégré, servis offline) */
  { n: '🚗 GB Theft Auto', f: 'games/gamebuino-theft-auto.bin' },
  { n: '🎲 Yatzy', f: 'games/yatzy.bin' },
  { n: '⚔ Reuben Quest', f: 'games/reuben-quest-lost-between-times.bin' },
  { n: '🐱 Cats & Coins', f: 'games/cats-and-coins.bin' },
];
