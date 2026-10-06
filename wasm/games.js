/* Liste offline des jeux chargés en un clic depuis le dock de droite.
 * Ajoutez une entrée par jeu : { n: 'Nom affiché', f: 'fichier.zip' }
 * (fichier posé à côté de index.html ; un .zip = carte SD complète, son
 * premier .bin/.pop lance le jeu).
 * Le standalone embarque automatiquement tous ces fichiers en base64. */
window.OFFLINE_GAMES = [
  { n: '🐰 Lapinou', f: 'lapinou.zip' },
  { n: '⛰ Celeste', f: 'celeste.zip' },
  { n: '🚀 Galaxy Fighters', f: 'galaxyfighters.zip' },
  { n: '🐱 Cats & Coins', f: 'cats-and-coins.zip' },
];
