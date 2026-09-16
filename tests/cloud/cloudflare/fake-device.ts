/**
 * Local fake device for pairing / proxy / MCP testing.
 * Usage: bun tests/cloud/cloudflare/fake-device.ts [cloudBaseUrl]
 * Identity is persisted in tests/cloud/cloudflare/.fake-device-key.json so the digest stays stable.
 */
import { mkdirSync, readFileSync, writeFileSync, existsSync } from "node:fs";
import { dirname, join } from "node:path";

const CLOUD = (process.argv[2] ?? "http://127.0.0.1:8787").replace(/\/$/, "");
const PSS_SALT_LENGTH = 32;
const KEY_PATH = join(import.meta.dir, ".fake-device-key.json");

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

function bytesToHex(bytes: ArrayBuffer | Uint8Array): string {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let out = "";
  for (let i = 0; i < view.length; i += 1) {
    out += view[i]!.toString(16).padStart(2, "0");
  }
  return out;
}

async function sha256Hex(bytes: ArrayBuffer | Uint8Array): Promise<string> {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  return bytesToHex(await crypto.subtle.digest("SHA-256", view));
}

function reply(
  ws: WebSocket,
  requestId: string,
  status: number,
  body: unknown,
  headers: Record<string, string> = { "content-type": "application/json" },
): void {
  const payload = typeof body === "string" ? body : JSON.stringify(body);
  ws.send(
    JSON.stringify({
      type: "response",
      requestId,
      status,
      headers,
      body: bytesToBase64Url(new TextEncoder().encode(payload)),
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

async function loadOrCreateIdentity(): Promise<{
  privateKey: CryptoKey;
  publicKeySpki: Uint8Array;
  digest: string;
  publicKeyB64: string;
}> {
  if (existsSync(KEY_PATH)) {
    const saved = JSON.parse(readFileSync(KEY_PATH, "utf8")) as { privateKey: JsonWebKey; publicKey: JsonWebKey };
    const privateKey = await crypto.subtle.importKey(
      "jwk",
      saved.privateKey,
      { name: "RSA-PSS", hash: "SHA-256" },
      true,
      ["sign"],
    );
    const publicKey = await crypto.subtle.importKey(
      "jwk",
      saved.publicKey,
      { name: "RSA-PSS", hash: "SHA-256" },
      true,
      ["verify"],
    );
    const spki = new Uint8Array(await crypto.subtle.exportKey("spki", publicKey));
    const digest = await sha256Hex(spki);
    return { privateKey, publicKeySpki: spki, digest, publicKeyB64: bytesToBase64Url(spki) };
  }

  const keyPair = (await crypto.subtle.generateKey(
    {
      name: "RSA-PSS",
      modulusLength: 3072,
      publicExponent: new Uint8Array([1, 0, 1]),
      hash: "SHA-256",
    },
    true,
    ["sign", "verify"],
  )) as CryptoKeyPair;
  const privateJwk = await crypto.subtle.exportKey("jwk", keyPair.privateKey);
  const publicJwk = await crypto.subtle.exportKey("jwk", keyPair.publicKey);
  mkdirSync(dirname(KEY_PATH), { recursive: true });
  writeFileSync(KEY_PATH, JSON.stringify({ privateKey: privateJwk, publicKey: publicJwk }, null, 2));
  const spki = new Uint8Array(await crypto.subtle.exportKey("spki", keyPair.publicKey));
  const digest = await sha256Hex(spki);
  return {
    privateKey: keyPair.privateKey,
    publicKeySpki: spki,
    digest,
    publicKeyB64: bytesToBase64Url(spki),
  };
}

async function main(): Promise<void> {
  const identity = await loadOrCreateIdentity();
  const { privateKey, digest, publicKeyB64 } = identity;

  const wsUrl = CLOUD.replace(/^http/, "ws") + `/cloud/device/${digest}`;
  console.log(`digest: ${digest}`);
  console.log(`mcp: ${CLOUD}/device/${digest}/mcp`);
  console.log(`pairing: ${CLOUD}/cloud/oauth/redirect?devicePublicKeyDigest=${digest}&redirect_uri=${encodeURIComponent("http://127.0.0.1:9999/cb")}&state=test`);
  console.log(`connecting ${wsUrl}`);

  const connect = (): void => {
    const ws = new WebSocket(wsUrl);
    ws.addEventListener("open", () => console.log("ws open"));
    ws.addEventListener("close", (e) => {
      console.log(`ws close code=${e.code} reason=${e.reason}; reconnecting in 1s`);
      setTimeout(connect, 1000);
    });
    ws.addEventListener("error", () => console.error("ws error"));

    ws.addEventListener("message", async (event) => {
      let msg: Record<string, unknown>;
      try {
        msg = JSON.parse(String(event.data)) as Record<string, unknown>;
      } catch {
        console.warn("non-json frame");
        return;
      }

      if (msg.type === "authRequest" && typeof msg.challenge === "string") {
        const signature = await crypto.subtle.sign(
          { name: "RSA-PSS", saltLength: PSS_SALT_LENGTH },
          privateKey,
          base64UrlToBytes(msg.challenge),
        );
        ws.send(
          JSON.stringify({
            type: "authResponse",
            devicePublicKey: publicKeyB64,
            devicePublicKeyDigest: digest,
            version: "fake-device-1",
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
          const rawBody =
            typeof msg.body === "string" && msg.body.length > 0
              ? new TextDecoder().decode(base64UrlToBytes(msg.body))
              : "";
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
