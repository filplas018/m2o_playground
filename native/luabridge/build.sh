#!/bin/sh
# Sestaví 32bitové dinput8.dll (luabridge) pro mafia2.exe křížovým překladačem llvm-mingw.
#   native/luabridge/build.sh            → native/luabridge/build/dinput8.dll
#   native/luabridge/build.sh install    → navíc zkopíruje DLL + luabridge.ini do složky hry
set -eu

cd "$(dirname "$0")"
CC=${CC:-$(ls -d "$HOME"/.local/opt/llvm-mingw-*/bin 2>/dev/null | head -1)/i686-w64-mingw32-gcc}
GAME=${GAME:-"$HOME/.steam/debian-installation/steamapps/common/Mafia II/pc"}

mkdir -p build
"$CC" -O2 -Wall -Wextra -shared -static -s -o build/dinput8.dll luabridge.c dinput8.def -lws2_32
echo "Sestaveno: $(pwd)/build/dinput8.dll"

if [ "${1:-}" = "install" ]; then
  cp build/dinput8.dll "$GAME/dinput8.dll"
  [ -f "$GAME/luabridge.ini" ] || cp luabridge.ini "$GAME/luabridge.ini"
  echo "Nainstalováno do $GAME (spouštět s WINEDLLOVERRIDES=dinput8=n,b – play-m2o.sh to nastaví)"
fi
