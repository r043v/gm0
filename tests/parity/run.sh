#!/usr/bin/env bash
# Parité bit à bit de gm0 : pour chaque jeu de games.txt, trois empreintes
# (écran par EMU_TRACE, WAV et capture finale sans EMU_TRACE — les deux
# chemins d'exécution sont différents), comparées à refs.txt.
#
#   tests/parity/run.sh [-u] <binaire gm0>
#     sans option : compare ; code de sortie 1 au moindre écart ou échec
#     -u          : réécrit refs.txt à partir de ce binaire
#
# Variables : GM0_GAMES (dossier des jeux, défaut wasm/), GM0_PARITY_FRAMES
# (défaut 1500), TMPDIR (dossiers jetables).
#
# Chaque jeu tourne seul dans un dossier jetable : l'EEPROM part d'un état
# vierge et --eeprom-ro garantit qu'aucun fichier n'est réécrit.  Une sortie
# vide (SDL sans pilote audio, par exemple) fait échouer le test au lieu de
# passer pour une égalité.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
games_dir=${GM0_GAMES:-$root/wasm}
frames=${GM0_PARITY_FRAMES:-1500}
refs=$here/refs.txt
list=$here/games.txt

update=0
if [ "${1:-}" = "-u" ]; then update=1; shift; fi
if [ $# -ne 1 ]; then echo "usage: $0 [-u] <binaire gm0>" >&2; exit 2; fi
bin=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
[ -x "$bin" ] || { echo "binaire introuvable : $1" >&2; exit 2; }

if command -v sha256sum >/dev/null 2>&1; then sha() { sha256sum | cut -d' ' -f1; }
else sha() { shasum -a 256 | cut -d' ' -f1; }; fi

work=$(mktemp -d "${TMPDIR:-/tmp}/gm0-parity.XXXXXX")
trap 'rm -rf "$work"' EXIT

# run_game <nom> <fichier> <script> : écrit trace/wav/shot dans $work/<nom>.*
# et renvoie 0 si les trois sorties sont non vides et gm0 a réussi.
run_game() {
    local name=$1 file=$2 input=$3
    local dir=$work/$name
    mkdir -p "$dir" || return 1
    cp "$games_dir/$file" "$dir/" || return 1
    local env_common="SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy EMU_NOPACE=1 EMU_FIXED_RTC=1700000000 EMU_INPUT=$input"

    # 1. écran : EMU_TRACE passe par l'instance de débogage (step_t)
    ( cd "$dir" && env $env_common EMU_TRACE=1 "$bin" "$file" --frames "$frames" --eeprom-ro \
        >/dev/null 2>"$dir/trace.log" ) || return 1
    grep '^\[frame ' "$dir/trace.log" > "$dir/trace.txt"

    # 2. WAV et capture : chemin rapide, sans EMU_TRACE
    ( cd "$dir" && env $env_common "$bin" "$file" --frames "$frames" --eeprom-ro \
        --wav audio.wav --shot shot.ppm >/dev/null 2>"$dir/run.log" ) || return 1

    [ -s "$dir/trace.txt" ] && [ -s "$dir/audio.wav" ] && [ -s "$dir/shot.ppm" ] || return 1
    return 0
}

# empreintes d'un jeu déjà exécuté : "trace wav shot"
fingerprint() {
    local dir=$work/$1
    printf '%s %s %s\n' "$(sha < "$dir/trace.txt")" "$(sha < "$dir/audio.wav")" "$(sha < "$dir/shot.ppm")"
}

fails=0
tmp_refs=$work/refs.new
if [ "$update" -eq 1 ]; then
    printf '# empreintes de référence de tests/parity/run.sh (SHA-256)\n' > "$tmp_refs"
    printf '# trace : lignes [frame] de EMU_TRACE ; wav : --wav ; shot : --shot\n' >> "$tmp_refs"
    printf '# %s frames par jeu\n' "$frames" >> "$tmp_refs"
fi

while read -r name file input; do
    case "$name" in ''|'#'*) continue ;; esac
    if [ ! -f "$games_dir/$file" ]; then
        printf '%-16s absent (%s)\n' "$name" "$games_dir/$file"; fails=$((fails + 1)); continue
    fi
    if ! run_game "$name" "$file" "$input"; then
        printf '%-16s ÉCHEC (gm0 a planté ou sortie vide)\n' "$name"; fails=$((fails + 1)); continue
    fi
    read -r t w s < <(fingerprint "$name")
    if [ "$update" -eq 1 ]; then
        printf '%s trace %s\n%s wav %s\n%s shot %s\n' "$name" "$t" "$name" "$w" "$name" "$s" >> "$tmp_refs"
        printf '%-16s écrit\n' "$name"
        continue
    fi
    line=$name
    for pair in "trace $t" "wav $w" "shot $s"; do
        kind=${pair%% *}; got=${pair#* }
        want=$(grep "^$name $kind " "$refs" | cut -d' ' -f3)
        if [ -z "$want" ]; then line="$line  $kind ABSENT"; fails=$((fails + 1))
        elif [ "$got" = "$want" ]; then line="$line  $kind ok"
        else line="$line  $kind DIFFÉRENT"; fails=$((fails + 1)); fi
    done
    printf '%s\n' "$line"
done < "$list"

if [ "$update" -eq 1 ]; then
    [ "$fails" -eq 0 ] || { echo "échecs : $fails — refs.txt non écrit" >&2; exit 1; }
    mv "$tmp_refs" "$refs"
    echo "refs.txt écrit"
    exit 0
fi
if [ "$fails" -eq 0 ]; then echo "parité : OK"; exit 0; fi
echo "parité : $fails écart(s)" >&2
exit 1
