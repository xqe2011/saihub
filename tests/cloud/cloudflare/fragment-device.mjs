// Run under Node: Bun's ws compatibility layer does not implement fragmented sends.
import { createRequire } from "node:module";
const require = createRequire(new URL("../../../cloud/cloudflare/package.json", import.meta.url));
const WebSocket = require("ws");
const { url, digest, publicKeyB64, privateKey } = JSON.parse(process.argv[2]);
const key = await crypto.subtle.importKey("jwk", privateKey, { name: "ECDSA", namedCurve: "P-256" }, false, ["sign"]);
const ws = new WebSocket(url + `/cloud/device/${digest}`);
ws.on("error", (error) => { console.error(error); process.exit(1); });
ws.on("message", async (raw) => {
  const message = JSON.parse(raw.toString());
  if (message.type === "authRequest") {
    const signed = Buffer.concat([Buffer.from("saihub/cloud-auth/v1\0"), Buffer.from(message.challenge, "base64url")]);
    const signature = await crypto.subtle.sign({ name: "ECDSA", hash: "SHA-256" }, key, signed);
    ws.send(JSON.stringify({ type: "authResponse", devicePublicKey: publicKeyB64, devicePublicKeyDigest: digest,
      version: "fragment-test", response: Buffer.from(signature).toString("base64url") }));
  } else if (message.type === "authResult") {
    if (!message.success) process.exit(1);
    console.log("ready");
  } else if (message.type === "request") {
    if (message.body !== null || message.headers["content-type"] !== undefined) process.exit(1);
    const prefix = JSON.stringify({ type: "response", requestId: message.requestId, status: 200, headers: { "content-type": "application/json" } });
    ws.send(prefix.slice(0, -1) + ',"body":', { fin: false });
    ws.send('{"events":[', { fin: false });
    for (let i = 0; i < 2000; i++) {
      ws.send((i ? "," : "") + JSON.stringify({ pin: 1, level: i % 2, time: i, label: "测试" }), { fin: false });
    }
    ws.send("]}}", { fin: true });
  }
});
