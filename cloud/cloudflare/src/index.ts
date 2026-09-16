import { Device } from "./device.ts";
import type { Env } from "./env.ts";
import { isDigest, jsonError } from "./protocol.ts";

export { Device };

const DEVICE_WS_RE = /^\/cloud\/device\/([0-9a-fA-F]{64})$/;
const DEVICE_HTTP_RE = /^\/device\/([0-9a-fA-F]{64})(\/.*)?$/;

function normalizeDigest(value: string): string {
  return value.toLowerCase();
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const pathname = url.pathname;

    const wsMatch = DEVICE_WS_RE.exec(pathname);
    if (wsMatch) {
      return handleDeviceWebSocket(request, env, normalizeDigest(wsMatch[1]!));
    }

    const httpMatch = DEVICE_HTTP_RE.exec(pathname);
    if (httpMatch) {
      return handleDeviceHttp(request, env, normalizeDigest(httpMatch[1]!), httpMatch[2] ?? "/", url.search);
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

  const path = `${suffixPath === "" ? "/" : suffixPath}${search}`;
  const id = env.DEVICE.idFromName(digest);
  const stub = env.DEVICE.get(id);
  const forwardUrl = new URL("https://device/proxy");
  forwardUrl.searchParams.set("path", path);

  const headers = new Headers(request.headers);
  return stub.fetch(
    new Request(forwardUrl, {
      method: request.method,
      headers,
      body: request.method === "GET" || request.method === "HEAD" ? undefined : request.body,
    }),
  );
}
