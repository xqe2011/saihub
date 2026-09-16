import { sealRoutingToken } from "./crypto.ts";
import type { Env } from "./env.ts";
import { isDigest, jsonError } from "./protocol.ts";
import { proxyToDevice } from "./proxy.ts";

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

export async function handlePairingSession(request: Request, env: Env): Promise<Response> {
  if (request.method !== "POST") {
    return jsonError(405, "method not allowed");
  }
  const body = await readJsonBody(request);
  if (!body) {
    return jsonError(400, "invalid body");
  }
  const digestRaw = typeof body.devicePublicKeyDigest === "string" ? body.devicePublicKeyDigest : "";
  const digest = digestRaw.toLowerCase();
  if (!isDigest(digest)) {
    return jsonError(400, "invalid devicePublicKeyDigest");
  }

  return proxyToDevice(env, digest, "POST", "/pairing/session", JSON.stringify({ devicePublicKeyDigest: digest }), {
    "content-type": "application/json",
  });
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
  const digestRaw = typeof body.devicePublicKeyDigest === "string" ? body.devicePublicKeyDigest : "";
  const digest = digestRaw.toLowerCase();
  const sessionToken = typeof body.sessionToken === "string" ? body.sessionToken : "";
  if (!isDigest(digest)) {
    return jsonError(400, "invalid devicePublicKeyDigest");
  }
  if (!sessionToken) {
    return jsonError(400, "invalid sessionToken");
  }

  const deviceRes = await proxyToDevice(
    env,
    digest,
    "POST",
    "/pairing/token",
    JSON.stringify({ devicePublicKeyDigest: digest, sessionToken }),
    { "content-type": "application/json" },
  );

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
