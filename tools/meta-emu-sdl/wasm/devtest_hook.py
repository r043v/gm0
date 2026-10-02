#!/usr/bin/env python3
"""(Re)construit wasm/devtest/index.html : le mono-fichier de release + un
hook de test ?test= qui charge firmware.bin et les autres fichiers du
répertoire (servis à côté).  Usage : python3 wasm/devtest_hook.py"""
import pathlib

d = pathlib.Path(__file__).resolve().parent
standalone = d / 'meta-emu-standalone.html'
if not standalone.exists():
    raise SystemExit('construisez d\'abord la release : make single')

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
    const fw = await (await fetch('firmware.bin')).arrayBuffer();
    items.push(mk('firmware.bin', fw));
    // les autres fichiers du répertoire deviennent la carte SD
    const listing = await (await fetch('')).text();
    for (const m of listing.matchAll(/href="([^"?/][^"]*)"/g)) {
      const name = m[1];
      if (name === 'index.html' || /\\.(html|js|wasm)$/i.test(name)) continue;
      const buf = await (await fetch(encodeURIComponent(name))).arrayBuffer();
      items.push(mk(name, buf));
    }
    await loadSet(items);
  } catch (e) { __show('ERREUR test : ' + e.message); }
}, 400));
</script>
"""
h = h.replace('</body>', hook + '</body>')
(d / 'devtest' / 'index.html').write_text(h)
print('wasm/devtest/index.html régénéré (hook ?test=)')
