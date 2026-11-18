import { afterAll, beforeAll, describe, expect, test } from "bun:test";
import { spawn, type Subprocess } from "bun";
import { join } from "node:path";
import { authSignedMessage, publicKeyDigest } from "../../../cloud/cloudflare/src/crypto.ts";
import { PUBLIC_KEY_BYTES } from "../../../cloud/cloudflare/src/protocol.ts";

const BIN_PATH = process.env.SAIHUB_CLOUD_BIN || join(import.meta.dir, "../../../cloud/cpp/build/saihub-cloud");
const BASE_PORT = 9900 + Math.floor(Math.random() * 900);
const ROUTING_TOKEN_SECRET = "local-dev-routing-token-secret";
const WEBSOCKET_TOKEN_SECRET = "local-dev-websocket-token-secret";
const ADMIN_TOKEN = "local-dev-admin-token";

type RequestMessage = { type: "request"; requestId: string; path: string };

function bytesToBase64Url(bytes: ArrayBuffer | Uint8Array): string {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let binary = "";
  for (let i = 0; i < view.length; i += 1) binary += String.fromCharCode(view[i]!);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function base64UrlToBytes(value: string): Uint8Array {
  const padded = value + "=".repeat((4 - (value.length % 4)) % 4);
  const binary = atob(padded.replace(/-/g, "+").replace(/_/g, "/"));
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) out[i] = binary.charCodeAt(i);
  return out;
}

async function sealRoutingToken(digest: string, grantSecret = "grant-secret"): Promise<string> {
  const keyMaterial = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(ROUTING_TOKEN_SECRET));
  const key = await crypto.subtle.importKey("raw", keyMaterial, { name: "AES-GCM" }, false, ["encrypt"]);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const plaintext = new TextEncoder().encode(JSON.stringify({ devicePublicKeyDigest: digest, grantSecret }));
  const ciphertext = new Uint8Array(await crypto.subtle.encrypt({ name: "AES-GCM", iv }, key, plaintext));
  const packed = new Uint8Array(iv.length + ciphertext.length);
  packed.set(iv, 0);
  packed.set(ciphertext, iv.length);
  return bytesToBase64Url(packed);
}

async function generateIdentity() {
  const kp = (await crypto.subtle.generateKey({ name: "ECDSA", namedCurve: "P-256" }, true, ["sign", "verify"])) as CryptoKeyPair;
  const raw = new Uint8Array((await crypto.subtle.exportKey("raw", kp.publicKey)) as ArrayBuffer);
  expect(raw.length).toBe(PUBLIC_KEY_BYTES);
  const digest = await publicKeyDigest(raw);
  if (!digest) throw new Error("digest failed");
  return { privateKey: kp.privateKey, digest, publicKeyB64: bytesToBase64Url(raw) };
}

async function openAndAuth(baseUrl: string) {
  const id = await generateIdentity();
  const wl = await fetch(`${baseUrl}/admin/devices/whitelist`, {
    method: "POST",
    headers: { authorization: `Bearer ${ADMIN_TOKEN}`, "content-type": "application/json" },
    body: JSON.stringify({ digest: id.digest }),
  });
  expect(wl.status).toBe(201);

  const ws = new WebSocket(baseUrl.replace(/^http/, "ws") + `/cloud/device/${id.digest}`);
  const requests: RequestMessage[] = [];
  await new Promise<void>((resolve, reject) => {
    ws.addEventListener("message", async (ev) => {
      const msg = JSON.parse(String(ev.data)) as Record<string, unknown>;
      if (msg.type === "authRequest") {
        const ch = base64UrlToBytes(String(msg.challenge));
        const sig = await crypto.subtle.sign({ name: "ECDSA", hash: "SHA-256" }, id.privateKey, authSignedMessage(ch));
        ws.send(JSON.stringify({
          type: "authResponse", devicePublicKey: id.publicKeyB64, devicePublicKeyDigest: id.digest,
          version: "headroom-1.0", response: bytesToBase64Url(sig),
        }));
      } else if (msg.type === "authResult") {
        if (msg.success) resolve(); else reject(new Error(String(msg.reason)));
      } else if (msg.type === "request") {
        requests.push(msg as RequestMessage);
      }
    });
    ws.addEventListener("error", () => reject(new Error("ws error")));
  });
  return { ws, id, requests, token: await sealRoutingToken(id.digest) };
}

describe("cpp headroom e2e", () => {
  let proc: Subprocess | null = null;
  let baseUrl = "";

  beforeAll(async () => {
    baseUrl = `http://127.0.0.1:${BASE_PORT}`;
    proc = spawn({
      cmd: [BIN_PATH],
      stdout: "pipe",
      stderr: "pipe",
      env: {
        ...process.env,
        HOST: "127.0.0.1",
        PORT: String(BASE_PORT),
        AUTH_TIMEOUT_MS: "2000",
        REQUEST_TIMEOUT_MS: "600",
        WEBSOCKET_TOKEN_TTL_S: "7200",
        ROUTING_TOKEN_SECRET,
        WEBSOCKET_TOKEN_SECRET,
        ADMIN_TOKEN,
        DB_PATH: `/tmp/saihub-headroom-${BASE_PORT}.db`,
      },
    });
    for (let i = 0; i < 50; i += 1) {
      try {
        const r = await fetch(`${baseUrl}/__ready__`);
        if (r.status === 404) break;
      } catch {}
      await Bun.sleep(50);
    }
  });

  afterAll(async () => {
    if (proc) {
      proc.kill();
      await proc.exited;
      proc = null;
    }
  });

  test("caps pending at eight and promotes one FIFO request per response", async () => {
    const { ws, id, requests, token } = await openAndAuth(baseUrl);
    try {
      const promises = Array.from({ length: 11 }, (_, i) =>
        fetch(`${baseUrl}/device/${id.digest}/request/${i}`, { headers: { authorization: `Bearer ${token}` } }),
      );
      await Bun.sleep(100);
      expect(requests.map((m) => m.path)).toEqual(Array.from({ length: 8 }, (_, i) => `/request/${i}`));

      const reply = (idx: number) => ws.send(JSON.stringify({
        type: "response", requestId: requests[idx]!.requestId, status: 200,
        headers: { "content-type": "application/json" }, body: { idx },
      }));

      reply(3);
      await Bun.sleep(40);
      expect(requests.length).toBe(9);
      expect(requests[8]!.path).toBe("/request/8");

      reply(3); // duplicate must not promote
      await Bun.sleep(40);
      expect(requests.length).toBe(9);

      reply(0);
      await Bun.sleep(40);
      expect(requests[9]!.path).toBe("/request/9");

      reply(1);
      await Bun.sleep(40);
      expect(requests[10]!.path).toBe("/request/10");

      for (const i of [2, 4, 5, 6, 7, 8, 9, 10]) reply(i);
      const results = await Promise.all(promises);
      expect(results.every((r) => r.status === 200)).toBe(true);
    } finally { ws.close(); }
  });

  test("timeouts free slots and disconnect rejects queued requests", async () => {
    const { ws, id, requests, token } = await openAndAuth(baseUrl);
    const promises = Array.from({ length: 10 }, (_, i) =>
      fetch(`${baseUrl}/device/${id.digest}/req/${i}`, { headers: { authorization: `Bearer ${token}` } }),
    );
    await Bun.sleep(80);
    expect(requests.length).toBe(8);
    // Wait for initial 8 to time out (600ms), promoting 8 & 9
    await Bun.sleep(700);
    expect(requests.length).toBe(10);
    const first8 = await Promise.all(promises.slice(0, 8));
    expect(first8.every((r) => r.status === 504)).toBe(true);
    ws.close();
    const last2 = await Promise.all(promises.slice(8));
    expect(last2.every((r) => r.status === 503)).toBe(true);
  });
});
