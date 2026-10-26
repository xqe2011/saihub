import type { Env } from "./env.ts";
import { isDigest, jsonError } from "./protocol.ts";

export function handleLandingPage(request: Request, digest: string): Response {
  if (request.method !== "GET") {
    return jsonError(405, "method not allowed");
  }
  if (!isDigest(digest)) {
    return jsonError(400, "invalid digest");
  }
  const origin = new URL(request.url).origin;
  return new Response(landingPageHtml(origin, digest), {
    status: 200,
    headers: { "content-type": "text/html; charset=utf-8" },
  });
}

export function handleLandingOnline(request: Request, env: Env, digest: string): Promise<Response> | Response {
  if (request.method !== "GET") {
    return jsonError(405, "method not allowed");
  }
  if (!isDigest(digest)) {
    return jsonError(400, "invalid digest");
  }
  const id = env.DEVICE.idFromName(digest);
  const stub = env.DEVICE.get(id);
  return stub.fetch(new Request("https://device/online"));
}

function landingPageHtml(origin: string, digest: string): string {
  const mcpUrl = `${origin}/device/${digest}/mcp`;
  const restUrl = `${origin}/device/${digest}`;
  const openapiUrl = `${origin}/device/${digest}/openapi.json`;
  const onlineUrl = `/cloud/landing/${digest}/online`;
  const echoUrl = `${origin}/cloud/oauth/echo`;
  const tokenUrl = `/cloud/oauth/redirect?devicePublicKeyDigest=${encodeURIComponent(digest)}&redirect_uri=${encodeURIComponent(echoUrl)}&resource=${encodeURIComponent(restUrl)}`;
  return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"/>
<title>SAIHUB</title>
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
  --md-sys-color-error:#ba1a1a;
  --md-sys-color-error-container:#ffdad6;
  --md-sys-color-on-error-container:#410002;
  --md-sys-shape-corner-small:8px;
  --md-sys-shape-corner-large:16px;
  --md-sys-shape-corner-extra-large:28px;
  --md-sys-elevation-1:0 1px 2px rgba(0,0,0,.16),0 1px 3px 1px rgba(0,0,0,.08);
  --md-sys-elevation-2:0 1px 2px rgba(0,0,0,.16),0 2px 6px 2px rgba(0,0,0,.08);
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
.top{
  display:flex;
  align-items:flex-start;
  justify-content:space-between;
  gap:1rem;
  margin-bottom:1.5rem;
}
.brand{margin:0;font-size:clamp(2rem,8vw,2.5rem);font-weight:700;letter-spacing:-.02em;line-height:1.1;color:var(--md-sys-color-primary)}
.subtitle{margin:.4rem 0 0;font-size:.95rem;line-height:1.4;color:var(--md-sys-color-on-surface-variant)}
.pill{
  flex:none;
  display:inline-flex;
  align-items:center;
  gap:.4rem;
  margin-top:.35rem;
  padding:.35rem .75rem;
  border-radius:999px;
  font-size:.72rem;
  font-weight:700;
  letter-spacing:.04em;
  text-transform:uppercase;
  background:var(--md-sys-color-surface-container);
  color:var(--md-sys-color-on-surface-variant);
}
.pill .dot{width:.5rem;height:.5rem;border-radius:999px;background:currentColor}
.pill.online{background:var(--md-sys-color-primary-container);color:var(--md-sys-color-on-primary-container)}
.pill.offline{background:var(--md-sys-color-error-container);color:var(--md-sys-color-on-error-container)}
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
.rows{display:flex;flex-direction:column;gap:1rem}
.row{display:flex;flex-direction:column;gap:.45rem}
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
.or{
  display:flex;
  align-items:center;
  gap:.75rem;
  margin:1.15rem .35rem;
  font-size:.72rem;
  font-weight:700;
  letter-spacing:.14em;
  text-transform:uppercase;
  color:var(--md-sys-color-on-surface-variant);
}
.or::before,.or::after{
  content:"";
  flex:1;
  height:1px;
  background:var(--md-sys-color-outline);
  opacity:.35;
}
.cta{
  display:flex;
  align-items:center;
  justify-content:center;
  width:100%;
  min-height:2.75rem;
  margin-top:.25rem;
  border-radius:999px;
  padding:.7rem 1.25rem;
  font:inherit;
  font-size:.92rem;
  font-weight:650;
  text-decoration:none;
  background:var(--md-sys-color-primary);
  color:var(--md-sys-color-on-primary);
  box-shadow:var(--md-sys-elevation-1);
}
.cta:active{transform:scale(.985)}
.top-right{display:flex;flex-direction:column;align-items:flex-end;gap:.5rem;flex:none}
.lang-select{
  appearance:none;border:0;border-radius:999px;padding:.45rem .85rem;font:inherit;font-size:.82rem;font-weight:650;
  background:var(--md-sys-color-surface-container);color:var(--md-sys-color-on-surface-variant);cursor:pointer;
}
.lang-select:focus{outline:2px solid var(--md-sys-color-primary);outline-offset:2px}
</style>
</head>
<body>
<main class="app">
  <header class="top">
    <div>
      <h1 class="brand">SAIHUB</h1>
      <p class="subtitle" data-i18n="subtitle">Give your agent a hand.</p>
    </div>
    <div class="top-right">
      <select class="lang-select" id="lang" aria-label="Language" title="Language">
        <option value="en">English</option>
        <option value="zh">中文</option>
      </select>
      <div class="pill" id="status" role="status" aria-live="polite"><span class="dot"></span><span id="statusText">Checking</span></div>
    </div>
  </header>
  <section class="sheet">
    <h2 class="section-label" data-i18n="mcp">MCP</h2>
    <p class="hint" data-i18n="mcpHint">Paste the MCP URL into Cursor, Claude Code, or Codex CLI. The client completes OAuth in the browser.</p>
    <div class="rows">
      <div class="row">
        <div class="row-head">
          <div class="field">
            <input id="mcp" type="text" readonly spellcheck="false" value="${escapeAttr(mcpUrl)}"/>
            <label for="mcp" data-i18n="mcpUrl">MCP URL</label>
          </div>
          <button class="copy" type="button" data-copy="mcp" data-i18n="copy">Copy</button>
        </div>
      </div>
    </div>
    <div class="or" data-i18n="or">OR</div>
    <h2 class="section-label" data-i18n="rest">REST + OpenAPI</h2>
    <div class="rows">
      <div class="row">
        <div class="row-head">
          <div class="field">
            <input id="rest" type="text" readonly spellcheck="false" value="${escapeAttr(restUrl)}"/>
            <label for="rest" data-i18n="restBase">REST API base</label>
          </div>
          <button class="copy" type="button" data-copy="rest" data-i18n="copy">Copy</button>
        </div>
      </div>
      <div class="row">
        <div class="row-head">
          <div class="field">
            <input id="openapi" type="text" readonly spellcheck="false" value="${escapeAttr(openapiUrl)}"/>
            <label for="openapi">openapi.json</label>
          </div>
          <button class="copy" type="button" data-copy="openapi" data-i18n="copy">Copy</button>
        </div>
      </div>
    </div>
    <p class="hint" data-i18n="restHint">REST calls need a routing token first.</p>
    <a class="cta" href="${escapeAttr(tokenUrl)}" data-i18n="getToken">Get routing token</a>
  </section>
</main>
<script>
const onlineUrl = ${JSON.stringify(onlineUrl)};
const I18N = {
  en: {
    subtitle: "Give your agent a hand.", language: "Language", mcp: "MCP",
    mcpHint: "Paste the MCP URL into Cursor, Claude Code, or Codex CLI. The client completes OAuth in the browser.",
    mcpUrl: "MCP URL", or: "OR", rest: "REST + OpenAPI", restBase: "REST API base",
    restHint: "REST calls need a routing token first.", getToken: "Get routing token",
    copy: "Copy", copied: "Copied", online: "Online", offline: "Offline", checking: "Checking",
  },
  zh: {
    subtitle: "给智能体一只手。", language: "语言", mcp: "MCP",
    mcpHint: "将 MCP 链接粘贴到 Cursor、Claude Code 或 Codex CLI。客户端会在浏览器中完成 OAuth。",
    mcpUrl: "MCP 链接", or: "或", rest: "REST + OpenAPI", restBase: "REST API 根地址",
    restHint: "REST 调用需先获取路由令牌。", getToken: "获取路由令牌",
    copy: "复制", copied: "已复制", online: "在线", offline: "离线", checking: "检查中",
  },
};
const LANG_KEY = "saihub-lang";
function detectLang() {
  const saved = localStorage.getItem(LANG_KEY);
  if (saved === "en" || saved === "zh") return saved;
  const nav = (navigator.language || "").toLowerCase();
  return nav.startsWith("zh") ? "zh" : "en";
}
let lang = detectLang();
let onlineState = null;
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
  document.querySelectorAll("[data-i18n]").forEach((el) => {
    el.textContent = t(el.dataset.i18n);
  });
  setOnline(onlineState);
}
const statusEl = document.getElementById("status");
const statusText = document.getElementById("statusText");

function setOnline(online) {
  onlineState = online;
  statusEl.classList.toggle("online", online === true);
  statusEl.classList.toggle("offline", online === false);
  statusText.textContent = online === true ? t("online") : online === false ? t("offline") : t("checking");
}

async function refreshOnline() {
  try {
    const res = await fetch(onlineUrl, { cache: "no-store" });
    const body = await res.json();
    setOnline(body && body.online === true);
  } catch {
    setOnline(false);
  }
}

function copyFrom(id, button) {
  const input = document.getElementById(id);
  input.focus();
  input.select();
  input.setSelectionRange(0, input.value.length);
  const done = () => {
    button.classList.add("done");
    button.textContent = t("copied");
    setTimeout(() => {
      button.classList.remove("done");
      button.textContent = t("copy");
    }, 1500);
  };
  const fallback = () => {
    try {
      if (document.execCommand("copy")) done();
    } catch {}
  };
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(input.value).then(done, fallback);
  } else {
    fallback();
  }
}

document.querySelectorAll("[data-copy]").forEach((button) => {
  button.addEventListener("click", () => copyFrom(button.getAttribute("data-copy"), button));
});
document.getElementById("lang").onchange = (e) => {
  lang = e.target.value === "zh" ? "zh" : "en";
  localStorage.setItem(LANG_KEY, lang);
  applyI18n();
};
applyI18n();
refreshOnline();
setInterval(refreshOnline, 5000);
</script>
</body>
</html>`;
}

function escapeAttr(value: string): string {
  return value.replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;");
}
