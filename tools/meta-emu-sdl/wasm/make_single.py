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
tag = '<script async src="meta_emu.js"></script>'
assert tag in h, 'balise du module introuvable dans index.html'
h = h.replace(tag, '<script>\n' + js + '\n</script>')

build = time.strftime('%d/%m %H:%M')
h1 = '<h1>Gamebuino META — émulateur (wasm)</h1>'
assert h1 in h, 'titre de l\'overlay introuvable'
h = h.replace(h1, f'<h1>Gamebuino META — émulateur (wasm) <small>build {build}</small></h1>')

out = d / 'meta-emu-standalone.html'
out.write_text(h)
print(f'{out.name} : {out.stat().st_size / 1024:.0f} Kio (build {build})')
