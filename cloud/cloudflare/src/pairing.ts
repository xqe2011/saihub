import { sealRoutingToken } from "./crypto.ts";
import type { Env } from "./env.ts";
import { isDigest, jsonError, MAX_PAIRING_NAME_LEN } from "./protocol.ts";

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

async function readJsonBody(request: Request): Promise<Record<string, unknown> | null> {
  try {
    const body = (await request.json()) as unknown;
    return isRecord(body) ? body : null;
  } catch {
    return null;
  }
}

function pairingStub(env: Env, digest: string, path: string, body: Record<string, unknown>): Promise<Response> {
  const id = env.DEVICE.idFromName(digest);
  const stub = env.DEVICE.get(id);
  return stub.fetch(new Request(`https://device${path}`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  }));
}

export async function handlePairingSession(request: Request, env: Env): Promise<Response> {
  if (request.method !== "POST") {
    return jsonError(405, "method not allowed");
  }
  const body = await readJsonBody(request);
  if (!body) {
    return jsonError(400, "invalid body");
  }
  const digest = typeof body.devicePublicKeyDigest === "string" ? body.devicePublicKeyDigest : "";
  const name = typeof body.name === "string" ? body.name.trim() : "";
  if (!isDigest(digest)) {
    return jsonError(400, "invalid devicePublicKeyDigest");
  }
  if (!name || name.length > MAX_PAIRING_NAME_LEN) {
    return jsonError(400, "invalid name");
  }

  return pairingStub(env, digest, "/pairing/session", { name });
}

export async function handlePairingToken(request: Request, env: Env): Promise<Response> {
  if (request.method !== "POST") {
    return jsonError(405, "method not allowed");
  }
  if (!env.ROUTING_TOKEN_SECRET) {
    return jsonError(500, "routing token secret not configured");
  }

  const body = await readJsonBody(request);
  if (!body) {
    return jsonError(400, "invalid body");
  }
  const digest = typeof body.devicePublicKeyDigest === "string" ? body.devicePublicKeyDigest : "";
  const sessionToken = typeof body.sessionToken === "string" ? body.sessionToken : "";
  if (!isDigest(digest)) {
    return jsonError(400, "invalid devicePublicKeyDigest");
  }
  if (!sessionToken) {
    return jsonError(400, "invalid sessionToken");
  }

  const deviceRes = await pairingStub(env, digest, "/pairing/token", { sessionToken });

  if (!deviceRes.ok) {
    return deviceRes;
  }

  let deviceBody: unknown;
  try {
    deviceBody = await deviceRes.json();
  } catch {
    return jsonError(502, "invalid device response");
  }
  if (!isRecord(deviceBody) || typeof deviceBody.grantSecret !== "string" || deviceBody.grantSecret.length === 0) {
    return jsonError(502, "invalid device response");
  }

  const routingToken = await sealRoutingToken(env.ROUTING_TOKEN_SECRET, {
    devicePublicKeyDigest: digest,
    grantSecret: deviceBody.grantSecret,
  });
  return Response.json({ routingToken });
}
