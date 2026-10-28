#!/bin/sh
# Spustí M2OServer. Binárky z build archivu jsou připojené jen pro čtení v /opt/m2o-dist
# (./server na hostu); před startem se zkopírují do /opt/m2o, kde jsou připojené i resources/
# a data/. server.json leží ve složce ./config (/opt/m2o-config) a do /opt/m2o se nalinkuje –
# server tak má vše „vedle sebe“, jak očekává. Připojené složky se nevnořují do sebe a nepřipojuje
# se samotný soubor (Docker Desktop to neumí spolehlivě).
set -eu

DIST=/opt/m2o-dist
cd /opt/m2o

# Verze libnode se mezi buildy mění (dokumentace uvádí .141, build 1.7.0 má .147)
if [ ! -f "$DIST/M2OServer" ] || ! ls "$DIST"/libnode.so.* > /dev/null 2>&1; then
  echo "Ve složce ./server chybí M2OServer nebo libnode.so.*." >&2
  echo "Zkopírujte binárky ze server-linux/ z build archivu M2O (Discord, #downloads):" >&2
  echo "  cp M2O-<verze>/server-linux/{M2OServer,libnode.so.*,crashpad_handler} server/" >&2
  exit 1
fi

# Jen binárky a knihovny – server.json a resources/ z ./server se nepřebírají
for file in "$DIST"/*; do
  [ -f "$file" ] && cp -f "$file" .
done
# Zip nemusí zachovat spustitelný bit
chmod +x M2OServer
[ ! -f crashpad_handler ] || chmod +x crashpad_handler

# Bez server.json ho server při prvním startu vytvoří s výchozími hodnotami – přes odkaz
# rovnou do ./config na hostu
ln -sf /opt/m2o-config/server.json server.json

exec ./M2OServer "$@"
