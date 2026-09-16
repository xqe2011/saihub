import { afterAll, beforeAll, describe, expect, test } from "bun:test";
import { spawn, type Subprocess } from "bun";
import { join } from "node:path";

const PACKAGE_DIR = join(import.meta.dir, "../../../cloud/cloudflare");
const PSS_SALT_LENGTH = 32;
const BASE_PORT = 8787 + Math.floor(Math.random() * 1000);

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
  body: string;
};

type DeviceInbound = AuthRequestMessage | AuthResultMessage | RequestMessage;

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

async function generateDeviceIdentity(): Promise<{
  privateKey: CryptoKey;
  publicKeySpki: Uint8Array;
  digest: string;
  publicKeyB64: string;
}> {
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
  const spkiBuf = await crypto.subtle.exportKey("spki", keyPair.publicKey);
  const spki = new Uint8Array(spkiBuf as ArrayBuffer);
  const digest = await sha256Hex(spki);
  return {
    privateKey: keyPair.privateKey,
    publicKeySpki: spki,
    digest,
    publicKeyB64: bytesToBase64Url(spki),
  };
}

async function signChallenge(privateKey: CryptoKey, challengeB64: string): Promise<string> {
  const challenge = base64UrlToBytes(challengeB64);
  const signature = await crypto.subtle.sign(
    { name: "RSA-PSS", saltLength: PSS_SALT_LENGTH },
    privateKey,
    challenge,
  );
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

  test("authenticates device and forwards http request/response", async () => {
    const ws = await openDeviceSocket(baseUrl, identity.digest);
    const result = await authenticateDevice(ws, identity);
    expect(result.success).toBe(true);

    const pendingRequest = waitForMessage(ws, (msg): msg is RequestMessage => msg.type === "request");

    const httpPromise = fetch(`${baseUrl}/device/${identity.digest}/pin/1?mode=pwm`, {
      method: "PUT",
      headers: {
        "content-type": "application/json",
        accept: "application/json",
        "x-lock-id": "lock-abc",
        authorization: "secret-should-not-forward",
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

    const requestBody = new TextDecoder().decode(base64UrlToBytes(deviceRequest.body));
    expect(JSON.parse(requestBody)).toEqual({ duty: 0.5 });

    ws.send(
      JSON.stringify({
        type: "response",
        requestId: deviceRequest.requestId,
        status: 200,
        headers: { "content-type": "application/json", "x-lock-id": "lock-abc" },
        body: bytesToBase64Url(new TextEncoder().encode(JSON.stringify({ ok: true, pin: 1 }))),
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

  test("correlates concurrent out-of-order responses", async () => {
    const ws = await openDeviceSocket(baseUrl, identity.digest);
    expect((await authenticateDevice(ws, identity)).success).toBe(true);

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

    const first = fetch(`${baseUrl}/device/${identity.digest}/first`, { method: "GET" });
    const second = fetch(`${baseUrl}/device/${identity.digest}/second`, { method: "GET" });
    await collect;

    const [a, b] = requests;
    expect(a).toBeDefined();
    expect(b).toBeDefined();
    expect(a!.requestId).not.toBe(b!.requestId);

    // Respond to the second request first.
    ws.send(
      JSON.stringify({
        type: "response",
        requestId: b!.requestId,
        status: 201,
        headers: { "content-type": "text/plain" },
        body: bytesToBase64Url(new TextEncoder().encode("second")),
      }),
    );
    ws.send(
      JSON.stringify({
        type: "response",
        requestId: a!.requestId,
        status: 200,
        headers: { "content-type": "text/plain" },
        body: bytesToBase64Url(new TextEncoder().encode("first")),
      }),
    );

    const [firstRes, secondRes] = await Promise.all([first, second]);
    expect(firstRes.status).toBe(200);
    expect(await firstRes.text()).toBe("first");
    expect(secondRes.status).toBe(201);
    expect(await secondRes.text()).toBe("second");
    ws.close();
  }, 60_000);

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

  test("rejects http while unauthenticated", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    // Connected but not authenticated yet (still waiting on challenge).
    await waitForMessage(ws, (msg): msg is AuthRequestMessage => msg.type === "authRequest");

    const res = await fetch(`${baseUrl}/device/${other.digest}/pin/1`);
    expect(res.status).toBe(503);
    expect(((await res.json()) as { reason: string }).reason).toBe("device offline");

    ws.close();
  }, 60_000);

  test("rejects digest mismatch", async () => {
    const ws = await openDeviceSocket(baseUrl, identity.digest);
    const result = await authenticateDevice(ws, identity, { digest: "0".repeat(64) });
    expect(result.success).toBe(false);
    expect(result.reason?.toLowerCase()).toContain("digest");
  }, 60_000);

  test("rejects bad signature", async () => {
    const ws = await openDeviceSocket(baseUrl, identity.digest);
    const result = await authenticateDevice(ws, identity, {
      response: bytesToBase64Url(crypto.getRandomValues(new Uint8Array(384))),
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

    // Do not answer the forwarded request.
    const res = await fetch(`${baseUrl}/device/${other.digest}/slow`);
    expect(res.status).toBe(504);
    expect(((await res.json()) as { reason: string }).reason).toBe("device timeout");
    ws.close();
  }, 60_000);

  test("disconnect with pending request returns 503", async () => {
    const other = await generateDeviceIdentity();
    const ws = await openDeviceSocket(baseUrl, other.digest);
    expect((await authenticateDevice(ws, other)).success).toBe(true);

    const pending = waitForMessage(ws, (msg): msg is RequestMessage => msg.type === "request");
    const httpPromise = fetch(`${baseUrl}/device/${other.digest}/drop`);
    await pending;
    ws.close();
    const res = await httpPromise;
    expect(res.status).toBe(503);
    expect(((await res.json()) as { reason: string }).reason).toBe("device offline");
  }, 60_000);

  test("offline device returns 503", async () => {
    const digest = "a".repeat(64);
    const res = await fetch(`${baseUrl}/device/${digest}/pin/1`);
    expect(res.status).toBe(503);
  }, 30_000);
});
