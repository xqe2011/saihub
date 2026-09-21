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

export function handleOauthEchoPage(request: Request): Response {
  if (request.method !== "GET") {
    return jsonError(405, "method not allowed");
  }
  return new Response(oauthEchoPageHtml(), {
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
body{margin:0;font-family:system-ui,sans-serif;background:#f5fbf7;color:#171d1b;display:flex;min-height:100vh;align-items:center;justify-content:center}
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
  <p id="count" hidden></p>
  <div id="form">
    <p><input id="name" type="text" maxlength="32" placeholder="Cursor" autocomplete="off" style="width:100%;max-width:20rem;padding:.6rem .75rem;font:inherit;border:1px solid #c5cdc9;border-radius:8px"/></p>
    <p><button id="start" type="button" style="padding:.6rem 1.1rem;font:inherit;font-weight:650;border:0;border-radius:999px;background:#006b54;color:#fff;cursor:pointer">Pair</button></p>
  </div>
  <p class="err" id="err"></p>
</main>
<script>
(() => {
  const digest = ${JSON.stringify(digest)};
  const redirectUri = ${JSON.stringify(redirectUri)};
  const state = ${JSON.stringify(state)};
  const statusEl = document.getElementById('status');
  const detailEl = document.getElementById('detail');
  const countEl = document.getElementById('count');
  const formEl = document.getElementById('form');
  const errEl = document.getElementById('err');
  const nameEl = document.getElementById('name');
  const startEl = document.getElementById('start');
  let countdownTimer = null;

  function fail(msg) {
    if (countdownTimer) clearInterval(countdownTimer);
    countdownTimer = null;
    formEl.hidden = false;
    countEl.hidden = true;
    nameEl.disabled = false;
    startEl.disabled = false;
    statusEl.textContent = 'Pairing failed';
    detailEl.textContent = 'Choose a name so you can revoke this grant later on the device.';
    errEl.textContent = msg || 'Pairing failed.';
  }

  function startCountdown(expiredAt) {
    const tick = () => {
      const left = Math.max(0, expiredAt - Math.floor(Date.now() / 1000));
      countEl.textContent = left + 's remaining';
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

      formEl.hidden = true;
      countEl.hidden = false;
      statusEl.textContent = 'Hold BOOT for 3 seconds';
      detailEl.textContent = 'This approval will create grant \`' + name + '\`.';
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

function oauthEchoPageHtml(): string {
  return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"/>
<title>SAIHUB routing token</title>
<style>
:root{
  --md-sys-color-primary:#006b54;
  --md-sys-color-on-primary:#ffffff;
  --md-sys-color-primary-container:#89f8d2;
  --md-sys-color-on-primary-container:#002117;
  --md-sys-color-secondary-container:#cee9dc;
  --md-sys-color-on-secondary-container:#072019;
  --md-sys-color-surface:#f5fbf7;
  --md-sys-color-surface-container:#e9efeb;
  --md-sys-color-surface-container-high:#e3e9e5;
  --md-sys-color-surface-container-highest:#dee4e0;
  --md-sys-color-surface-container-lowest:#ffffff;
  --md-sys-color-on-surface:#171d1b;
  --md-sys-color-on-surface-variant:#3f4945;
  --md-sys-color-outline:#6f7975;
  --md-sys-shape-corner-small:8px;
  --md-sys-shape-corner-extra-large:28px;
  --md-sys-elevation-1:0 1px 2px rgba(0,0,0,.16),0 1px 3px 1px rgba(0,0,0,.08);
}
*{box-sizing:border-box}
html,body{width:100%;margin:0;min-height:100%;overflow-x:hidden}
body{
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;
  color:var(--md-sys-color-on-surface);
  background:
    radial-gradient(900px 480px at 0% -5%, rgba(137,248,210,.55) 0%, transparent 60%),
    radial-gradient(700px 420px at 100% 0%, rgba(206,233,220,.7) 0%, transparent 55%),
    var(--md-sys-color-surface);
  background-attachment:fixed;
  -webkit-font-smoothing:antialiased;
}
.app{
  width:100%;
  max-width:32rem;
  margin:0 auto;
  padding:max(1.25rem, env(safe-area-inset-top)) 1rem max(2rem, env(safe-area-inset-bottom));
}
.brand{margin:0;font-size:clamp(2rem,8vw,2.5rem);font-weight:700;letter-spacing:-.02em;line-height:1.1;color:var(--md-sys-color-primary)}
.subtitle{margin:.4rem 0 1.5rem;font-size:.95rem;line-height:1.4;color:var(--md-sys-color-on-surface-variant)}
.sheet{
  background:var(--md-sys-color-surface-container-lowest);
  border-radius:var(--md-sys-shape-corner-extra-large);
  box-shadow:var(--md-sys-elevation-1);
  padding:1.25rem 1rem 1.35rem;
}
.section-label{
  margin:0 0 .35rem .35rem;
  font-size:.72rem;
  font-weight:700;
  letter-spacing:.08em;
  text-transform:uppercase;
  color:var(--md-sys-color-on-surface-variant);
}
.hint{margin:0 0 1rem .35rem;font-size:.88rem;line-height:1.4;color:var(--md-sys-color-on-surface-variant)}
.row-head{display:flex;align-items:flex-end;gap:.65rem}
.field{position:relative;flex:1;min-width:0}
.field input{
  width:100%;
  appearance:none;
  border:1px solid var(--md-sys-color-outline);
  border-radius:var(--md-sys-shape-corner-small) var(--md-sys-shape-corner-small) 0 0;
  border-bottom-width:2px;
  background:var(--md-sys-color-surface-container-highest);
  color:var(--md-sys-color-on-surface);
  padding:1.35rem .85rem .55rem;
  font:inherit;
  font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  font-size:.82rem;
  outline:none;
}
.field input:focus{border-color:var(--md-sys-color-primary);background:var(--md-sys-color-surface-container-high)}
.field label{
  position:absolute;
  left:.85rem;
  top:.55rem;
  font-size:.72rem;
  font-weight:600;
  color:var(--md-sys-color-primary);
  pointer-events:none;
}
.copy{
  flex:none;
  min-width:4.5rem;
  min-height:2.75rem;
  border:0;
  border-radius:999px;
  padding:.55rem 1rem;
  font:inherit;
  font-size:.88rem;
  font-weight:650;
  background:var(--md-sys-color-primary);
  color:var(--md-sys-color-on-primary);
  cursor:pointer;
  box-shadow:var(--md-sys-elevation-1);
}
.copy:active{transform:scale(.97)}
.copy.done{background:var(--md-sys-color-secondary-container);color:var(--md-sys-color-on-secondary-container);box-shadow:none}
.copy:disabled{opacity:.42;cursor:not-allowed;box-shadow:none}
.empty{display:none}
.empty.show{display:block}
.token{display:none}
.token.show{display:block}
</style>
</head>
<body>
<main class="app">
  <h1 class="brand">SAIHUB</h1>
  <p class="subtitle">Routing token for REST calls.</p>
  <section class="sheet">
    <h2 class="section-label">Bearer token</h2>
    <p class="hint empty" id="missing">No routing token in this URL. Get one from the landing page.</p>
    <div class="token" id="tokenBox">
      <p class="hint">Send this as <code>Authorization: Bearer</code> on REST requests.</p>
      <div class="row-head">
        <div class="field">
          <input id="token" type="text" readonly spellcheck="false" value=""/>
          <label for="token">Routing token</label>
        </div>
        <button class="copy" type="button" id="copy">Copy</button>
      </div>
    </div>
  </section>
</main>
<script>
const token = new URLSearchParams(location.search).get("code") || "";
const missingEl = document.getElementById("missing");
const tokenBox = document.getElementById("tokenBox");
const input = document.getElementById("token");
const button = document.getElementById("copy");
if (token) {
  input.value = token;
  tokenBox.classList.add("show");
} else {
  missingEl.classList.add("show");
  button.disabled = true;
}
function done() {
  button.classList.add("done");
  button.textContent = "Copied";
  setTimeout(() => {
    button.classList.remove("done");
    button.textContent = "Copy";
  }, 1500);
}
function fallback() {
  input.focus();
  input.select();
  input.setSelectionRange(0, input.value.length);
  try {
    if (document.execCommand("copy")) done();
  } catch {}
}
button.addEventListener("click", () => {
  if (!input.value) return;
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(input.value).then(done, fallback);
  } else {
    fallback();
  }
});
</script>
</body>
</html>`;
}
