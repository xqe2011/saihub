/**
 * Local fake device for pairing / proxy / MCP testing.
 * Usage: bun tests/cloud/cloudflare/fake-device.ts [cloudBaseUrl]
 * Identity is persisted in tests/cloud/cloudflare/.fake-device-key.json so the digest stays stable.
 */
import { mkdirSync, readFileSync, writeFileSync, existsSync, unlinkSync } from "node:fs";
import { dirname, join } from "node:path";
import { authSignedMessage, publicKeyDigest } from "../../../cloud/cloudflare/src/crypto.ts";
import { PUBLIC_KEY_BYTES, SIGNATURE_BYTES } from "../../../cloud/cloudflare/src/protocol.ts";

const CLOUD = (process.argv[2] ?? "http://127.0.0.1:8787").replace(/\/$/, "");
const KEY_PATH = join(import.meta.dir, ".fake-device-key.json");
const IDENTITY_VERSION = 2;

function bytesToBase64Url(bytes: ArrayBuffer | Uint8Array): string {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let binary = "";
  for (let i = 0; i < view.length; i += 1) {
    binary += String.fromCharCode(view[i]!);
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function base64UrlToBytes(value: string): Uint8Array {
  const padded = value + "=".repeat((4 - (value.length % 4)) % 4);
  const base64 = padded.replace(/-/g, "+").replace(/_/g, "/");
  const binary = atob(base64);
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) {
    out[i] = binary.charCodeAt(i);
  }
  return out;
}

function reply(
  ws: WebSocket,
  requestId: string,
  status: number,
  body: unknown,
  headers: Record<string, string> = { "content-type": "application/json" },
): void {
  ws.send(
    JSON.stringify({
      type: "response",
      requestId,
      status,
      headers,
      body: body === "" ? null : body,
    }),
  );
}

function handleMcp(ws: WebSocket, requestId: string, method: string, rawBody: string): void {
  if (method === "GET" || method === "DELETE") {
    reply(ws, requestId, 405, { jsonrpc: "2.0", error: { code: -32600, message: "Method Not Allowed" }, id: null });
    return;
  }
  if (method === "OPTIONS") {
    reply(ws, requestId, 204, "", {});
    return;
  }

  let msg: Record<string, unknown>;
  try {
    msg = JSON.parse(rawBody || "{}") as Record<string, unknown>;
  } catch {
    reply(ws, requestId, 400, { jsonrpc: "2.0", error: { code: -32700, message: "Parse error" }, id: null });
    return;
  }

  const rpcMethod = typeof msg.method === "string" ? msg.method : null;
  const id = msg.id ?? null;

  if (!rpcMethod) {
    if (id !== null && id !== undefined) {
      reply(ws, requestId, 202, "", {});
      return;
    }
    reply(ws, requestId, 400, { jsonrpc: "2.0", error: { code: -32600, message: "Invalid Request" }, id: null });
    return;
  }

  // notifications
  if (id === null || id === undefined) {
    reply(ws, requestId, 202, "", {});
    return;
  }

  if (rpcMethod === "initialize") {
    const params = (msg.params ?? {}) as Record<string, unknown>;
    const requested = typeof params.protocolVersion === "string" ? params.protocolVersion : "2025-06-18";
    const protocolVersion =
      requested === "2025-06-18" || requested === "2025-03-26" || requested === "2024-11-05" ? "2025-06-18" : "2025-06-18";
    reply(ws, requestId, 200, {
      jsonrpc: "2.0",
      id,
      result: {
        protocolVersion,
        capabilities: { tools: {} },
        serverInfo: { name: "saihub-fake", version: "1.0.0" },
        instructions: "Fake cloud-paired device for local MCP OAuth testing.",
      },
    });
    console.log("→ mcp initialize");
    return;
  }

  if (rpcMethod === "ping") {
    reply(ws, requestId, 200, { jsonrpc: "2.0", id, result: {} });
    return;
  }

  if (rpcMethod === "tools/list") {
    reply(ws, requestId, 200, {
      jsonrpc: "2.0",
      id,
      result: {
        tools: [
          {
            name: "echo",
            description: "Echo a message (fake device).",
            inputSchema: {
              type: "object",
              properties: { message: { type: "string" } },
              required: ["message"],
            },
          },
        ],
      },
    });
    console.log("→ mcp tools/list");
    return;
  }

  if (rpcMethod === "tools/call") {
    const params = (msg.params ?? {}) as Record<string, unknown>;
    const name = typeof params.name === "string" ? params.name : "";
    const args = (params.arguments ?? {}) as Record<string, unknown>;
    const message = typeof args.message === "string" ? args.message : "";
    reply(ws, requestId, 200, {
      jsonrpc: "2.0",
      id,
      result: {
        content: [{ type: "text", text: name === "echo" ? message : `unknown tool: ${name}` }],
        isError: name !== "echo",
      },
    });
    console.log(`→ mcp tools/call ${name}`);
    return;
  }

  reply(ws, requestId, 200, {
    jsonrpc: "2.0",
    id,
    error: { code: -32601, message: "Method not found" },
  });
}

type SavedIdentity = {
  version: number;
  privateKey: JsonWebKey;
  publicKey: JsonWebKey;
};

async function loadOrCreateIdentity(): Promise<{
  privateKey: CryptoKey;
  publicKeyRaw: Uint8Array;
  digest: string;
  publicKeyB64: string;
}> {
  if (existsSync(KEY_PATH)) {
    try {
      const saved = JSON.parse(readFileSync(KEY_PATH, "utf8")) as SavedIdentity;
      if (saved.version === IDENTITY_VERSION && saved.privateKey?.crv === "P-256") {
        const privateKey = await crypto.subtle.importKey(
          "jwk",
          saved.privateKey,
          { name: "ECDSA", namedCurve: "P-256" },
          true,
          ["sign"],
        );
        const publicKey = await crypto.subtle.importKey(
          "jwk",
          saved.publicKey,
          { name: "ECDSA", namedCurve: "P-256" },
          true,
          ["verify"],
        );
        const raw = new Uint8Array((await crypto.subtle.exportKey("raw", publicKey)) as ArrayBuffer);
        if (raw.length === PUBLIC_KEY_BYTES && raw[0] === 0x04) {
          const digest = await publicKeyDigest(raw);
          if (digest !== null) {
            return { privateKey, publicKeyRaw: raw, digest, publicKeyB64: bytesToBase64Url(raw) };
          }
        }
      }
    } catch {
      // fall through and regenerate
    }
    unlinkSync(KEY_PATH);
  }

  const keyPair = (await crypto.subtle.generateKey({ name: "ECDSA", namedCurve: "P-256" }, true, [
    "sign",
    "verify",
  ])) as CryptoKeyPair;
  const privateJwk = await crypto.subtle.exportKey("jwk", keyPair.privateKey);
  const publicJwk = await crypto.subtle.exportKey("jwk", keyPair.publicKey);
  mkdirSync(dirname(KEY_PATH), { recursive: true });
  writeFileSync(
    KEY_PATH,
    JSON.stringify({ version: IDENTITY_VERSION, privateKey: privateJwk, publicKey: publicJwk }, null, 2),
  );
  const raw = new Uint8Array((await crypto.subtle.exportKey("raw", keyPair.publicKey)) as ArrayBuffer);
  const digest = await publicKeyDigest(raw);
  if (digest === null || raw.length !== PUBLIC_KEY_BYTES) {
    throw new Error("failed to create ECDSA fake-device identity");
  }
  return {
    privateKey: keyPair.privateKey,
    publicKeyRaw: raw,
    digest,
    publicKeyB64: bytesToBase64Url(raw),
  };
}

async function main(): Promise<void> {
  const identity = await loadOrCreateIdentity();
  const { privateKey, digest, publicKeyB64 } = identity;

  const wsUrl = CLOUD.replace(/^http/, "ws") + `/cloud/device/${digest}`;
  console.log(`digest: ${digest}`);
  console.log(`mcp: ${CLOUD}/device/${digest}/mcp`);
  console.log(`landing: ${CLOUD}/cloud/landing/${digest}/page`);
  console.log(
    `pairing: ${CLOUD}/cloud/oauth/redirect?devicePublicKeyDigest=${digest}&redirect_uri=${encodeURIComponent("http://127.0.0.1:9999/cb")}&state=test`,
  );
  console.log(`connecting ${wsUrl}`);

  const connect = (): void => {
    const ws = new WebSocket(wsUrl);
    const heartbeat = setInterval(() => { if (ws.readyState === WebSocket.OPEN) ws.send("ping"); }, 10_000);
    ws.addEventListener("open", () => console.log("ws open"));
    ws.addEventListener("close", (e) => {
      clearInterval(heartbeat);
      console.log(`ws close code=${e.code} reason=${e.reason}; reconnecting in 1s`);
      setTimeout(connect, 1000);
    });
    ws.addEventListener("error", () => console.error("ws error"));

    ws.addEventListener("message", async (event) => {
      if (event.data === "pong") return;
      let msg: Record<string, unknown>;
      try {
        msg = JSON.parse(String(event.data)) as Record<string, unknown>;
      } catch {
        console.warn("non-json frame");
        return;
      }

      if (msg.type === "authRequest" && typeof msg.challenge === "string") {
        const challenge = base64UrlToBytes(msg.challenge);
        const signature = await crypto.subtle.sign(
          { name: "ECDSA", hash: "SHA-256" },
          privateKey,
          authSignedMessage(challenge),
        );
        if (signature.byteLength !== SIGNATURE_BYTES) {
          console.error(`unexpected signature length ${signature.byteLength}`);
          return;
        }
        ws.send(
          JSON.stringify({
            type: "authResponse",
            devicePublicKey: publicKeyB64,
            devicePublicKeyDigest: digest,
            version: "fake-device-2",
            response: bytesToBase64Url(signature),
          }),
        );
        return;
      }

      if (msg.type === "authResult") {
        console.log(`authResult success=${msg.success}${msg.reason ? ` reason=${msg.reason}` : ""}`);
        return;
      }

      if (msg.type === "request" && typeof msg.requestId === "string" && typeof msg.path === "string") {
        const path = msg.path.split("?")[0] ?? msg.path;
        const method = typeof msg.method === "string" ? msg.method : "GET";
        console.log(`← ${method} ${msg.path}`);

        if (path === "/pairing/session") {
          const expiredAt = Math.floor(Date.now() / 1000) + 30;
          const sessionToken = `sess-${crypto.randomUUID()}`;
          reply(ws, msg.requestId, 200, { sessionToken, expiredAt });
          console.log(`→ sessionToken ${sessionToken} expiredAt=${expiredAt}`);
          return;
        }
        if (path === "/pairing/token") {
          await Bun.sleep(800);
          const grantSecret = `grant-${crypto.randomUUID()}`;
          reply(ws, msg.requestId, 200, { grantSecret });
          console.log(`→ grantSecret ${grantSecret}`);
          return;
        }
        if (path === "/mcp") {
          const rawBody = msg.body === null ? "" : JSON.stringify(msg.body);
          console.log(`  body: ${rawBody.slice(0, 200)}${rawBody.length > 200 ? "…" : ""}`);
          handleMcp(ws, msg.requestId, method, rawBody);
          return;
        }

        reply(ws, msg.requestId, 200, { ok: true, path: msg.path, method });
      }
    });
  };

  connect();
}

await main();
