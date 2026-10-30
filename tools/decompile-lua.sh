#!/bin/sh
# Dekompiluje Lua 5.1 bytecode Mafie II (lua_Number = 4 B float) na zdrojový kód přes unluac.
#   tools/decompile-lua.sh <složka s bytecode> <výstupní složka>
# Potřebuje Javu: $JAVA, nebo ~/Documents/m2o_extract/tools/jre/bin/java, nebo java v PATH,
# a unluac.jar ($UNLUAC, výchozí ~/Documents/m2o_extract/tools/unluac.jar – sourceforge.net/projects/unluac).
set -eu

[ $# -eq 2 ] || { echo "Použití: $0 <bytecode> <výstup>" >&2; exit 1; }
SRC=$(cd "$1" && pwd)
OUT=$2
TOOLS=$HOME/Documents/m2o_extract/tools
JAVA=${JAVA:-$( [ -x "$TOOLS/jre/bin/java" ] && echo "$TOOLS/jre/bin/java" || command -v java )}
UNLUAC=${UNLUAC:-$TOOLS/unluac.jar}
[ -n "$JAVA" ] && [ -f "$UNLUAC" ] || { echo "Chybí Java nebo $UNLUAC" >&2; exit 1; }

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
export JAVA UNLUAC SRC OUT

# Paralelně podle počtu jader. Bytecode mají i soubory .AP a .SHP – rozhoduje hlavička "\033Lua".
# Soubory, které unluac nezvládne, skončí ve failed.txt
cd "$SRC"
: > "$OUT/failed.txt"
find . -type f | xargs -P "$(nproc)" -I{} sh -c '
  [ "$(head -c 4 "{}" | od -An -c | tr -d " ")" = "033Lua" ] || exit 0
  target="$OUT/{}"
  mkdir -p "$(dirname "$target")"
  "$JAVA" -jar "$UNLUAC" "{}" > "$target" 2>/dev/null || { rm -f "$target"; echo "{}" >> "$OUT/failed.txt"; }
'
echo "Hotovo: $(find "$OUT" -type f ! -name failed.txt | wc -l) souborů v $OUT, selhalo: $(wc -l < "$OUT/failed.txt")"
