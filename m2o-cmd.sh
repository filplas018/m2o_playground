#!/bin/sh
# Pošle příkaz do konzole běžícího serveru a vypíše, co server odpověděl.
#   ./m2o-cmd.sh "restart playground"     znovu načte kód resource (hot reload)
#   ./m2o-cmd.sh refresh                  najde nové / změněné resources
#   ./m2o-cmd.sh status
# Kontejner má TTY (kvůli interaktivní konzoli), proto se připojuje přes `script` (pseudo-terminál)
# a odpojí se sekvencí Ctrl+P, Ctrl+Q – server běží dál.
set -eu

CONTAINER=m2o-server
[ $# -gt 0 ] || { echo "Použití: $0 \"<příkaz>\"" >&2; exit 1; }

since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
(sleep 0.5; printf '%s\r' "$*"; sleep 1.5; printf '\020\021') |
  script -qfc "docker attach $CONTAINER" /dev/null > /dev/null 2>&1 || true
docker logs --since "$since" "$CONTAINER" 2>&1
