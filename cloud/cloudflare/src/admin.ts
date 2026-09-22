import type { Env } from "./env.ts";
import { isDigest, jsonError } from "./protocol.ts";
import { deleteWhitelist, insertWhitelist, listWhitelist } from "./whitelist.ts";

const ADMIN_WHITELIST_ITEM_RE = /^\/admin\/devices\/whitelist\/([123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]{26,33})$/;

export function isAdminPath(pathname: string): boolean {
  return pathname === "/admin" || pathname === "/admin/" || pathname === "/admin/devices/whitelist" || ADMIN_WHITELIST_ITEM_RE.test(pathname);
}

export function handleAdmin(request: Request, env: Env): Promise<Response> | Response {
  const pathname = new URL(request.url).pathname;
  if (pathname === "/admin" || pathname === "/admin/") {
    return handleAdminPage(request);
  }
  if (pathname === "/admin/devices/whitelist") {
    return handleAdminWhitelistCollection(request, env);
  }
  const item = ADMIN_WHITELIST_ITEM_RE.exec(pathname);
  if (item) {
    return handleAdminWhitelistItem(request, env, item[1]!);
  }
  return jsonError(404, "not found");
}

function handleAdminPage(request: Request): Response {
  if (request.method !== "GET") {
    return jsonError(405, "method not allowed");
  }
  return new Response(adminPageHtml(), {
    status: 200,
    headers: { "content-type": "text/html; charset=utf-8" },
  });
}

async function handleAdminWhitelistCollection(request: Request, env: Env): Promise<Response> {
  const auth = requireAdmin(request, env);
  if (auth) {
    return auth;
  }
  if (request.method === "GET") {
    const raw = new URL(request.url).searchParams.get("page");
    const page = raw === null || raw === "" ? 1 : Number.parseInt(raw, 10);
    if (!Number.isInteger(page) || page < 1) {
      return jsonError(400, "invalid page");
    }
    return Response.json(await listWhitelist(env, page));
  }
  if (request.method === "POST") {
    let body: unknown;
    try {
      body = await request.json();
    } catch {
      return jsonError(400, "invalid body");
    }
    if (
      typeof body !== "object" ||
      body === null ||
      !("digest" in body) ||
      typeof body.digest !== "string" ||
      !isDigest(body.digest)
    ) {
      return jsonError(400, "invalid digest");
    }
    const result = await insertWhitelist(env, body.digest);
    if (!result.created) {
      return jsonError(409, "digest already whitelisted");
    }
    return Response.json({ digest: body.digest, created_at: result.created_at }, { status: 201 });
  }
  return jsonError(405, "method not allowed");
}

async function handleAdminWhitelistItem(request: Request, env: Env, digest: string): Promise<Response> {
  const auth = requireAdmin(request, env);
  if (auth) {
    return auth;
  }
  if (request.method !== "DELETE") {
    return jsonError(405, "method not allowed");
  }
  if (!isDigest(digest)) {
    return jsonError(400, "invalid digest");
  }
  if (!(await deleteWhitelist(env, digest))) {
    return jsonError(404, "not found");
  }
  const stub = env.DEVICE.get(env.DEVICE.idFromName(digest));
  await stub.fetch(new Request("https://device/disconnect", { method: "POST" }));
  return Response.json({ digest });
}

function requireAdmin(request: Request, env: Env): Response | null {
  if (!env.ADMIN_TOKEN) {
    return jsonError(500, "admin token not configured");
  }
  const header = request.headers.get("authorization");
  if (!header || !header.toLowerCase().startsWith("bearer ")) {
    return jsonError(401, "missing or invalid admin token");
  }
  const token = header.slice("bearer ".length).trim();
  if (!token || !adminTokenEqual(token, env.ADMIN_TOKEN)) {
    return jsonError(401, "missing or invalid admin token");
  }
  return null;
}

function adminTokenEqual(left: string, right: string): boolean {
  if (left.length !== right.length) {
    return false;
  }
  let mismatch = 0;
  for (let i = 0; i < left.length; i += 1) {
    mismatch |= left.charCodeAt(i) ^ right.charCodeAt(i);
  }
  return mismatch === 0;
}

function adminPageHtml(): string {
  return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"/>
<title>SAIHUB Admin</title>
<style>
:root{
  --md-sys-color-primary:#006b54;
  --md-sys-color-on-primary:#ffffff;
  --md-sys-color-primary-container:#89f8d2;
  --md-sys-color-on-primary-container:#002117;
  --md-sys-color-surface:#f5fbf7;
  --md-sys-color-surface-container:#e9efeb;
  --md-sys-color-surface-container-high:#e3e9e5;
  --md-sys-color-surface-container-highest:#dee4e0;
  --md-sys-color-surface-container-lowest:#ffffff;
  --md-sys-color-on-surface:#171d1b;
  --md-sys-color-on-surface-variant:#3f4945;
  --md-sys-color-outline:#6f7975;
  --md-sys-color-error:#ba1a1a;
  --md-sys-color-error-container:#ffdad6;
  --md-sys-color-on-error-container:#410002;
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
.app{width:100%;max-width:40rem;margin:0 auto;padding:max(1.25rem, env(safe-area-inset-top)) 1rem max(2rem, env(safe-area-inset-bottom))}
.brand{margin:0;font-size:clamp(2rem,8vw,2.5rem);font-weight:700;letter-spacing:-.02em;line-height:1.1;color:var(--md-sys-color-primary)}
.subtitle{margin:.4rem 0 1.5rem;font-size:.95rem;line-height:1.4;color:var(--md-sys-color-on-surface-variant)}
.sheet{background:var(--md-sys-color-surface-container-lowest);border-radius:var(--md-sys-shape-corner-extra-large);box-shadow:var(--md-sys-elevation-1);padding:1.25rem 1rem 1.35rem}
.section-label{margin:0 0 .75rem .35rem;font-size:.72rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--md-sys-color-on-surface-variant)}
.hint{margin:0 0 1rem .35rem;font-size:.88rem;line-height:1.4;color:var(--md-sys-color-on-surface-variant)}
.error{margin:0 0 1rem;padding:.75rem 1rem;border-radius:var(--md-sys-shape-corner-small);background:var(--md-sys-color-error-container);color:var(--md-sys-color-on-error-container);font-size:.88rem}
.hidden{display:none!important}
.field{position:relative}
.field input{
  width:100%;appearance:none;border:1px solid var(--md-sys-color-outline);border-radius:var(--md-sys-shape-corner-small) var(--md-sys-shape-corner-small) 0 0;border-bottom-width:2px;
  background:var(--md-sys-color-surface-container-highest);color:var(--md-sys-color-on-surface);padding:1.35rem .85rem .55rem;font:inherit;font-size:.95rem;outline:none;
}
.field input.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.82rem}
.field input:focus{border-color:var(--md-sys-color-primary);background:var(--md-sys-color-surface-container-high)}
.field label{position:absolute;left:.85rem;top:.55rem;font-size:.72rem;font-weight:600;color:var(--md-sys-color-primary);pointer-events:none}
.row{display:flex;align-items:flex-end;gap:.65rem;margin-bottom:1rem}
.row .field{flex:1;min-width:0}
.btn{
  flex:none;min-height:2.75rem;border:0;border-radius:999px;padding:.55rem 1.15rem;font:inherit;font-size:.88rem;font-weight:650;
  background:var(--md-sys-color-primary);color:var(--md-sys-color-on-primary);cursor:pointer;box-shadow:var(--md-sys-elevation-1);
}
.btn:active{transform:scale(.97)}
.btn.danger{background:var(--md-sys-color-error)}
.btn.ghost{background:var(--md-sys-color-surface-container);color:var(--md-sys-color-on-surface);box-shadow:none}
.list{display:flex;flex-direction:column;gap:.65rem}
.item{display:flex;align-items:center;gap:.75rem;padding:.85rem;border-radius:12px;background:var(--md-sys-color-surface-container)}
.item .meta{flex:1;min-width:0}
.digest{margin:0;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.82rem;word-break:break-all}
.when{margin:.25rem 0 0;font-size:.75rem;color:var(--md-sys-color-on-surface-variant)}
.pager{display:flex;align-items:center;justify-content:space-between;gap:.75rem;margin-top:1rem}
.empty{margin:0;padding:1rem .35rem;color:var(--md-sys-color-on-surface-variant);font-size:.9rem}
</style>
</head>
<body>
<main class="app">
  <h1 class="brand">SAIHUB</h1>
  <p class="subtitle">Device whitelist</p>
  <section class="sheet" id="login">
    <h2 class="section-label">Admin</h2>
    <p class="hint">Enter the admin token to manage allowed devices.</p>
    <p class="error hidden" id="loginError"></p>
    <form id="loginForm">
      <div class="row">
        <div class="field">
          <input id="token" type="password" autocomplete="current-password" required/>
          <label for="token">Admin token</label>
        </div>
        <button class="btn" type="submit">Enter</button>
      </div>
    </form>
  </section>
  <section class="sheet hidden" id="panel">
    <h2 class="section-label">Add device</h2>
    <p class="error hidden" id="panelError"></p>
    <form id="addForm">
      <div class="row">
        <div class="field">
          <input id="digest" class="mono" type="text" spellcheck="false" required/>
          <label for="digest">Digest</label>
        </div>
        <button class="btn" type="submit">Add</button>
      </div>
    </form>
    <h2 class="section-label">Whitelist</h2>
    <div class="list" id="items"></div>
    <p class="empty hidden" id="empty">No devices yet.</p>
    <div class="pager">
      <button class="btn ghost" type="button" id="prev">Previous</button>
      <span id="pageLabel"></span>
      <button class="btn ghost" type="button" id="next">Next</button>
    </div>
  </section>
</main>
<script>
const TOKEN_KEY = "saihub-admin-token";
const loginEl = document.getElementById("login");
const panelEl = document.getElementById("panel");
const loginError = document.getElementById("loginError");
const panelError = document.getElementById("panelError");
const itemsEl = document.getElementById("items");
const emptyEl = document.getElementById("empty");
const pageLabel = document.getElementById("pageLabel");
const prevBtn = document.getElementById("prev");
const nextBtn = document.getElementById("next");
let page = 1;
let total = 0;
let pageSize = 50;

function token() {
  return sessionStorage.getItem(TOKEN_KEY) || "";
}

function showError(el, message) {
  el.textContent = message;
  el.classList.toggle("hidden", !message);
}

function setAuthed(authed) {
  loginEl.classList.toggle("hidden", authed);
  panelEl.classList.toggle("hidden", !authed);
}

async function api(path, options) {
  const headers = Object.assign({ authorization: "Bearer " + token() }, options && options.headers);
  if (options && options.body && !headers["content-type"]) headers["content-type"] = "application/json";
  const res = await fetch(path, Object.assign({}, options, { headers }));
  let body = null;
  const text = await res.text();
  if (text) {
    try { body = JSON.parse(text); } catch { body = { reason: text }; }
  }
  return { res, body };
}

function failureMessage(body, fallback) {
  return body && body.reason ? body.reason : fallback;
}

async function load() {
  showError(panelError, "");
  const { res, body } = await api("/admin/devices/whitelist?page=" + page);
  if (res.status === 401) {
    sessionStorage.removeItem(TOKEN_KEY);
    setAuthed(false);
    showError(loginError, "Invalid admin token.");
    return;
  }
  if (!res.ok) {
    const message = failureMessage(body, "Request failed (" + res.status + ")");
    if (panelEl.classList.contains("hidden")) showError(loginError, message);
    showError(panelError, message);
    return;
  }
  setAuthed(true);
  page = body.page;
  pageSize = body.pageSize;
  total = body.total;
  const items = body.items || [];
  itemsEl.innerHTML = "";
  emptyEl.classList.toggle("hidden", items.length !== 0);
  for (const item of items) {
    const row = document.createElement("div");
    row.className = "item";
    const meta = document.createElement("div");
    meta.className = "meta";
    const digest = document.createElement("p");
    digest.className = "digest";
    digest.textContent = item.digest;
    const when = document.createElement("p");
    when.className = "when";
    when.textContent = new Date(item.created_at).toISOString();
    meta.appendChild(digest);
    meta.appendChild(when);
    const del = document.createElement("button");
    del.className = "btn danger";
    del.type = "button";
    del.textContent = "Delete";
    del.addEventListener("click", () => remove(item.digest));
    row.appendChild(meta);
    row.appendChild(del);
    itemsEl.appendChild(row);
  }
  const pages = Math.max(1, Math.ceil(total / pageSize) || 1);
  pageLabel.textContent = "Page " + page + " of " + pages;
  prevBtn.disabled = page <= 1;
  nextBtn.disabled = page >= pages;
}

async function remove(digest) {
  showError(panelError, "");
  const { res, body } = await api("/admin/devices/whitelist/" + encodeURIComponent(digest), { method: "DELETE" });
  if (!res.ok) {
    showError(panelError, failureMessage(body, "Delete failed (" + res.status + ")"));
    return;
  }
  await load();
}

document.getElementById("loginForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  sessionStorage.setItem(TOKEN_KEY, document.getElementById("token").value);
  showError(loginError, "");
  page = 1;
  await load();
});

document.getElementById("addForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  showError(panelError, "");
  const digest = document.getElementById("digest").value.trim();
  const { res, body } = await api("/admin/devices/whitelist", { method: "POST", body: JSON.stringify({ digest }) });
  if (!res.ok) {
    showError(panelError, failureMessage(body, "Add failed (" + res.status + ")"));
    return;
  }
  document.getElementById("digest").value = "";
  page = 1;
  await load();
});

prevBtn.addEventListener("click", async () => { if (page > 1) { page -= 1; await load(); } });
nextBtn.addEventListener("click", async () => { page += 1; await load(); });

if (token()) load();
</script>
</body>
</html>`;
}
