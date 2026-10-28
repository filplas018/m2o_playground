// Klient (sandboxovaný V8 u každého hráče) – HUD, vstupy, kamera, web views.
const RESOURCE = "playground";

/**
 * Whitelist lokálních volání, o která smí požádat server (intents down).
 * @type {Record<string, (args: any) => void>}
 */
const CLIENT_CALLS = {
  "hud.showMessage": (a) => Hud.showMessage(a.text, a.seconds),
  "hud.showHint": (a) => Hud.showHint(a.text, a.area ?? 3, -1, a.seconds ?? 5),
  "camera.reset": () => Camera.reset(),
};

Events.on("playground:client", (/** @type {any} */ data) => {
  const call = data && CLIENT_CALLS[data.fn];
  if (call) call(data.args || {});
});

Events.on("resourceStart", (resourceName) => {
  if (resourceName !== RESOURCE) return;

  // Bindy patří resource a po jeho zastavení se samy zruší
  Key.bind("h", "down", () => Hud.setVisible(!Hud.isVisible()));
  // Sdílený stav mění jen server – klient pošle požadavek (consequences up)
  Key.bind("e", "down", () => Events.emitServer("playground:toggle", { accessory: "engine" }));
  Key.bind("l", "down", () => Events.emitServer("playground:toggle", { accessory: "lights" }));
});
