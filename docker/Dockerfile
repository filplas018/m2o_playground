# Běhové prostředí pro dedikovaný server M2O (Mafia 2 Online).
# Binárky serveru (M2OServer, libnode.so.141, crashpad_handler) v image NEJSOU – stahují se
# z Discordu M2O (#downloads) a připojují se jako volume (./server). Nový build = rozbalit
# a restartovat kontejner, image se nepřestavuje.
#
# Ubuntu 24.04: server potřebuje glibc 2.38+ (Ubuntu 22.04 / Debian 12 nestačí).
FROM ubuntu:24.04

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    libatomic1 libstdc++6 libgcc-s1 libcurl4t64 openssl ca-certificates tini \
  && rm -rf /var/lib/apt/lists/*

# libnode.so.141 leží vedle M2OServer; bez invariantního TSC (VPS) by profiler Tracy
# server při startu shodil s kódem 1 – proměnná je neškodná všude
ENV LD_LIBRARY_PATH=/opt/m2o \
    TRACY_NO_INVARIANT_CHECK=1

COPY entrypoint.sh /usr/local/bin/m2o-entrypoint
# /opt/m2o patří uživateli ubuntu – entrypoint sem kopíruje binárky, server sem může zapisovat
RUN chmod +x /usr/local/bin/m2o-entrypoint \
  && mkdir -p /opt/m2o /opt/m2o-dist \
  && chown ubuntu:ubuntu /opt/m2o
WORKDIR /opt/m2o
# Uživatel ubuntu (UID 1000) – soubory v připojených složkách patří stejnému UID jako na hostu
USER ubuntu

# UDP = herní provoz, TCP port+1 = HTTP s informacemi pro server browser / masterlist
EXPOSE 27015/udp 27016/tcp

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/m2o-entrypoint"]
