import { openRoutingToken } from "./crypto.ts";
import { Device } from "./device.ts";
import type { Env } from "./env.ts";
import { handleOauthRedirectPage, handleProtectedResourceMetadata, handleRegister, handleToken, handleWellKnown, unauthorized } from "./oauth.ts";
import { handlePairingSession, handlePairingToken } from "./pairing.ts";
import { isDigest, jsonError } from "./protocol.ts";
import { proxyToDevice } from "./proxy.ts";

export { Device };

const DEVICE_WS_RE = /^\/cloud\/device\/([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]{26,33})$/;
const DEVICE_HTTP_RE = /^\/device\/([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]{26,33})(\/.*)?$/;
const PROTECTED_RESOURCE_RE = /^\/\.well-known\/oauth-protected-resource(?:\/.*)?$/;

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const pathname = url.pathname;

    if (pathname === "/.well-known/oauth-authorization-server") {
      return handleWellKnown(request);
    }
    if (PROTECTED_RESOURCE_RE.test(pathname)) {
      return handleProtectedResourceMetadata(request, pathname);
    }
    if (pathname === "/register") {
      return handleRegister(request);
    }
    if (pathname === "/token") {
      return handleToken(request, env);
    }
    if (pathname === "/cloud/oauth/redirect") {
      return handleOauthRedirectPage(request);
    }
    if (pathname === "/cloud/pairing/session") {
      return handlePairingSession(request, env);
    }
    if (pathname === "/cloud/pairing/token") {
      return handlePairingToken(request, env);
    }

    const wsMatch = DEVICE_WS_RE.exec(pathname);
    if (wsMatch) {
      return handleDeviceWebSocket(request, env, wsMatch[1]!);
    }

    const httpMatch = DEVICE_HTTP_RE.exec(pathname);
    if (httpMatch) {
      return handleDeviceHttp(request, env, httpMatch[1]!, httpMatch[2] ?? "/", url.search);
    }

    return jsonError(404, "not found");
  },
};

function handleDeviceWebSocket(request: Request, env: Env, digest: string): Promise<Response> | Response {
  if (!isDigest(digest)) {
    return jsonError(400, "invalid digest");
  }
  if (request.method !== "GET") {
    return jsonError(405, "method not allowed");
  }
  if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
    return jsonError(426, "websocket upgrade required");
  }

  const id = env.DEVICE.idFromName(digest);
  const stub = env.DEVICE.get(id);
  const forwardUrl = new URL("https://device/websocket");
  forwardUrl.searchParams.set("digest", digest);
  return stub.fetch(forwardUrl, request);
}

async function handleDeviceHttp(
  request: Request,
  env: Env,
  digest: string,
  suffixPath: string,
  search: string,
): Promise<Response> {
  if (!isDigest(digest)) {
    return jsonError(400, "invalid digest");
  }

  const auth = await requireRoutingToken(request, env, digest);
  if (auth) {
    return auth;
  }

  const path = `${suffixPath === "" ? "/" : suffixPath}${search}`;
  const headers: Record<string, string> = {};
  for (const name of ["content-type", "accept", "x-lock-id"] as const) {
    const value = request.headers.get(name);
    if (value !== null) {
      headers[name] = value;
    }
  }

  const body =
    request.method === "GET" || request.method === "HEAD" ? undefined : await request.text();
  return proxyToDevice(env, digest, request.method, path, body, headers);
}

async function requireRoutingToken(request: Request, env: Env, digest: string): Promise<Response | null> {
  if (!env.ROUTING_TOKEN_SECRET) {
    return jsonError(500, "routing token secret not configured");
  }
  const header = request.headers.get("authorization");
  if (!header || !header.toLowerCase().startsWith("bearer ")) {
    return unauthorized(request);
  }
  const token = header.slice("bearer ".length).trim();
  if (!token) {
    return unauthorized(request);
  }
  const payload = await openRoutingToken(env.ROUTING_TOKEN_SECRET, token);
  if (!payload || payload.devicePublicKeyDigest !== digest) {
    return unauthorized(request);
  }
  return null;
}
