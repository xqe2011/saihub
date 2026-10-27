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
main{position:relative;max-width:28rem;width:100%;padding:1.5rem;text-align:center}
.lang-select{position:absolute;top:1rem;right:1rem;appearance:none;border:0;border-radius:999px;padding:.4rem .75rem;font:inherit;font-size:.82rem;font-weight:650;background:#cee9dc;color:#072019;cursor:pointer}
h1{font-size:1.25rem;margin:0 0 .75rem}
p{margin:.5rem 0;color:#3f4945}
.err{color:#ba1a1a}
</style>
</head>
<body>
<main>
  <select class="lang-select" id="lang" aria-label="Language" title="Language">
    <option value="en">English</option>
    <option value="zh">中文</option>
  </select>
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
  const I18N = {
    en: {
      language: "Language", title: "Name this client",
      detail: "Choose a name so you can revoke this grant later on the device.",
      pair: "Pair", failed: "Pairing failed", failedDetail: "Choose a name so you can revoke this grant later on the device.",
      enterName: "Enter a name.", holdBoot: "Hold BOOT for 3 seconds",
      grantDetail: "This approval will create grant \`{name}\`.", remaining: "{s}s remaining",
    },
    zh: {
      language: "语言", title: "为此客户端命名",
      detail: "选择一个名称，以便稍后在设备上撤销此授权。",
      pair: "配对", failed: "配对失败", failedDetail: "选择一个名称，以便稍后在设备上撤销此授权。",
      enterName: "请输入名称。", holdBoot: "按住 BOOT 三秒",
      grantDetail: "确认后将创建授权 \`{name}\`。", remaining: "剩余 {s} 秒",
    },
  };
  const LANG_KEY = "saihub-lang";
  function detectLang() {
    const saved = localStorage.getItem(LANG_KEY);
    if (saved === "en" || saved === "zh") return saved;
    return (navigator.language || "").toLowerCase().startsWith("zh") ? "zh" : "en";
  }
  let lang = detectLang();
  function t(key, vars) {
    const table = I18N[lang] || I18N.en;
    let s = table[key] != null ? table[key] : I18N.en[key] != null ? I18N.en[key] : key;
    if (vars) Object.keys(vars).forEach((k) => { s = s.replace(new RegExp("\\\\{" + k + "\\\\}", "g"), String(vars[k])); });
    return s;
  }
  const statusEl = document.getElementById('status');
  const detailEl = document.getElementById('detail');
  const countEl = document.getElementById('count');
  const formEl = document.getElementById('form');
  const errEl = document.getElementById('err');
  const nameEl = document.getElementById('name');
  const startEl = document.getElementById('start');
  const langEl = document.getElementById('lang');
  let countdownTimer = null;
  let pairingName = '';
  let expiredAt = 0;
  let phase = 'idle';

  function applyIdleCopy() {
    document.documentElement.lang = lang === "zh" ? "zh-CN" : "en";
    document.title = t("title");
    langEl.value = lang;
    langEl.setAttribute("aria-label", t("language"));
    langEl.title = t("language");
    startEl.textContent = t("pair");
    if (phase === "idle" || phase === "failed") {
      statusEl.textContent = phase === "failed" ? t("failed") : t("title");
      detailEl.textContent = phase === "failed" ? t("failedDetail") : t("detail");
    } else if (phase === "waiting") {
      statusEl.textContent = t("holdBoot");
      detailEl.textContent = t("grantDetail", { name: pairingName });
      if (expiredAt) {
        const left = Math.max(0, expiredAt - Math.floor(Date.now() / 1000));
        countEl.textContent = t("remaining", { s: left });
      }
    }
  }

  function fail(msg) {
    if (countdownTimer) clearInterval(countdownTimer);
    countdownTimer = null;
    formEl.hidden = false;
    countEl.hidden = true;
    nameEl.disabled = false;
    startEl.disabled = false;
    phase = "failed";
    applyIdleCopy();
    errEl.textContent = msg || t("failed");
  }

  function startCountdown(nextExpiredAt) {
    expiredAt = nextExpiredAt;
    const tick = () => {
      const left = Math.max(0, expiredAt - Math.floor(Date.now() / 1000));
      countEl.textContent = t("remaining", { s: left });
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

  langEl.onchange = (e) => {
    lang = e.target.value === "zh" ? "zh" : "en";
    localStorage.setItem(LANG_KEY, lang);
    applyIdleCopy();
  };
  applyIdleCopy();

  startEl.onclick = async () => {
    const name = (nameEl.value || '').trim();
    if (!name) {
      errEl.textContent = t("enterName");
      return;
    }
    errEl.textContent = '';
    startEl.disabled = true;
    nameEl.disabled = true;
    pairingName = name;
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
      phase = "waiting";
      applyIdleCopy();
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
      fail(e && e.message ? e.message : t("failed"));
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
.top{display:flex;align-items:flex-start;justify-content:space-between;gap:1rem;margin-bottom:1.5rem}
.lang-select{
  appearance:none;border:0;border-radius:999px;padding:.45rem .85rem;font:inherit;font-size:.82rem;font-weight:650;
  background:var(--md-sys-color-surface-container);color:var(--md-sys-color-on-surface-variant);cursor:pointer;flex:none;
}
</style>
</head>
<body>
<main class="app">
  <header class="top">
    <div>
      <h1 class="brand">SAIHUB</h1>
      <p class="subtitle" data-i18n="subtitle">Routing token for REST calls.</p>
    </div>
    <select class="lang-select" id="lang" aria-label="Language" title="Language">
      <option value="en">English</option>
      <option value="zh">中文</option>
    </select>
  </header>
  <section class="sheet">
    <h2 class="section-label" data-i18n="bearer">Bearer token</h2>
    <p class="hint empty" id="missing" data-i18n="missing">No routing token in this URL. Get one from the landing page.</p>
    <div class="token" id="tokenBox">
      <p class="hint" data-i18n-html="hint">Send this as <code>Authorization: Bearer</code> on REST requests.</p>
      <div class="row-head">
        <div class="field">
          <input id="token" type="text" readonly spellcheck="false" value=""/>
          <label for="token" data-i18n="token">Routing token</label>
        </div>
        <button class="copy" type="button" id="copy" data-i18n="copy">Copy</button>
      </div>
    </div>
  </section>
</main>
<script>
const I18N = {
  en: {
    language: "Language", subtitle: "Routing token for REST calls.", bearer: "Bearer token",
    missing: "No routing token in this URL. Get one from the landing page.",
    hint: "Send this as <code>Authorization: Bearer</code> on REST requests.",
    token: "Routing token", copy: "Copy", copied: "Copied",
  },
  zh: {
    language: "语言", subtitle: "用于 REST 调用的路由令牌。", bearer: "Bearer 令牌",
    missing: "此链接中没有路由令牌。请从落地页获取。",
    hint: "在 REST 请求中作为 <code>Authorization: Bearer</code> 发送。",
    token: "路由令牌", copy: "复制", copied: "已复制",
  },
};
const LANG_KEY = "saihub-lang";
function detectLang() {
  const saved = localStorage.getItem(LANG_KEY);
  if (saved === "en" || saved === "zh") return saved;
  return (navigator.language || "").toLowerCase().startsWith("zh") ? "zh" : "en";
}
let lang = detectLang();
function t(key) {
  const table = I18N[lang] || I18N.en;
  return table[key] != null ? table[key] : I18N.en[key] != null ? I18N.en[key] : key;
}
function applyI18n() {
  document.documentElement.lang = lang === "zh" ? "zh-CN" : "en";
  const langEl = document.getElementById("lang");
  if (langEl) {
    langEl.value = lang;
    langEl.setAttribute("aria-label", t("language"));
    langEl.title = t("language");
  }
  document.querySelectorAll("[data-i18n]").forEach((el) => { el.textContent = t(el.dataset.i18n); });
  document.querySelectorAll("[data-i18n-html]").forEach((el) => { el.innerHTML = t(el.dataset.i18nHtml); });
}
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
  button.textContent = t("copied");
  setTimeout(() => {
    button.classList.remove("done");
    button.textContent = t("copy");
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
document.getElementById("lang").onchange = (e) => {
  lang = e.target.value === "zh" ? "zh" : "en";
  localStorage.setItem(LANG_KEY, lang);
  applyI18n();
};
applyI18n();
</script>
</body>
</html>`;
}
