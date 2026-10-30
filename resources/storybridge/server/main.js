// storybridge – server strana mostu k Lua enginu Mafie II.
// Hráč s nativním doplňkem luabridge (dinput8.dll ve složce hry) se připojí TCP na tento port
// a představí se Steam ID. Server mu pak může poslat Lua kód, který se spustí v jeho hře.
// Protokol (řádkový): klient → HELLO <steamid> <verze>, RESULT <id> <kód>
//                     server → LUA <id> <délka>\n<kód>
const net = require("node:net");

const RESOURCE = "storybridge";
const PORT = Number(process.env.STORYBRIDGE_PORT || 27020);
const RESULT_TIMEOUT_MS = 10000;

/** steamId → { socket, version, pending: Map<id, resolve> } */
const bridges = new Map();
let nextId = 1;

const server = net.createServer((socket) => {
  socket.setNoDelay(true);
  let buffer = Buffer.alloc(0);
  let steamId = null;

  socket.on("data", (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    let newline;
    while ((newline = buffer.indexOf(0x0a)) >= 0) {
      const [kind, ...rest] = buffer.subarray(0, newline).toString("latin1").trim().split(" ");
      if (kind === "RESULT") {
        // RESULT <id> <kód> <délka>\n<text> – text může obsahovat i konce řádků
        const length = Number(rest[2] || 0);
        if (buffer.length < newline + 1 + length) return; // text ještě nedorazil celý
        const value = length ? buffer.subarray(newline + 1, newline + 1 + length).toString("utf8") : null;
        buffer = buffer.subarray(newline + 1 + length);
        const bridge = steamId && bridges.get(steamId);
        const resolve = bridge?.pending.get(Number(rest[0]));
        if (resolve) {
          bridge.pending.delete(Number(rest[0]));
          resolve({ code: Number(rest[1]), value });
        }
        continue;
      }
      buffer = buffer.subarray(newline + 1);
      if (kind === "HELLO" && /^\d{1,20}$/.test(rest[0] || "")) {
        // Bez Steam ID (0) se bridge vede jako anonymní a páruje se s jediným hráčem bez bridge
        steamId = rest[0] === "0" ? `anon:${socket.remoteAddress}:${socket.remotePort}` : rest[0];
        bridges.get(steamId)?.socket.destroy();
        bridges.set(steamId, { socket, version: rest[1] || "?", pending: new Map() });
        console.log(`luabridge připojen: steamId ${steamId}, verze ${rest[1]} (${socket.remoteAddress})`);
      }
    }
  });
  socket.on("error", () => {});
  socket.on("close", () => {
    if (steamId && bridges.get(steamId)?.socket === socket) {
      for (const resolve of bridges.get(steamId).pending.values()) resolve({ code: -2, value: null });
      bridges.delete(steamId);
      console.log(`luabridge odpojen: steamId ${steamId}`);
    }
  });
});

/**
 * Bridge hráče: podle Steam ID, jinak jediný anonymní bridge, pokud je online jediný hráč bez
 * vlastního bridge (lokální test, klient, kterému hra nevrátila Steam ID).
 */
function bridgeFor(player) {
  const own = bridges.get(String(player.steamId));
  if (own) return own;
  const anonymous = [...bridges.entries()].filter(([key]) => key.startsWith("anon:"));
  const withoutBridge = World.players.filter((p) => !bridges.has(String(p.steamId)));
  return anonymous.length === 1 && withoutBridge.length === 1 && withoutBridge[0].id === player.id
    ? anonymous[0][1]
    : undefined;
}

/** Má hráč připojený luabridge? */
function hasBridge(player) {
  return bridgeFor(player) !== undefined;
}

/**
 * Spustí Lua v enginu hry daného hráče (jen u něj – nic se nesynchronizuje).
 * code: 0 = OK, 1xx = chyba načtení (syntaxe), 2xx = chyba za běhu, -1 = Lua stav nedostupný,
 * -2 = bridge odpojen, -3 = timeout. value: první návratová hodnota chunku (`return …`) nebo
 * chybová hláška jako text, pokud jde na text převést. null = hráč bridge nemá.
 * @param {Player} player
 * @param {string} code
 * @returns {Promise<{ code: number, value: string | null } | null>}
 */
function runLua(player, code) {
  const bridge = bridgeFor(player);
  if (!bridge) return Promise.resolve(null);
  const id = nextId++;
  const body = Buffer.from(code, "utf8");
  bridge.socket.write(`LUA ${id} ${body.length}\n`);
  bridge.socket.write(body);
  return new Promise((resolve) => {
    bridge.pending.set(id, resolve);
    setTimeout(() => {
      if (bridge.pending.delete(id)) resolve({ code: -3, value: null });
    }, RESULT_TIMEOUT_MS);
  });
}

/** Výsledek runLua jako krátký text pro chat/konzoli */
function describeResult(result) {
  if (!result) return "luabridge není připojen.";
  return result.value === null ? `${result.code}` : `${result.code}: ${result.value}`;
}

Exports.register("runLua", runLua);
Exports.register("hasBridge", hasBridge);

/** @param {Player} player @param {string} text */
function say(player, text) {
  Chat.sendToPlayer(player, text, { author: "storybridge", color: 0x9ad0ffff });
}

/** @type {Record<string, (player: Player, args: string[]) => Promise<void> | void>} */
const COMMANDS = {
  bridge(player) {
    const bridge = bridgeFor(player);
    say(player, bridge ? `luabridge připojen (verze ${bridge.version}).` : "luabridge není připojen.");
  },
  // Ladění: /lua <kód> – spustí Lua jen ve vlastní hře
  async lua(player, args) {
    say(player, `Lua → ${describeResult(await runLua(player, args.join(" ")))}`);
  },
  // Ukázka: stmívání a úkol s cílem na GPS (texty bere hra sama z TextDatabase podle ID)
  async questdemo(player) {
    const { x, y } = player.position;
    const result = await runLua(
      player,
      `game.hud:FaderFadeOut(500)
       DelayBuffer:Insert(function() game.hud:FaderFadeIn(1500) end, {}, 800, 1, false)
       storyQuest = game.quests:NewQuest(5, 12, true, "0056000005", "0056000005")
       storyObjective = game.navigation:RegisterObjectivePos(${x + 150}, ${y}, storyQuest:AddObjective("0251010018"))`,
    );
    say(player, `questdemo → ${describeResult(result)}`);
  },
};

Events.on("playerCommand", (player, command, args) => {
  const handler = COMMANDS[command.toLowerCase()];
  if (handler) return handler(player, args);
});

// Konzole serveru (./m2o-cmd.sh "bridge …"): stav, Lua a ukázka pro všechny připojené hráče
//   bridge status | bridge lua <kód> | bridge questdemo
Events.on("consoleCommand", async (command, args) => {
  if (command !== "bridge") return;
  const [action, ...rest] = args;
  const players = World.players.filter(() => true);
  if (action === "status" || !action) {
    console.log(`bridges: ${[...bridges.keys()].join(", ") || "žádný"}`);
    for (const player of players) console.log(`  ${player.nickname} (${player.steamId}): ${hasBridge(player) ? "bridge" : "-"}`);
    return;
  }
  for (const player of players) {
    if (action === "lua") console.log(`${player.nickname}: Lua → ${describeResult(await runLua(player, rest.join(" ")))}`);
    if (action === "questdemo") await COMMANDS.questdemo(player);
  }
});

Events.on("playerConnect", (player) => {
  // Bridge se připojuje nezávisle na M2O – dáme mu chvíli
  setTimeout(() => {
    if (World.players.some((p) => p.id === player.id)) {
      say(player, hasBridge(player) ? "luabridge: připojen." : "luabridge: nepřipojen (vlastní příběh nepoběží).");
    }
  }, 8000);
});

Events.on("resourceStart", (resourceName) => {
  if (resourceName !== RESOURCE) return;
  server.listen(PORT, "0.0.0.0", () => console.log(`storybridge naslouchá na TCP ${PORT}`));
});

Events.on("resourceStop", (resourceName) => {
  if (resourceName !== RESOURCE) return;
  for (const bridge of bridges.values()) bridge.socket.destroy();
  bridges.clear();
  server.close();
});
