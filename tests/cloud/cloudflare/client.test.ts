import { afterAll, beforeAll, describe, expect, test } from "bun:test";
import { spawn, type Subprocess } from "bun";
import { join } from "node:path";
import { authSignedMessage, publicKeyDigest } from "../../../cloud/cloudflare/src/crypto.ts";
import { PUBLIC_KEY_BYTES, SIGNATURE_BYTES } from "../../../cloud/cloudflare/src/protocol.ts";

const PACKAGE_DIR = join(import.meta.dir, "../../../cloud/cloudflare");
const BASE_PORT = 8787 + Math.floor(Math.random() * 1000);
const ROUTING_TOKEN_SECRET = "local-dev-routing-token-secret";
const ACCESS_TOKEN_EXPIRES_IN = 315_360_000;

/** Stable placeholder digests for offline/oauth path tests (Base58Check of SHA-256(label)[0..19]). */
const PLACEHOLDER = {
  a: "KUCzSr49wPWckUDouJLybJuRYtVpMajPX",
  b: "6fZuj9x4tozLd6CAQ7AhTLd9RYXHqq42x",
  c: "5Ep2vW3TjZVpdvmFFvw3XrEAALpNvLNAN",
  d: "3FTZKk18itEyDpnPvCcUFTYsQ74pm1MFy",
  e: "6ndQCSYG2zEb281o2AEhr2g2ssidiANCr",
  zero: "9kD1gZjgzuP8KuQw8fKTm9hoNuqKvtq3b",
} as const;

type AuthRequestMessage = {
  type: "authRequest";
  challenge: string;
};

type AuthResultMessage = {
  type: "authResult";
  success: boolean;
  reason?: string;
};

type RequestMessage = {
  type: "request";
  requestId: string;
  method: string;
  path: string;
  headers: Record<string, string>;
  body: Record<string, unknown> | null;
  grantSecret?: string;
};

type PairingSessionRequestMessage = {
  type: "pairingSessionRequest";
  name: string;
};

type DeviceInbound =
  | AuthRequestMessage
  | AuthResultMessage
  | RequestMessage
  | PairingSessionRequestMessage;

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

async function sealRoutingToken(
  secret: string,
  payload: { devicePublicKeyDigest: string; grantSecret: string },
): Promise<string> {
  const keyMaterial = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(secret));
  const key = await crypto.subtle.importKey("raw", keyMaterial, { name: "AES-GCM" }, false, ["encrypt"]);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const plaintext = new TextEncoder().encode(JSON.stringify(payload));
  const ciphertext = new Uint8Array(await crypto.subtle.encrypt({ name: "AES-GCM", iv }, key, plaintext));
  const packed = new Uint8Array(iv.length + ciphertext.length);
  packed.set(iv, 0);
  packed.set(ciphertext, iv.length);
  return bytesToBase64Url(packed);
}

async function openRoutingToken(
  secret: string,
  token: string,
): Promise<{ devicePublicKeyDigest: string; grantSecret: string } | null> {
  const packed = base64UrlToBytes(token);
  if (packed.length <= 12) {
    return null;
  }
  const iv = packed.subarray(0, 12);
  const ciphertext = packed.subarray(12);
  try {
    const keyMaterial = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(secret));
    const key = await crypto.subtle.importKey("raw", keyMaterial, { name: "AES-GCM" }, false, ["decrypt"]);
    const plaintext = await crypto.subtle.decrypt({ name: "AES-GCM", iv }, key, ciphertext);
    const parsed = JSON.parse(new TextDecoder().decode(plaintext)) as {
      devicePublicKeyDigest?: string;
      grantSecret?: string;
    };
    if (typeof parsed.devicePublicKeyDigest !== "string" || typeof parsed.grantSecret !== "string") {
      return null;
    }
    return { devicePublicKeyDigest: parsed.devicePublicKeyDigest, grantSecret: parsed.grantSecret };
  } catch {
    return null;
  }
}

async function bearerFor(digest: string, grantSecret = "grant-secret"): Promise<string> {
  return sealRoutingToken(ROUTING_TOKEN_SECRET, { devicePublicKeyDigest: digest, grantSecret });
}

async function generateDeviceIdentity(): Promise<{
  privateKey: CryptoKey;
  publicKeyRaw: Uint8Array;
  digest: string;
  publicKeyB64: string;
}> {
  const keyPair = (await crypto.subtle.generateKey({ name: "ECDSA", namedCurve: "P-256" }, true, [
    "sign",
    "verify",
  ])) as CryptoKeyPair;
  const raw = new Uint8Array((await crypto.subtle.exportKey("raw", keyPair.publicKey)) as ArrayBuffer);
  expect(raw.length).toBe(PUBLIC_KEY_BYTES);
  expect(raw[0]).toBe(0x04);
  const digest = await publicKeyDigest(raw);
  if (digest === null) {
    throw new Error("failed to compute public key digest");
  }
  return {
    privateKey: keyPair.privateKey,
    publicKeyRaw: raw,
    digest,
    publicKeyB64: bytesToBase64Url(raw),
  };
}

async function signChallenge(privateKey: CryptoKey, challengeB64: string): Promise<string> {
  const challenge = base64UrlToBytes(challengeB64);
  const signature = await crypto.subtle.sign(
    { name: "ECDSA", hash: "SHA-256" },
    privateKey,
    authSignedMessage(challenge),
  );
  expect(signature.byteLength).toBe(SIGNATURE_BYTES);
  return bytesToBase64Url(signature);
}

function waitForMessage<T extends DeviceInbound>(
  ws: WebSocket,
  predicate: (msg: DeviceInbound) => msg is T,
  timeoutMs = 10_000,
): Promise<T> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      cleanup();
      reject(new Error("timed out waiting for websocket message"));
    }, timeoutMs);

    const onMessage = (event: MessageEvent) => {
      let parsed: DeviceInbound;
      try {
        parsed = JSON.parse(String(event.data)) as DeviceInbound;
      } catch {
        return;
      }
      if (!predicate(parsed)) {
        return;
      }
      cleanup();
      resolve(parsed);
    };

    const onClose = () => {
      cleanup();
      reject(new Error("websocket closed while waiting for message"));
    };

    const cleanup = () => {
      clearTimeout(timer);
      ws.removeEventListener("message", onMessage);
      ws.removeEventListener("close", onClose);
    };

    ws.addEventListener("message", onMessage);
    ws.addEventListener("close", onClose);
  });
}

async function openDeviceSocket(baseUrl: string, digest: string): Promise<WebSocket> {
  const wsUrl = baseUrl.replace(/^http/, "ws") + `/cloud/device/${digest}`;
  const ws = new WebSocket(wsUrl);
  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("websocket open timeout")), 15_000);
    ws.addEventListener("open", () => {
      clearTimeout(timer);
      resolve();
    });
    ws.addEventListener("error", () => {
      clearTimeout(timer);
      reject(new Error("websocket open failed"));
    });
  });
  return ws;
}

async function authenticateDevice(
  ws: WebSocket,
  identity: { privateKey: CryptoKey; digest: string; publicKeyB64: string },
  overrides?: { digest?: string; publicKeyB64?: string; response?: string; version?: string },
): Promise<AuthResultMessage> {
  const authRequest = await waitForMessage(ws, (msg): msg is AuthRequestMessage => msg.type === "authRequest");
  const response = overrides?.response ?? (await signChallenge(identity.privateKey, authRequest.challenge));
  ws.send(
    JSON.stringify({
      type: "authResponse",
      devicePublicKey: overrides?.publicKeyB64 ?? identity.publicKeyB64,
      devicePublicKeyDigest: overrides?.digest ?? identity.digest,
      version: overrides?.version ?? "test-1.0.0",
      response,
    }),
  );
  return waitForMessage(ws, (msg): msg is AuthResultMessage => msg.type === "authResult");
}

async function waitForReady(baseUrl: string, timeoutMs = 60_000): Promise<void> {
  const started = Date.now();
  while (Date.now() - started < timeoutMs) {
    try {
      const res = await fetch(`${baseUrl}/__ready_probe__`);
      // Any HTTP response means the worker is listening.
      if (res.status === 404 || res.ok) {
        return;
      }
    } catch {
      // retry
    }
    await Bun.sleep(200);
  }
  throw new Error(`worker did not become ready at ${baseUrl}`);
}

function replyJson(ws: WebSocket, requestId: string, status: number, body: unknown): void {
  ws.send(
    JSON.stringify({
      type: "response",
      requestId,
      status,
      headers: { "content-type": "application/json" },
      body,
    }),
  );
}

describe("cloudflare device proxy e2e", () => {
  let proc: Subprocess | null = null;
  let baseUrl = "";
  let identity: Awaited<ReturnType<typeof generateDeviceIdentity>>;

  beforeAll(async () => {
    identity = await generateDeviceIdentity();
    const port = BASE_PORT;
    baseUrl = `http://127.0.0.1:${port}`;

    proc = spawn({
      cmd: [
        "bunx",
        "wrangler",
        "dev",
        "--config",
        "wrangler.test.jsonc",
        "--port",
        String(port),
        "--ip",
        "127.0.0.1",
        "--local",
        "--persist-to",
        `.wrangler/e2e-${port}`,
      ],
      cwd: PACKAGE_DIR,
      stdout: "pipe",
      stderr: "pipe",
      env: {
        ...process.env,
        WRANGLER_SEND_METRICS: "false",
      },
    });

    await waitForReady(baseUrl);
  }, 120_000);

  afterAll(async () => {
    if (proc) {
      proc.kill();
      await proc.exited;
      proc = null;
    }
  });

  test("serves oauth well-known and static register", async () => {
    const wellKnown = await fetch(`${baseUrl}/.well-known/oauth-authorization-server`);
    expect(wellKnown.status).toBe(200);
    const meta = (await wellKnown.json()) as Record<string, unknown>;
    expect(meta.authorization_endpoint).toBe(`${baseUrl}/cloud/oauth/redirect`);
    expect(meta.token_endpoint).toBe(`${baseUrl}/token`);
    expect(meta.registration_endpoint).toBe(`${baseUrl}/register`);
    expect(meta.code_challenge_methods_supported).toEqual(["S256"]);

    const register = await fetch(`${baseUrl}/register`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        client_name: "Cursor",
        redirect_uris: ["http://127.0.0.1:54321/callback", "cursor://anysphere.cursor-mcp/oauth/callback"],
        grant_types: ["authorization_code"],
        token_endpoint_auth_method: "none",
      }),
    });
    expect(register.status).toBe(201);
    const client = (await register.json()) as {
      client_id: string;
      token_endpoint_auth_method: string;
      redirect_uris: string[];
      client_name: string;
    };
    expect(client.client_id).toBe("saihub-static-client");
    expect(client.token_endpoint_auth_method).toBe("none");
    expect(client.redirect_uris).toEqual([
      "http://127.0.0.1:54321/callback",
      "cursor://anysphere.cursor-mcp/oauth/callback",
    ]);
    expect(client.client_name).toBe("Cursor");
  }, 30_000);

  test("mcp 401 includes protected resource metadata", async () => {
    const digest = PLACEHOLDER.d;
    const res = await fetch(`${baseUrl}/device/${digest}/mcp`, { method: "POST" });
    expect(res.status).toBe(401);
    const www = res.headers.get("www-authenticate") ?? "";
    expect(www).toContain("resource_metadata=");
    expect(www).toContain(`/.well-known/oauth-protected-resource/device/${digest}/mcp`);

    const metaRes = await fetch(`${baseUrl}/.well-known/oauth-protected-resource/device/${digest}/mcp`);
    expect(metaRes.status).toBe(200);
    const meta = (await metaRes.json()) as { resource: string; authorization_servers: string[] };
    expect(meta.resource).toBe(`${baseUrl}/device/${digest}/mcp`);
    expect(meta.authorization_servers).toEqual([baseUrl]);
  }, 30_000);

  test("oauth redirect accepts digest from resource param", async () => {
    const digest = PLACEHOLDER.e;
    const resource = `${baseUrl}/device/${digest}/mcp`;
    const ok = await fetch(
      `${baseUrl}/cloud/oauth/redirect?resource=${encodeURIComponent(resource)}&redirect_uri=${encodeURIComponent("https://client.example/cb")}&state=xyz`,
    );
    expect(ok.status).toBe(200);
    expect(await ok.text()).toContain(digest);
  }, 30_000);

  test("landing page and online status do not require a bearer token", async () => {
    const digest = PLACEHOLDER.a;
    const missing = await fetch(`${baseUrl}/cloud/landing/${digest}/page`, { method: "POST" });
    expect(missing.status).toBe(405);

    const page = await fetch(`${baseUrl}/cloud/landing/${digest}/page`);
    expect(page.status).toBe(200);
    expect(page.headers.get("content-type") ?? "").toContain("text/html");
    const html = await page.text();
    expect(html).toContain(`${baseUrl}/device/${digest}/mcp`);
    expect(html).toContain(`${baseUrl}/device/${digest}/openapi.json`);
    expect(html).toContain(`value="${baseUrl}/device/${digest}"`);
    expect(html).toContain(`/cloud/landing/${digest}/online`);

    const offline = await fetch(`${baseUrl}/cloud/landing/${digest}/online`);
    expect(offline.status).toBe(200);
    expect(offline.headers.get("cache-control")).toBe("no-store");
    expect((await offline.json()) as { online: boolean }).toEqual({ online: false });

    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    try {
      const authRequest = await waitForMessage(ws, (msg): msg is AuthRequestMessage => msg.type === "authRequest");
      expect((await (await fetch(`${baseUrl}/cloud/landing/${other.digest}/online`)).json()) as { online: boolean }).toEqual({ online: false });
      ws.send(JSON.stringify({
        type: "authResponse",
        devicePublicKey: other.publicKeyB64,
        devicePublicKeyDigest: other.digest,
        version: "test-1.0.0",
        response: await signChallenge(other.privateKey, authRequest.challenge),
      }));
      expect((await waitForMessage(ws, (msg): msg is AuthResultMessage => msg.type === "authResult")).success).toBe(true);
      expect((await (await fetch(`${baseUrl}/cloud/landing/${other.digest}/online`)).json()) as { online: boolean }).toEqual({ online: true });
    } finally {
      ws.close();
    }
    await Bun.sleep(300);
    expect((await (await fetch(`${baseUrl}/cloud/landing/${other.digest}/online`)).json()) as { online: boolean }).toEqual({ online: false });
  }, 60_000);

  test("oauth redirect page requires digest and redirect_uri", async () => {
    const missing = await fetch(`${baseUrl}/cloud/oauth/redirect`);
    expect(missing.status).toBe(400);

    const noRedirect = await fetch(`${baseUrl}/cloud/oauth/redirect?devicePublicKeyDigest=${PLACEHOLDER.a}`);
    expect(noRedirect.status).toBe(400);

    const ok = await fetch(
      `${baseUrl}/cloud/oauth/redirect?devicePublicKeyDigest=${PLACEHOLDER.a}&redirect_uri=${encodeURIComponent("https://client.example/cb")}&state=xyz`,
    );
    expect(ok.status).toBe(200);
    expect(ok.headers.get("content-type") ?? "").toContain("text/html");
    const html = await ok.text();
    expect(html).toContain("Name this client");
    expect(html).toContain("Press button for 3 seconds.");
  }, 30_000);

  test("token endpoint exchanges routingToken code", async () => {
    const code = await bearerFor(identity.digest, "grant-from-code");
    const res = await fetch(`${baseUrl}/token`, {
      method: "POST",
      headers: { "content-type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams({
        grant_type: "authorization_code",
        code,
        code_verifier: "ignored",
      }),
    });
    expect(res.status).toBe(200);
    const body = (await res.json()) as { access_token: string; token_type: string; expires_in: number };
    expect(body.access_token).toBe(code);
    expect(body.token_type).toBe("Bearer");
    expect(body.expires_in).toBe(ACCESS_TOKEN_EXPIRES_IN);

    const bad = await fetch(`${baseUrl}/token`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ grant_type: "authorization_code", code: "not-a-token" }),
    });
    expect(bad.status).toBe(400);
    expect(((await bad.json()) as { error: string }).error).toBe("invalid_grant");
  }, 30_000);

  test("pairing session requires name", async () => {
    const res = await fetch(`${baseUrl}/cloud/pairing/session`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: identity.digest }),
    });
    expect(res.status).toBe(400);
    expect(((await res.json()) as { reason: string }).reason).toBe("invalid name");
  }, 30_000);

  test("pairing session and token mint routingToken without leaking grantSecret", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);

    const sessionPending = waitForMessage(ws, (msg): msg is PairingSessionRequestMessage =>
      msg.type === "pairingSessionRequest");
    const sessionPromise = fetch(`${baseUrl}/cloud/pairing/session`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, name: "Cursor" }),
    });
    const sessionReq = await sessionPending;
    expect(sessionReq.name).toBe("Cursor");
    expect("requestId" in sessionReq).toBe(false);
    const expiredAt = Math.floor(Date.now() / 1000) + 30;
    ws.send(JSON.stringify({
      type: "pairingSessionResponse", success: true, sessionToken: "sess-1", expiredAt,
    }));

    const sessionRes = await sessionPromise;
    expect(sessionRes.status).toBe(200);
    const sessionBody = (await sessionRes.json()) as { sessionToken: string; expiredAt: number };
    expect(sessionBody.sessionToken).toBe("sess-1");
    expect(sessionBody.expiredAt).toBe(expiredAt);

    const inbound: DeviceInbound[] = [];
    const onMessage = (event: MessageEvent) => {
      try {
        inbound.push(JSON.parse(String(event.data)) as DeviceInbound);
      } catch {
        // ignore
      }
    };
    ws.addEventListener("message", onMessage);
    const tokenPromise = fetch(`${baseUrl}/cloud/pairing/token`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, sessionToken: "sess-1" }),
    });
    await Bun.sleep(200);
    expect(inbound.some((msg) => (msg as { type: string }).type === "pairingSessionTokenRequest")).toBe(false);
    ws.send(JSON.stringify({
      type: "pairingSessionTokenResponse", success: true, grantSecret: "abcdefghijklmnopqrstuvwx012345",
    }));

    const tokenRes = await tokenPromise;
    ws.removeEventListener("message", onMessage);
    expect(tokenRes.status).toBe(200);
    const tokenBody = (await tokenRes.json()) as Record<string, unknown>;
    expect(tokenBody.grantSecret).toBeUndefined();
    expect(typeof tokenBody.routingToken).toBe("string");
    const opened = await openRoutingToken(ROUTING_TOKEN_SECRET, tokenBody.routingToken as string);
    expect(opened).toEqual({ devicePublicKeyDigest: other.digest, grantSecret: "abcdefghijklmnopqrstuvwx012345" });

    ws.close();
  }, 60_000);

  test("pairing token validates sessionToken on the relay", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);

    const missing = await fetch(`${baseUrl}/cloud/pairing/token`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, sessionToken: "sess-1" }),
    });
    expect(missing.status).toBe(401);
    expect(((await missing.json()) as { reason: string }).reason).toBe("session token is invalid or expired");

    const sessionPending = waitForMessage(ws, (msg): msg is PairingSessionRequestMessage =>
      msg.type === "pairingSessionRequest");
    const sessionPromise = fetch(`${baseUrl}/cloud/pairing/session`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, name: "Cursor" }),
    });
    await sessionPending;
    ws.send(JSON.stringify({
      type: "pairingSessionResponse", success: true, sessionToken: "sess-1",
      expiredAt: Math.floor(Date.now() / 1000) + 30,
    }));
    expect((await sessionPromise).status).toBe(200);

    const inbound: DeviceInbound[] = [];
    const onMessage = (event: MessageEvent) => {
      try {
        inbound.push(JSON.parse(String(event.data)) as DeviceInbound);
      } catch {
        // ignore
      }
    };
    ws.addEventListener("message", onMessage);
    const mismatch = await fetch(`${baseUrl}/cloud/pairing/token`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, sessionToken: "wrong" }),
    });
    expect(mismatch.status).toBe(401);
    expect(((await mismatch.json()) as { reason: string }).reason).toBe("session token is invalid or expired");
    await Bun.sleep(150);
    expect(inbound).toEqual([]);
    ws.removeEventListener("message", onMessage);
    ws.close();
  }, 60_000);

  test("pairing session maps conflict and grant limit", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);

    const conflictPending = waitForMessage(ws, (msg): msg is PairingSessionRequestMessage =>
      msg.type === "pairingSessionRequest");
    const conflictPromise = fetch(`${baseUrl}/cloud/pairing/session`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, name: "A" }),
    });
    await conflictPending;
    ws.send(JSON.stringify({
      type: "pairingSessionResponse", success: false,
      reason: "a pairing session is already active",
    }));
    const conflictRes = await conflictPromise;
    expect(conflictRes.status).toBe(409);

    const fullPending = waitForMessage(ws, (msg): msg is PairingSessionRequestMessage =>
      msg.type === "pairingSessionRequest");
    const fullPromise = fetch(`${baseUrl}/cloud/pairing/session`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, name: "B" }),
    });
    await fullPending;
    ws.send(JSON.stringify({
      type: "pairingSessionResponse", success: false,
      reason: "grant secret limit reached (16)",
    }));
    const fullRes = await fullPromise;
    expect(fullRes.status).toBe(422);
    ws.close();
  }, 60_000);

  test("pairing allows concurrent token requests", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);

    const sessionPending = waitForMessage(ws, (msg): msg is PairingSessionRequestMessage =>
      msg.type === "pairingSessionRequest");
    const sessionPromise = fetch(`${baseUrl}/cloud/pairing/session`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, name: "Cursor" }),
    });
    await sessionPending;
    ws.send(JSON.stringify({
      type: "pairingSessionResponse", success: true, sessionToken: "sess-1",
      expiredAt: Math.floor(Date.now() / 1000) + 30,
    }));
    expect((await sessionPromise).status).toBe(200);

    const firstPromise = fetch(`${baseUrl}/cloud/pairing/token`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, sessionToken: "sess-1" }),
    });
    const secondPromise = fetch(`${baseUrl}/cloud/pairing/token`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, sessionToken: "sess-1" }),
    });
    await Bun.sleep(200);
    const grantSecret = "abcdefghijklmnopqrstuvwx012345";
    ws.send(JSON.stringify({
      type: "pairingSessionTokenResponse", success: true, grantSecret,
    }));
    const [firstRes, secondRes] = await Promise.all([firstPromise, secondPromise]);
    expect(firstRes.status).toBe(200);
    expect(secondRes.status).toBe(200);
    const firstOpened = await openRoutingToken(
      ROUTING_TOKEN_SECRET, ((await firstRes.json()) as { routingToken: string }).routingToken,
    );
    const secondOpened = await openRoutingToken(
      ROUTING_TOKEN_SECRET, ((await secondRes.json()) as { routingToken: string }).routingToken,
    );
    expect(firstOpened?.grantSecret).toBe(grantSecret);
    expect(secondOpened?.grantSecret).toBe(grantSecret);

    const retry = await fetch(`${baseUrl}/cloud/pairing/token`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, sessionToken: "sess-1" }),
    });
    expect(retry.status).toBe(200);
    const retryOpened = await openRoutingToken(
      ROUTING_TOKEN_SECRET, ((await retry.json()) as { routingToken: string }).routingToken,
    );
    expect(retryOpened?.grantSecret).toBe(grantSecret);
    ws.close();
  }, 60_000);

  test("pairing token rejects expired sessionToken", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);

    const sessionPending = waitForMessage(ws, (msg): msg is PairingSessionRequestMessage =>
      msg.type === "pairingSessionRequest");
    const sessionPromise = fetch(`${baseUrl}/cloud/pairing/session`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, name: "Cursor" }),
    });
    await sessionPending;
    ws.send(JSON.stringify({
      type: "pairingSessionResponse", success: true, sessionToken: "sess-1",
      expiredAt: Math.floor(Date.now() / 1000) - 1,
    }));
    expect((await sessionPromise).status).toBe(200);

    const tokenRes = await fetch(`${baseUrl}/cloud/pairing/token`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, sessionToken: "sess-1" }),
    });
    expect(tokenRes.status).toBe(401);
    expect(((await tokenRes.json()) as { reason: string }).reason).toBe("session token is invalid or expired");
    ws.close();
  }, 60_000);

  test("pairing token uses grant sent by the device without a token request", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);

    const sessionPending = waitForMessage(ws, (msg): msg is PairingSessionRequestMessage =>
      msg.type === "pairingSessionRequest");
    const sessionPromise = fetch(`${baseUrl}/cloud/pairing/session`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, name: "Cursor" }),
    });
    await sessionPending;
    ws.send(JSON.stringify({
      type: "pairingSessionResponse", success: true, sessionToken: "sess-1",
      expiredAt: Math.floor(Date.now() / 1000) + 30,
    }));
    expect((await sessionPromise).status).toBe(200);

    ws.send(JSON.stringify({
      type: "pairingSessionTokenResponse", success: true, grantSecret: "abcdefghijklmnopqrstuvwx012345",
    }));
    await Bun.sleep(200);

    const tokenRes = await fetch(`${baseUrl}/cloud/pairing/token`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: other.digest, sessionToken: "sess-1" }),
    });
    expect(tokenRes.status).toBe(200);
    const opened = await openRoutingToken(
      ROUTING_TOKEN_SECRET, ((await tokenRes.json()) as { routingToken: string }).routingToken,
    );
    expect(opened?.grantSecret).toBe("abcdefghijklmnopqrstuvwx012345");
    ws.close();
  }, 60_000);

  test("pairing session returns 503 when device offline", async () => {
    const digest = PLACEHOLDER.b;
    const res = await fetch(`${baseUrl}/cloud/pairing/session`, {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ devicePublicKeyDigest: digest, name: "Cursor" }),
    });
    expect(res.status).toBe(503);
  }, 30_000);

  test("device http requires bearer routing token", async () => {
    const missing = await fetch(`${baseUrl}/device/${identity.digest}/pin/1`);
    expect(missing.status).toBe(401);
    expect(missing.headers.get("www-authenticate") ?? "").toContain("Bearer");
    expect(((await missing.json()) as { reason: string }).reason).toContain("access token");

    const wrongDigest = await bearerFor(PLACEHOLDER.c);
    const mismatch = await fetch(`${baseUrl}/device/${identity.digest}/pin/1`, {
      headers: { authorization: `Bearer ${wrongDigest}` },
    });
    expect(mismatch.status).toBe(401);
    expect(mismatch.headers.get("www-authenticate") ?? "").toContain("Bearer");
  }, 30_000);

  test("automatically responds to device text heartbeats", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    try {
      expect((await authenticateDevice(ws, other)).success).toBe(true);
      const pong = new Promise<string>((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error("pong timeout")), 2000);
        ws.addEventListener("message", (event) => {
          clearTimeout(timer);
          resolve(String(event.data));
        }, { once: true });
      });
      ws.send("ping");
      expect(await pong).toBe("pong");
    } finally { ws.close(); }
  });

  test("authenticates device and forwards http request/response", async () => {
    const ws = await openDeviceSocket(baseUrl, identity.digest);
    const result = await authenticateDevice(ws, identity);
    expect(result.success).toBe(true);
    const token = await bearerFor(identity.digest);

    const pendingRequest = waitForMessage(ws, (msg): msg is RequestMessage => msg.type === "request");

    const httpPromise = fetch(`${baseUrl}/device/${identity.digest}/pin/1?mode=pwm`, {
      method: "PUT",
      headers: {
        "content-type": "application/json",
        accept: "application/json",
        "x-lock-id": "lock-abc",
        authorization: `Bearer ${token}`,
      },
      body: JSON.stringify({ duty: 0.5 }),
    });

    const deviceRequest = await pendingRequest;
    expect(deviceRequest.method).toBe("PUT");
    expect(deviceRequest.path).toBe("/pin/1?mode=pwm");
    expect(deviceRequest.requestId.length).toBeGreaterThan(0);
    expect(deviceRequest.headers["content-type"]).toBe("application/json");
    expect(deviceRequest.headers["accept"]).toBe("application/json");
    expect(deviceRequest.headers["x-lock-id"]).toBe("lock-abc");
    expect(deviceRequest.headers["authorization"]).toBeUndefined();
    expect(deviceRequest.grantSecret).toBe("grant-secret");

    expect(deviceRequest.body).toEqual({ duty: 0.5 });

    ws.send(
      JSON.stringify({
        type: "response",
        requestId: deviceRequest.requestId,
        status: 200,
        headers: { "content-type": "application/json", "x-lock-id": "lock-abc" },
        body: { ok: true, pin: 1 },
      }),
    );

    const httpResponse = await httpPromise;
    expect(httpResponse.status).toBe(200);
    expect(httpResponse.headers.get("content-type")).toBe("application/json");
    expect(httpResponse.headers.get("x-lock-id")).toBe("lock-abc");
    const payload = (await httpResponse.json()) as { ok: boolean; pin: number };
    expect(payload).toEqual({ ok: true, pin: 1 });

    ws.close();
  }, 60_000);

  test("forwards empty device response body", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);
    const token = await bearerFor(other.digest);

    const pending = waitForMessage(ws, (msg): msg is RequestMessage => msg.type === "request");
    const httpPromise = fetch(`${baseUrl}/device/${other.digest}/mcp`, {
      method: "POST",
      headers: {
        authorization: `Bearer ${token}`,
        "content-type": "application/json",
      },
      body: JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }),
    });
    const req = await pending;
    ws.send(
      JSON.stringify({
        type: "response",
        requestId: req.requestId,
        status: 202,
        headers: {},
        body: null,
      }),
    );
    const res = await httpPromise;
    expect(res.status).toBe(202);
    expect(await res.text()).toBe("");
    ws.close();
  }, 60_000);

  test("correlates concurrent out-of-order responses", async () => {
    const ws = await openDeviceSocket(baseUrl, identity.digest);
    try {
      expect((await authenticateDevice(ws, identity)).success).toBe(true);
      const token = await bearerFor(identity.digest);

      const requests: RequestMessage[] = [];
      const collect = new Promise<void>((resolve) => {
        const onMessage = (event: MessageEvent) => {
          const parsed = JSON.parse(String(event.data)) as DeviceInbound;
          if (parsed.type !== "request") {
            return;
          }
          requests.push(parsed);
          if (requests.length === 2) {
            ws.removeEventListener("message", onMessage);
            resolve();
          }
        };
        ws.addEventListener("message", onMessage);
      });

      const first = fetch(`${baseUrl}/device/${identity.digest}/first`, {
        method: "GET",
        headers: { authorization: `Bearer ${token}` },
      });
      const second = fetch(`${baseUrl}/device/${identity.digest}/second`, {
        method: "GET",
        headers: { authorization: `Bearer ${token}` },
      });
      await collect;

      const byPath = Object.fromEntries(requests.map((r) => [r.path, r]));
      const firstReq = byPath["/first"];
      const secondReq = byPath["/second"];
      expect(firstReq).toBeDefined();
      expect(secondReq).toBeDefined();
      expect(firstReq!.requestId).not.toBe(secondReq!.requestId);

      ws.send(
        JSON.stringify({
          type: "response",
          requestId: secondReq!.requestId,
          status: 201,
          headers: { "content-type": "application/json" },
          body: { order: "second" },
        }),
      );
      ws.send(
        JSON.stringify({
          type: "response",
          requestId: firstReq!.requestId,
          status: 200,
          headers: { "content-type": "application/json" },
          body: { order: "first" },
        }),
      );

      const [firstRes, secondRes] = await Promise.all([first, second]);
      expect(firstRes.status).toBe(200);
      expect(await firstRes.json() as unknown).toEqual({ order: "first" });
      expect(secondRes.status).toBe(201);
      expect(await secondRes.json() as unknown).toEqual({ order: "second" });
    } finally {
      ws.close();
      await Bun.sleep(300);
    }
  }, 60_000);

  test("forwards bodyless OPTIONS without synthesizing a text/plain content type", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);
    const token = await bearerFor(other.digest);
    try {
      const pending = waitForMessage(ws, (msg): msg is RequestMessage => msg.type === "request");
      const response = fetch(`${baseUrl}/device/${other.digest}/pin/`, {
        method: "OPTIONS",
        headers: { authorization: `Bearer ${token}` },
      });
      const request = await pending;
      expect(request.method).toBe("OPTIONS");
      expect(request.path).toBe("/pin/");
      expect(request.body).toBeNull();
      ws.send(JSON.stringify({
        type: "response", requestId: request.requestId, status: 204,
        headers: {}, body: null,
      }));
      const result = await response;
      expect(result.status).toBe(204);
      expect(await result.text()).toBe("");
    } finally {
      ws.close();
    }
  }, 30_000);

  test("forwards bodyless writes and GET query parameters", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);
    const token = await bearerFor(other.digest);
    try {
      for (const [method, path] of [["POST", "/uart/0/flush"], ["PUT", "/lock/test"], ["PATCH", "/pin/6"]] as const) {
        const pending = waitForMessage(ws, (msg): msg is RequestMessage => msg.type === "request");
        const response = fetch(`${baseUrl}/device/${other.digest}${path}`, {
          method, headers: { authorization: `Bearer ${token}` },
        });
        const request = await pending;
        expect(request.method).toBe(method);
        expect(request.path).toBe(path);
        expect(request.body).toBeNull();
        replyJson(ws, request.requestId, 200, { ok: true });
        expect((await response).status).toBe(200);
      }
      const pending = waitForMessage(ws, (msg): msg is RequestMessage => msg.type === "request");
      const response = fetch(`${baseUrl}/device/${other.digest}/pin/level?pins=6&pins=7`, {
        headers: { authorization: `Bearer ${token}` },
      });
      const request = await pending;
      expect(request.method).toBe("GET");
      expect(request.path).toBe("/pin/level?pins=6&pins=7");
      expect(request.body).toBeNull();
      expect(request.headers["content-type"]).toBeUndefined();
      replyJson(ws, request.requestId, 200, { levels: [0, 1] });
      const result = await response;
      expect(result.status).toBe(200);
      expect(await result.json() as unknown).toEqual({ levels: [0, 1] });

      const missingType = spawn(["curl", "--noproxy", "*", "--silent", "--show-error", "--max-time", "10",
        "--request", "POST", `${baseUrl}/device/${other.digest}/pin/level`,
        "--header", `Authorization: Bearer ${token}`, "--header", "Content-Type:",
        "--data-binary", "{}", "--write-out", "%{http_code}"], { stdout: "pipe", stderr: "pipe" });
      expect(await new Response(missingType.stdout).text()).toContain("415");
      expect(await missingType.exited).toBe(0);
    } finally { ws.close(); }
  }, 30_000);

  test("rejects non-JSON media and handles MCP SSE on the cloud", async () => {
    const token = await bearerFor(PLACEHOLDER.a);
    for (const contentType of ["text/plain", "application/jsonp", "application/octet-stream"]) {
      const res = await fetch(`${baseUrl}/device/${PLACEHOLDER.a}/pin/1`, {
        method: "PUT", headers: { authorization: `Bearer ${token}`, "content-type": contentType }, body: "{}",
      });
      expect(res.status).toBe(415);
      expect(await res.json() as unknown).toEqual({ reason: `cloud relay not support ${contentType} currently, use application/json instead` });
    }
    const sse = await fetch(`${baseUrl}/device/${PLACEHOLDER.a}/mcp`, {
      headers: { authorization: `Bearer ${token}`, accept: "text/event-stream" },
    });
    expect(sse.status).toBe(405);
    const post = await fetch(`${baseUrl}/device/${PLACEHOLDER.a}/mcp`, {
      method: "POST", headers: { authorization: `Bearer ${token}`, "content-type": "application/json", accept: "text/event-stream" }, body: "{}",
    });
    expect(post.status).toBe(406);
    expect(await post.json() as unknown).toEqual({ reason: "cloud relay not support text/event-stream currently, use application/json instead" });
  });

  test("validates JSON object bodies and preserves MCP headers", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);
    const token = await bearerFor(other.digest);
    try {
      for (const body of ["{", "[]", "null", '"base64"']) {
        const res = await fetch(`${baseUrl}/device/${other.digest}/mcp`, {
          method: "POST", headers: { authorization: `Bearer ${token}`, "content-type": "application/json" }, body,
        });
        expect(res.status).toBe(400);
      }
      const pending = waitForMessage(ws, (msg): msg is RequestMessage => msg.type === "request");
      const response = fetch(`${baseUrl}/device/${other.digest}/mcp`, {
        method: "POST", headers: {
          authorization: `Bearer ${token}`, "content-type": "Application/JSON; charset=utf-8",
          accept: "application/json, text/event-stream", "mcp-protocol-version": "2025-03-26", "mcp-session-id": "session-1",
        }, body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "tools/list" }),
      });
      const request = await pending;
      expect(request.body).toEqual({ jsonrpc: "2.0", id: 1, method: "tools/list" });
      expect(request.headers["mcp-protocol-version"]).toBe("2025-03-26");
      expect(request.headers["mcp-session-id"]).toBe("session-1");
      replyJson(ws, request.requestId, 200, { jsonrpc: "2.0", id: 1, result: {} });
      expect((await response).status).toBe(200);
    } finally { ws.close(); }
  });

  test("receives a fragmented JSON response larger than the old 64 KiB limit", async () => {
    const other = await generateDeviceIdentity();
    const device = spawn(["node", join(import.meta.dir, "fragment-device.mjs"), JSON.stringify({
      url: baseUrl.replace(/^http/, "ws"), digest: other.digest, publicKeyB64: other.publicKeyB64,
      privateKey: await crypto.subtle.exportKey("jwk", other.privateKey),
    })], { stdout: "pipe", stderr: "inherit" });
    const token = await bearerFor(other.digest);
    try {
      const reader = device.stdout.getReader();
      const ready = await reader.read();
      expect(new TextDecoder().decode(ready.value)).toContain("ready");
      reader.releaseLock();
      const response = fetch(`${baseUrl}/device/${other.digest}/pin/trace`, { headers: { authorization: `Bearer ${token}` } });
      const events = Array.from({ length: 2000 }, (_, i) => ({ pin: 1, level: i % 2, time: i, label: "测试" }));
      const res = await response;
      expect(res.status).toBe(200);
      expect(await res.json() as unknown).toEqual({ events });
    } finally { device.kill(); await device.exited; }
  });

  test("rejects non-JSON device responses and supports bodyless 204", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);
    const token = await bearerFor(other.digest);
    try {
      for (const status of [200, 204]) {
        const pending = waitForMessage(ws, (msg): msg is RequestMessage => msg.type === "request");
        const response = fetch(`${baseUrl}/device/${other.digest}/pin/1`, { headers: { authorization: `Bearer ${token}` } });
        const request = await pending;
        ws.send(JSON.stringify({ type: "response", requestId: request.requestId, status, headers: { "content-type": "text/plain" }, body: status === 204 ? null : {} }));
        const res = await response;
        expect(res.status).toBe(status === 204 ? 204 : 502);
        if (status === 204) expect(await res.text()).toBe("");
      }
    } finally { ws.close(); }
  });

  test("rejects duplicate device websocket", async () => {
    const ws1 = await openDeviceSocket(baseUrl, identity.digest);
    expect((await authenticateDevice(ws1, identity)).success).toBe(true);

    const wsUrl = baseUrl.replace(/^http/, "ws") + `/cloud/device/${identity.digest}`;
    const duplicate = await fetch(wsUrl.replace(/^ws/, "http"), {
      headers: {
        Upgrade: "websocket",
        Connection: "Upgrade",
        "Sec-WebSocket-Key": "dGhlIHNhbXBsZSBub25jZQ==",
        "Sec-WebSocket-Version": "13",
      },
    });
    expect(duplicate.status).toBe(409);
    const body = (await duplicate.json()) as { reason: string };
    expect(body.reason).toBe("device already connected");

    ws1.close();
    await Bun.sleep(300);
  }, 60_000);

  test("rejects http while device unauthenticated", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    await waitForMessage(ws, (msg): msg is AuthRequestMessage => msg.type === "authRequest");
    const token = await bearerFor(other.digest);

    const res = await fetch(`${baseUrl}/device/${other.digest}/pin/1`, {
      headers: { authorization: `Bearer ${token}` },
    });
    expect(res.status).toBe(503);
    expect(((await res.json()) as { reason: string }).reason).toBe("device offline");

    ws.close();
  }, 60_000);

  test("rejects digest mismatch", async () => {
    const ws = await openDeviceSocket(baseUrl, identity.digest);
    const result = await authenticateDevice(ws, identity, { digest: PLACEHOLDER.zero });
    expect(result.success).toBe(false);
    expect(result.reason?.toLowerCase()).toContain("digest");
  }, 60_000);

  test("rejects bad signature", async () => {
    const ws = await openDeviceSocket(baseUrl, identity.digest);
    const result = await authenticateDevice(ws, identity, {
      response: bytesToBase64Url(crypto.getRandomValues(new Uint8Array(SIGNATURE_BYTES))),
    });
    expect(result.success).toBe(false);
  }, 60_000);

  test("rejects signature without domain prefix", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    const authRequest = await waitForMessage(ws, (msg): msg is AuthRequestMessage => msg.type === "authRequest");
    const challenge = base64UrlToBytes(authRequest.challenge);
    const bare = await crypto.subtle.sign({ name: "ECDSA", hash: "SHA-256" }, other.privateKey, challenge);
    ws.send(
      JSON.stringify({
        type: "authResponse",
        devicePublicKey: other.publicKeyB64,
        devicePublicKeyDigest: other.digest,
        version: "test-1.0.0",
        response: bytesToBase64Url(bare),
      }),
    );
    const result = await waitForMessage(ws, (msg): msg is AuthResultMessage => msg.type === "authResult");
    expect(result.success).toBe(false);
    ws.close();
  }, 60_000);

  test("rejects RSA public key material", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    await waitForMessage(ws, (msg): msg is AuthRequestMessage => msg.type === "authRequest");
    const rsa = (await crypto.subtle.generateKey(
      { name: "RSA-PSS", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-256" },
      true,
      ["sign", "verify"],
    )) as CryptoKeyPair;
    const spki = new Uint8Array((await crypto.subtle.exportKey("spki", rsa.publicKey)) as ArrayBuffer);
    ws.send(
      JSON.stringify({
        type: "authResponse",
        devicePublicKey: bytesToBase64Url(spki),
        devicePublicKeyDigest: other.digest,
        version: "test-1.0.0",
        response: bytesToBase64Url(crypto.getRandomValues(new Uint8Array(SIGNATURE_BYTES))),
      }),
    );
    const result = await waitForMessage(ws, (msg): msg is AuthResultMessage => msg.type === "authResult");
    expect(result.success).toBe(false);
    ws.close();
  }, 60_000);

  test("rejects wrong signature length", async () => {
    const ws = await openDeviceSocket(baseUrl, identity.digest);
    const result = await authenticateDevice(ws, identity, {
      response: bytesToBase64Url(crypto.getRandomValues(new Uint8Array(48))),
    });
    expect(result.success).toBe(false);
  }, 60_000);

  test("rejects malformed auth response", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    await waitForMessage(ws, (msg): msg is AuthRequestMessage => msg.type === "authRequest");
    const closed = new Promise<void>((resolve) => {
      ws.addEventListener("close", () => resolve(), { once: true });
    });
    ws.send(JSON.stringify({ type: "authResponse", broken: true }));
    const result = await waitForMessage(ws, (msg): msg is AuthResultMessage => msg.type === "authResult").catch(
      () => null,
    );
    if (result) {
      expect(result.success).toBe(false);
    }
    await closed;
  }, 60_000);

  test("auth timeout disconnects", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    await waitForMessage(ws, (msg): msg is AuthRequestMessage => msg.type === "authRequest");
    const result = await waitForMessage(ws, (msg): msg is AuthResultMessage => msg.type === "authResult", 5_000);
    expect(result.success).toBe(false);
    expect(result.reason).toBe("authentication timeout");
  }, 60_000);

  test("response timeout returns 504", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);
    const token = await bearerFor(other.digest);

    const res = await fetch(`${baseUrl}/device/${other.digest}/slow`, {
      headers: { authorization: `Bearer ${token}` },
    });
    expect(res.status).toBe(504);
    expect(((await res.json()) as { reason: string }).reason).toBe("device timeout");
    ws.close();
  }, 60_000);

  test("disconnect with pending request returns 503", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);
    const token = await bearerFor(other.digest);

    const pending = waitForMessage(ws, (msg): msg is RequestMessage => msg.type === "request");
    const httpPromise = fetch(`${baseUrl}/device/${other.digest}/drop`, {
      headers: { authorization: `Bearer ${token}` },
    });
    await pending;
    ws.close();
    const res = await httpPromise;
    expect(res.status).toBe(503);
    expect(((await res.json()) as { reason: string }).reason).toBe("device offline");
  }, 60_000);

  test("offline device returns 503", async () => {
    const digest = PLACEHOLDER.a;
    const token = await bearerFor(digest);
    const res = await fetch(`${baseUrl}/device/${digest}/pin/1`, {
      headers: { authorization: `Bearer ${token}` },
    });
    expect(res.status).toBe(503);
  }, 30_000);
});
