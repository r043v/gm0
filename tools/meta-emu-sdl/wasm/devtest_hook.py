#!/usr/bin/env python3
"""(Re)construit wasm/devtest/index.html : le mono-fichier de release + un
hook de test ?test= qui charge firmware.bin et les autres fichiers du
répertoire (servis à côté).  Usage : python3 wasm/devtest_hook.py"""
import pathlib

d = pathlib.Path(__file__).resolve().parent
standalone = d / 'meta-emu-standalone.html'
if not standalone.exists():
    raise SystemExit('construisez d\'abord la release : make single')

# liste figée à la génération : à l'exécution, fetch('') sur un répertoire
# contenant index.html renvoie la page elle-même, pas un listing
card = sorted(
    p.name for p in d.joinpath('devtest').iterdir()
    if p.is_file() and p.name != 'index.html'
    and p.suffix.lower() not in ('.js', '.wasm', '.html')
)
files_js = '[' + ', '.join(repr(n) for n in card) + ']'

# écrasement déterministe depuis la release propre (pas de chirurgie)
h = standalone.read_text()

hook = """<script>
window.addEventListener('load', () => setTimeout(async () => {
  if (!location.search.includes('test=')) return;
  try {
    if (!Module.calledRun && !Module._malloc) {
      await new Promise(res => { Module.onRuntimeInitialized = res; setTimeout(res, 8000); });
    }
    const mk = (path, buf) => ({ path, file: new File([buf], path.split('/').pop()) });
    const items = [];
    for (const name of __FILES__) {
      const buf = await (await fetch(encodeURIComponent(name))).arrayBuffer();
      items.push(mk(name, buf));
    }
    __show('hook ?test= : ' + items.length + ' fichier(s)');
    await loadSet(items);
  } catch (e) { __show('ERREUR test : ' + e.message); }
}, 400));
</script>
""".replace('__FILES__', files_js)

h = h.replace('</body>', hook + '</body>')
(d / 'devtest' / 'index.html').write_text(h)
print(f'wasm/devtest/index.html régénéré (hook ?test=, {len(card)} fichier(s) : {files_js})')
