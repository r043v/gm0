#!/usr/bin/env python3
"""Fusionne index.html + meta_emu_single.js (wasm embarqué en base64,
build -sSINGLE_FILE=1) en un unique fichier HTML autonome :
meta-emu-standalone.html — distribuable tel quel, ouvrable en file://.

Le titre affiche l'heure du build : un standalone périmé (renvoyé par
mail, extrait d'un vieux zip, onglet resté ouvert) se reconnaît d'un
coup d'œil au lieu de ressembler à une version cassée."""
import pathlib
import time

d = pathlib.Path(__file__).resolve().parent
js = (d / 'meta_emu_single.js').read_text()
# le JS inliné ne doit pas fermer le conteneur <script> de l'hôte
js = js.replace('</script', '<\\/script')

h = (d / 'index.html').read_text()

# jeux offline du manifeste games.js : embarqués en base64 (file:// sans réseau)
import base64, re
games_tag = '<script src="games.js"></script>'
assert games_tag in h, 'balise games.js introuvable dans index.html'
gjs = (d / 'games.js').read_text()
embed = {}
for m in re.finditer(r"f:\s*'([^']+)'", gjs):
    f = d / m.group(1)
    if f.exists():
        embed[m.group(1)] = base64.b64encode(f.read_bytes()).decode()
inline = 'window.OFFLINE_EMBED = {' + ', '.join(
    f'"{k}": "{v}"' for k, v in embed.items()) + '};\n' + gjs
inline = inline.replace('</script', '<\\/script')
h = h.replace(games_tag, '<script>\n' + inline + '\n</script>')
manquants = [m.group(1) for m in re.finditer(r"f:\s*'([^']+)'", gjs)
             if m.group(1) not in embed]
if manquants:
    print('attention : fichiers absents, non embarqués :', ', '.join(manquants))

tag = '<script async src="meta_emu.js"></script>'
assert tag in h, 'balise du module introuvable dans index.html'
h = h.replace(tag, '<script>\n' + js + '\n</script>')

build = time.strftime('%d/%m %H:%M')
h1 = '<h1>Gamebuino META / Pokitto — émulateur (wasm)</h1>'
assert h1 in h, 'titre de l\'overlay introuvable'
h = h.replace(h1, f'<h1>Gamebuino META / Pokitto — émulateur (wasm) <small>build {build}</small></h1>')

out = d / 'meta-emu-standalone.html'
out.write_text(h)
print(f'{out.name} : {out.stat().st_size / 1024:.0f} Kio (build {build})')
