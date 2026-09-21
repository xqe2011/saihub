import { openRoutingToken } from "./crypto.ts";
import type { Env } from "./env.ts";
import { DIGEST_RE, isDigest, jsonError } from "./protocol.ts";

export const ACCESS_TOKEN_EXPIRES_IN = 315_360_000; // 10 years
export const STATIC_CLIENT_ID = "saihub-static-client";

const DIGEST_PATH = "([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]{26,33})";
const DEVICE_MCP_RE = new RegExp(`^/device/${DIGEST_PATH}/mcp/?$`);
const DEVICE_PATH_RE = new RegExp(`^/device/${DIGEST_PATH}(?:/|$)`);

export function unauthorized(request: Request): Response {
  const origin = new URL(request.url).origin;
  const pathname = new URL(request.url).pathname;
  const metadataPath = protectedResourceMetadataPath(pathname);
  const resourceMetadata = `${origin}${metadataPath}`;
  const wwwAuthenticate =
    `Bearer error="invalid_token", error_description="missing or invalid access token", resource_metadata="${resourceMetadata}"`;
  return new Response(JSON.stringify({ reason: "missing or invalid access token" }), {
    status: 401,
    headers: {
      "content-type": "application/json",
      "www-authenticate": wwwAuthenticate,
    },
  });
}

function protectedResourceMetadataPath(pathname: string): string {
  const mcp = DEVICE_MCP_RE.exec(pathname);
  if (mcp) {
    return `/.well-known/oauth-protected-resource/device/${mcp[1]!}/mcp`;
  }
  return "/.well-known/oauth-protected-resource";
}

export function handleWellKnown(request: Request): Response {
  if (request.method !== "GET") {
    return jsonError(405, "method not allowed");
  }
  const origin = new URL(request.url).origin;
  return Response.json({
    issuer: origin,
    authorization_endpoint: `${origin}/cloud/oauth/redirect`,
    token_endpoint: `${origin}/token`,
    registration_endpoint: `${origin}/register`,
    response_types_supported: ["code"],
    grant_types_supported: ["authorization_code"],
    code_challenge_methods_supported: ["S256"],
    token_endpoint_auth_methods_supported: ["none"],
  });
}

export function handleProtectedResourceMetadata(request: Request, pathname: string): Response {
  if (request.method !== "GET") {
    return jsonError(405, "method not allowed");
  }
  const origin = new URL(request.url).origin;
  const suffix = pathname.replace(/^\/\.well-known\/oauth-protected-resource/, "") || "";
  const resource = suffix.length > 0 ? `${origin}${suffix}` : `${origin}/`;
  return Response.json({
    resource,
    authorization_servers: [origin],
    bearer_methods_supported: ["header"],
  });
}

export async function handleRegister(request: Request): Promise<Response> {
  if (request.method !== "POST") {
    return jsonError(405, "method not allowed");
  }

  let redirectUris: string[] = ["http://127.0.0.1/callback"];
  let clientName: string | undefined;
  let grantTypes = ["authorization_code"];
  let responseTypes = ["code"];
  let tokenEndpointAuthMethod = "none";

  try {
    const body = (await request.json()) as unknown;
    if (typeof body === "object" && body !== null && !Array.isArray(body)) {
      const record = body as Record<string, unknown>;
      if (Array.isArray(record.redirect_uris) && record.redirect_uris.every((u) => typeof u === "string")) {
        redirectUris = record.redirect_uris as string[];
      }
      if (typeof record.client_name === "string") {
        clientName = record.client_name;
      }
      if (Array.isArray(record.grant_types) && record.grant_types.every((g) => typeof g === "string")) {
        grantTypes = record.grant_types as string[];
      }
      if (Array.isArray(record.response_types) && record.response_types.every((r) => typeof r === "string")) {
        responseTypes = record.response_types as string[];
      }
      if (typeof record.token_endpoint_auth_method === "string") {
        tokenEndpointAuthMethod = record.token_endpoint_auth_method;
      }
    }
  } catch {
    // ignore body; return static defaults
  }

  return Response.json(
    {
      client_id: STATIC_CLIENT_ID,
      client_id_issued_at: Math.floor(Date.now() / 1000),
      client_name: clientName,
      redirect_uris: redirectUris,
      grant_types: grantTypes,
      response_types: responseTypes,
      token_endpoint_auth_method: tokenEndpointAuthMethod,
    },
    { status: 201 },
  );
}

export async function handleToken(request: Request, env: Env): Promise<Response> {
  if (request.method !== "POST") {
    return jsonError(405, "method not allowed");
  }
  if (!env.ROUTING_TOKEN_SECRET) {
    return jsonError(500, "routing token secret not configured");
  }

  const params = await readTokenParams(request);
  if (!params) {
    return Response.json({ error: "invalid_request" }, { status: 400 });
  }
  if (params.grant_type !== "authorization_code" || !params.code) {
    return Response.json({ error: "invalid_request" }, { status: 400 });
  }

  const opened = await openRoutingToken(env.ROUTING_TOKEN_SECRET, params.code);
  if (!opened) {
    return Response.json({ error: "invalid_grant" }, { status: 400 });
  }

  return Response.json({
    access_token: params.code,
    token_type: "Bearer",
    expires_in: ACCESS_TOKEN_EXPIRES_IN,
  });
}

async function readTokenParams(request: Request): Promise<Record<string, string> | null> {
  const contentType = request.headers.get("content-type") ?? "";
  try {
    if (contentType.includes("application/json")) {
      const body = (await request.json()) as unknown;
      if (typeof body !== "object" || body === null || Array.isArray(body)) {
        return null;
      }
      const out: Record<string, string> = {};
      for (const [key, value] of Object.entries(body as Record<string, unknown>)) {
        if (typeof value === "string") {
          out[key] = value;
        }
      }
      return out;
    }
    const text = await request.text();
    const params = new URLSearchParams(text);
    const out: Record<string, string> = {};
    for (const [key, value] of params.entries()) {
      out[key] = value;
    }
    return out;
  } catch {
    return null;
  }
}

export function handleOauthRedirectPage(request: Request): Response {
  if (request.method !== "GET") {
    return jsonError(405, "method not allowed");
  }
  const url = new URL(request.url);
  const digest = resolveDigest(url);
  const redirectUri = url.searchParams.get("redirect_uri") ?? "";
  const state = url.searchParams.get("state") ?? "";

  if (!digest || !isDigest(digest)) {
    return jsonError(400, "invalid devicePublicKeyDigest");
  }
  if (!isAbsoluteHttpUrl(redirectUri)) {
    return jsonError(400, "invalid redirect_uri");
  }

  return new Response(pairingPageHtml(digest, redirectUri, state), {
    status: 200,
    headers: { "content-type": "text/html; charset=utf-8" },
  });
}

function resolveDigest(url: URL): string | null {
  const direct = url.searchParams.get("devicePublicKeyDigest") ?? "";
  if (DIGEST_RE.test(direct)) {
    return direct;
  }
  const resource = url.searchParams.get("resource") ?? "";
  if (!resource) {
    return null;
  }
  try {
    const resourceUrl = new URL(resource);
    const match = DEVICE_MCP_RE.exec(resourceUrl.pathname) ?? DEVICE_PATH_RE.exec(resourceUrl.pathname);
    return match ? match[1]! : null;
  } catch {
    return null;
  }
}

function isAbsoluteHttpUrl(value: string): boolean {
  try {
    const parsed = new URL(value);
    return parsed.protocol === "https:" || parsed.protocol === "http:";
  } catch {
    return false;
  }
}

function pairingPageHtml(digest: string, redirectUri: string, state: string): string {
  return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Pair device</title>
<style>
body{margin:0;font-family:system-ui,sans-serif;background:#f5f7f6;color:#171d1b;display:flex;min-height:100vh;align-items:center;justify-content:center}
main{max-width:28rem;padding:1.5rem;text-align:center}
h1{font-size:1.25rem;margin:0 0 .75rem}
p{margin:.5rem 0;color:#3f4945}
.err{color:#ba1a1a}
</style>
</head>
<body>
<main>
  <h1 id="status">Name this client</h1>
  <p id="detail">Choose a name so you can revoke this grant later on the device.</p>
  <p><input id="name" type="text" maxlength="32" placeholder="Cursor" autocomplete="off" style="width:100%;max-width:20rem;padding:.6rem .75rem;font:inherit;border:1px solid #c5cdc9;border-radius:8px"/></p>
  <p><button id="start" type="button" style="padding:.6rem 1.1rem;font:inherit;font-weight:650;border:0;border-radius:999px;background:#006b54;color:#fff;cursor:pointer">Pair</button></p>
  <p class="err" id="err"></p>
</main>
<script>
(() => {
  const digest = ${JSON.stringify(digest)};
  const redirectUri = ${JSON.stringify(redirectUri)};
  const state = ${JSON.stringify(state)};
  const statusEl = document.getElementById('status');
  const detailEl = document.getElementById('detail');
  const errEl = document.getElementById('err');
  const nameEl = document.getElementById('name');
  const startEl = document.getElementById('start');
  let countdownTimer = null;

  function fail(msg) {
    if (countdownTimer) clearInterval(countdownTimer);
    statusEl.textContent = 'Pairing failed';
    errEl.textContent = msg || 'Pairing failed.';
    startEl.disabled = false;
  }

  function startCountdown(expiredAt) {
    const tick = () => {
      const left = Math.max(0, expiredAt - Math.floor(Date.now() / 1000));
      detailEl.textContent = left + 's remaining';
      if (left <= 0 && countdownTimer) {
        clearInterval(countdownTimer);
        countdownTimer = null;
      }
    };
    tick();
    countdownTimer = setInterval(tick, 250);
  }

  function redirectWithCode(code) {
    const target = new URL(redirectUri);
    target.searchParams.set('code', code);
    if (state) target.searchParams.set('state', state);
    location.href = target.toString();
  }

  startEl.onclick = async () => {
    const name = (nameEl.value || '').trim();
    if (!name) {
      errEl.textContent = 'Enter a name.';
      return;
    }
    errEl.textContent = '';
    startEl.disabled = true;
    nameEl.disabled = true;
    try {
      const sessionRes = await fetch('/cloud/pairing/session', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ devicePublicKeyDigest: digest, name }),
      });
      const sessionBody = await sessionRes.json().catch(() => ({}));
      if (!sessionRes.ok) throw new Error(sessionBody.reason || 'session failed');

      statusEl.textContent = 'Press button for 3 seconds.';
      startCountdown(Number(sessionBody.expiredAt) || 0);

      const tokenRes = await fetch('/cloud/pairing/token', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ devicePublicKeyDigest: digest, sessionToken: sessionBody.sessionToken }),
      });
      const tokenBody = await tokenRes.json().catch(() => ({}));
      if (!tokenRes.ok) throw new Error(tokenBody.reason || 'token failed');
      if (!tokenBody.routingToken) throw new Error('missing routing token');
      if (countdownTimer) clearInterval(countdownTimer);
      redirectWithCode(tokenBody.routingToken);
    } catch (e) {
      fail(e && e.message ? e.message : 'Pairing failed.');
    }
  };
})();
</script>
</body>
</html>`;
}
