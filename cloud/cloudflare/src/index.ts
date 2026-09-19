import { openRoutingToken } from "./crypto.ts";
import { Device } from "./device.ts";
import type { Env } from "./env.ts";
import { handleOauthRedirectPage, handleProtectedResourceMetadata, handleRegister, handleToken, handleWellKnown, unauthorized } from "./oauth.ts";
import { handlePairingSession, handlePairingToken } from "./pairing.ts";
import { isDigest, isJsonContentType, unsupportedContentType, selectForwardHeaders, jsonError } from "./protocol.ts";
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
  const contentType = request.headers.get("content-type");
  const isMcp = suffixPath === "/mcp";
  if (isMcp && (request.method === "GET" || request.method === "DELETE")) {
    return new Response(null, { status: 405, headers: { Allow: "POST, OPTIONS" } });
  }
  // Validate writes before the device relay adds its JSON transport header.
  const requiresJson = ["POST", "PUT", "PATCH"].includes(request.method);
  if ((contentType !== null || requiresJson) && !isJsonContentType(contentType)) return unsupportedContentType(contentType);
  const accept = request.headers.get("accept");
  if (accept && !accept.split(",").some((part) => /^(application\/json|application\/\*|\*\/\*)$/i.test(part.split(";", 1)[0]!.trim()) && !/;\s*q=0(?:\.0*)?\s*(?:;|$)/i.test(part))) {
    return unsupportedContentType(accept, 406);
  }
  const headers = selectForwardHeaders(request);

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
