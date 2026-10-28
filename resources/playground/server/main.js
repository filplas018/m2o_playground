// Server (Node.js) – autoritativní stav: hráči, vozidla, svět, peníze.
const RESOURCE = "playground";
const SPAWN = { x: -1649.5, y: 65.267, z: -9.541 }; // respawn bod z dokumentace (player-lifecycle)

/** Vozidla vytvořená tímto resource – po zastavení resource je musíme zničit sami */
const spawnedVehicles = new Map(); // vehicle.id -> Vehicle

/**
 * „Intent“ pro klienta: server UI nekreslí, jen požádá klienta o lokální volání
 * z jeho whitelistu (CLIENT_CALLS v client/main.js).
 * @param {Player} player
 * @param {string} fn
 * @param {Record<string, unknown>} [args]
 */
function clientCall(player, fn, args) {
  player.emit("playground:client", JSON.stringify({ fn, args: args || {} }));
}

/** @param {Player} player @param {string} text */
function notify(player, text) {
  Chat.sendToPlayer(player, text, { author: "Server", color: 0xd2b48cff });
}

Events.on("resourceStart", (resourceName) => {
  if (resourceName !== RESOURCE) return;
  console.log("spuštěno");
  // Stav světa se sám přehraje i hráčům, kteří se připojí později
  World.setTime(15, 30);
  World.setWeather("DTFreeRideDay");
});

Events.on("resourceStop", (resourceName) => {
  if (resourceName !== RESOURCE) return;
  for (const vehicle of spawnedVehicles.values()) {
    try {
      vehicle.destroy();
    } catch {
      // už neexistuje
    }
  }
  spawnedVehicles.clear();
});

Events.on("playerConnect", (player) => {
  console.log(`${player.nickname} se připojil (ping ${player.ping} ms, steamId ${player.steamId})`);
  clientCall(player, "hud.showMessage", { text: `Vítej v Empire Bay, ${player.nickname}!`, seconds: 5 });
  player.spawn(SPAWN.x, SPAWN.y, SPAWN.z);
});

// Automatický respawn neexistuje – řeší ho game mode
Events.on("playerDeath", (player, killer, info) => {
  const cause = info.vehicle ? "přejetí" : `zbraň ${info.weaponId}`;
  console.log(`${player.nickname} zemřel (${cause}${info.bodyPart === 2 ? ", headshot" : ""})`);
  setTimeout(() => {
    if (player.isDead()) player.spawn(SPAWN.x, SPAWN.y, SPAWN.z);
  }, 3000);
});

/** @type {Record<string, (player: Player, args: string[]) => void>} */
const COMMANDS = {
  veh(player, args) {
    const model = Number(args[0] ?? 32);
    if (!Number.isInteger(model) || model < 1 || model > 57) return notify(player, "Použití: /veh <1-57>");
    const p = player.position;
    const vehicle = Vehicle.spawn(model, new Vector3(p.x + 3, p.y, p.z));
    spawnedVehicles.set(vehicle.id, vehicle);
    notify(player, `Vozidlo ${model} je připravené.`);
  },
  heal(player) {
    player.restoreHealth();
    notify(player, "Zdraví obnoveno.");
  },
  pos(player) {
    const { x, y, z } = player.position;
    notify(player, `Pozice: ${x.toFixed(2)}, ${y.toFixed(2)}, ${z.toFixed(2)}`);
  },
};

Events.on("playerCommand", (player, command, args) => {
  const handler = COMMANDS[command.toLowerCase()];
  if (!handler) return notify(player, `Neznámý příkaz /${command}`);
  handler(player, args);
});

Events.on("vehicleDestroy", (vehicle) => {
  spawnedVehicles.delete(vehicle.id);
});

// Klient → server: payload je nedůvěryhodný, hráč se bere z prvního argumentu
// EventHandler má v typech argumenty `unknown` – typy hráče a payloadu je potřeba uvést
Events.onClient("playground:toggle", (/** @type {Player} */ player, /** @type {any} */ data) => {
  const accessory = data && data.accessory;
  if (accessory !== "engine" && accessory !== "lights") return;
  const vehicle = player.getVehicle();
  if (!vehicle) return;
  if (accessory === "engine") vehicle.toggleEngine();
  else vehicle.toggleLights();
});

// Řádky konzole serveru (docker attach m2o-server), které nejsou vestavěný příkaz
Events.on("consoleCommand", (command) => {
  if (command === "players") {
    console.log(`Online: ${World.players.map((player) => player.nickname).join(", ") || "nikdo"}`);
  }
});
