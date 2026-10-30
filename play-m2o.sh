#!/bin/sh
# Spustí klienta M2O (M2OLauncher.exe, Windows) přes Proton ve stejném prefixu, ve kterém Steam
# spouští Mafii II (Classic, appid 50130). Steam musí běžet – klient se přes něj přihlašuje (steamId).
#   ./play-m2o.sh
set -eu

STEAM=${STEAM:-$HOME/.steam/debian-installation}
APPID=50130
CLIENT=${M2O_CLIENT:-$HOME/Games/M2O-client}
PROTON=${PROTON:-"$STEAM/steamapps/common/Proton - Experimental"}
RUNTIME="$STEAM/steamapps/common/SteamLinuxRuntime_sniper"

pgrep -x steam > /dev/null || { echo "Spusťte nejdřív Steam." >&2; exit 1; }
[ -f "$CLIENT/M2OLauncher.exe" ] || { echo "Chybí $CLIENT/M2OLauncher.exe" >&2; exit 1; }

export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM"
export STEAM_COMPAT_DATA_PATH="$STEAM/steamapps/compatdata/$APPID"
export STEAM_COMPAT_INSTALL_PATH="$STEAM/steamapps/common/Mafia II"
export SteamAppId=$APPID SteamGameId=$APPID
# Log Protonu: ~/steam-50130.log
export PROTON_LOG=${PROTON_LOG:-1}
# luabridge: proxy dinput8.dll ve složce hry (native/luabridge) má přednost před vestavěným
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}dinput8=n,b"

cd "$CLIENT"
exec "$RUNTIME/_v2-entry-point" --verb=waitforexitandrun -- "$PROTON/proton" waitforexitandrun "$CLIENT/M2OLauncher.exe" "$@"
