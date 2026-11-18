#include "html.hpp"

#include <yyjson.h>
#include <zlib.h>

#include <cstdlib>
#include <cstring>

namespace saihub {

namespace {

std::string escape_attr(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '&') out += "&amp;";
        else if (c == '"') out += "&quot;";
        else if (c == '<') out += "&lt;";
        else out.push_back(c);
    }
    return out;
}

std::string url_encode(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

std::string json_stringify_str(std::string_view value) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* val = yyjson_mut_strn(doc, value.data(), value.size());
    yyjson_mut_doc_set_root(doc, val);
    size_t len = 0;
    char* raw = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!raw) return "\"\"";
    std::string out(raw, len);
    free(raw);
    return out;
}

} // namespace

std::string landing_page_html(std::string_view origin, std::string_view digest) {
    std::string mcp_url = std::string(origin) + "/device/" + std::string(digest) + "/mcp";
    std::string rest_url = std::string(origin) + "/device/" + std::string(digest);
    std::string openapi_url = std::string(origin) + "/device/" + std::string(digest) + "/openapi.json";
    std::string online_url = "/cloud/landing/" + std::string(digest) + "/online";
    std::string echo_url = std::string(origin) + "/cloud/oauth/echo";
    std::string token_url = "/cloud/oauth/redirect?devicePublicKeyDigest=" + url_encode(digest) +
                            "&redirect_uri=" + url_encode(echo_url) +
                            "&resource=" + url_encode(rest_url);

    std::string html;
    html.reserve(10500);
    html += R"HTML(<!DOCTYPE html>
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
            <input id="mcp" type="text" readonly spellcheck="false" value=")HTML";
    html += escape_attr(mcp_url);
    html += R"HTML("/>
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
            <input id="rest" type="text" readonly spellcheck="false" value=")HTML";
    html += escape_attr(rest_url);
    html += R"HTML("/>
            <label for="rest" data-i18n="restBase">REST API base</label>
          </div>
          <button class="copy" type="button" data-copy="rest" data-i18n="copy">Copy</button>
        </div>
      </div>
      <div class="row">
        <div class="row-head">
          <div class="field">
            <input id="openapi" type="text" readonly spellcheck="false" value=")HTML";
    html += escape_attr(openapi_url);
    html += R"HTML("/>
            <label for="openapi">openapi.json</label>
          </div>
          <button class="copy" type="button" data-copy="openapi" data-i18n="copy">Copy</button>
        </div>
      </div>
    </div>
    <p class="hint" data-i18n="restHint">REST calls need a routing token first.</p>
    <a class="cta" href=")HTML";
    html += escape_attr(token_url);
    html += R"HTML(" data-i18n="getToken">Get routing token</a>
  </section>
</main>
<script>
const onlineUrl = )HTML";
    html += json_stringify_str(online_url);
    html += R"HTML(;
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
</html>)HTML";
    return html;
}

std::string pairing_page_html(std::string_view digest, std::string_view redirect_uri, std::string_view state) {
    std::string html;
    html.reserve(6000);
    html += R"HTML(<!DOCTYPE html>
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
  const digest = )HTML";
    html += json_stringify_str(digest);
    html += ";\n  const redirectUri = ";
    html += json_stringify_str(redirect_uri);
    html += ";\n  const state = ";
    html += json_stringify_str(state);
    html += R"HTML(;
  const I18N = {
    en: {
      language: "Language", title: "Name this client",
      detail: "Choose a name so you can revoke this grant later on the device.",
      pair: "Pair", failed: "Pairing failed", failedDetail: "Choose a name so you can revoke this grant later on the device.",
      enterName: "Enter a name.", holdBoot: "Hold BOOT for 3 seconds",
      grantDetail: "This approval will create grant `{name}`.", remaining: "{s}s remaining",
    },
    zh: {
      language: "语言", title: "为此客户端命名",
      detail: "选择一个名称，以便稍后在设备上撤销此授权。",
      pair: "配对", failed: "配对失败", failedDetail: "选择一个名称，以便稍后在设备上撤销此授权。",
      enterName: "请输入名称。", holdBoot: "按住 BOOT 三秒",
      grantDetail: "确认后将创建授权 `{name}`。", remaining: "剩余 {s} 秒",
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
    if (vars) Object.keys(vars).forEach((k) => { s = s.replace(new RegExp("\\{" + k + "\\}", "g"), String(vars[k])); });
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
</html>)HTML";
    return html;
}

std::string_view oauth_echo_page_html() {
    static constexpr std::string_view kHtml = R"HTML(<!DOCTYPE html>
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
</html>)HTML";
    return kHtml;
}

std::string_view admin_page_html() {
    static constexpr std::string_view kHtml = R"HTML(<!DOCTYPE html>
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
.stack{display:flex;flex-direction:column;gap:.65rem;margin-bottom:1rem}
.field select,.field input[type=file]{
  width:100%;appearance:none;border:1px solid var(--md-sys-color-outline);border-radius:var(--md-sys-shape-corner-small) var(--md-sys-shape-corner-small) 0 0;border-bottom-width:2px;
  background:var(--md-sys-color-surface-container-highest);color:var(--md-sys-color-on-surface);padding:1.35rem .85rem .55rem;font:inherit;font-size:.95rem;outline:none;
}
.field select:focus,.field input[type=file]:focus{border-color:var(--md-sys-color-primary);background:var(--md-sys-color-surface-container-high)}
.split{margin-top:1.75rem}
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
      <p class="subtitle" data-i18n="subtitle">Device whitelist and file cache</p>
    </div>
    <select class="lang-select" id="lang" aria-label="Language" title="Language">
      <option value="en">English</option>
      <option value="zh">中文</option>
    </select>
  </header>
  <section class="sheet" id="login">
    <h2 class="section-label" data-i18n="admin">Admin</h2>
    <p class="hint" data-i18n="loginHint">Enter the admin token to manage allowed devices.</p>
    <p class="error hidden" id="loginError"></p>
    <form id="loginForm">
      <div class="row">
        <div class="field">
          <input id="token" type="password" autocomplete="current-password" required/>
          <label for="token" data-i18n="adminToken">Admin token</label>
        </div>
        <button class="btn" type="submit" data-i18n="enter">Enter</button>
      </div>
    </form>
  </section>
  <section class="sheet hidden" id="panel">
    <h2 class="section-label" data-i18n="addDevice">Add device</h2>
    <p class="error hidden" id="panelError"></p>
    <form id="addForm">
      <div class="row">
        <div class="field">
          <input id="digest" class="mono" type="text" spellcheck="false" required/>
          <label for="digest" data-i18n="digest">Digest</label>
        </div>
        <button class="btn" type="submit" data-i18n="add">Add</button>
      </div>
    </form>
    <h2 class="section-label" data-i18n="whitelist">Whitelist</h2>
    <div class="list" id="items"></div>
    <p class="empty hidden" id="empty" data-i18n="noDevices">No devices yet.</p>
    <div class="pager">
      <button class="btn ghost" type="button" id="prev" data-i18n="previous">Previous</button>
      <span id="pageLabel"></span>
      <button class="btn ghost" type="button" id="next" data-i18n="next">Next</button>
    </div>
    <h2 class="section-label split" data-i18n="fileCache">File cache</h2>
    <p class="hint" data-i18n="cacheHint">Upload mcp.json or openapi.json for a firmware version. Cached tools/list and OpenAPI skip the device.</p>
    <form id="cacheForm">
      <div class="stack">
        <div class="field">
          <input id="cacheVersion" class="mono" type="text" spellcheck="false" required maxlength="64"/>
          <label for="cacheVersion" data-i18n="version">Version</label>
        </div>
        <div class="field">
          <select id="cacheFilename" required>
            <option value="mcp.json">mcp.json</option>
            <option value="openapi.json">openapi.json</option>
          </select>
          <label for="cacheFilename" data-i18n="filename">Filename</label>
        </div>
        <div class="field">
          <input id="cacheFile" type="file" accept="application/json,.json" required/>
          <label for="cacheFile" data-i18n="jsonFile">JSON file</label>
        </div>
      </div>
      <button class="btn" type="submit" data-i18n="upload">Upload</button>
    </form>
    <div class="list" id="cacheItems"></div>
    <p class="empty hidden" id="cacheEmpty" data-i18n="noCache">No cached files yet.</p>
    <div class="pager">
      <button class="btn ghost" type="button" id="cachePrev" data-i18n="previous">Previous</button>
      <span id="cachePageLabel"></span>
      <button class="btn ghost" type="button" id="cacheNext" data-i18n="next">Next</button>
    </div>
  </section>
</main>
<script>
const I18N = {
  en: {
    language: "Language", subtitle: "Device whitelist and file cache", admin: "Admin",
    loginHint: "Enter the admin token to manage allowed devices.", adminToken: "Admin token", enter: "Enter",
    addDevice: "Add device", digest: "Digest", add: "Add", whitelist: "Whitelist", noDevices: "No devices yet.",
    previous: "Previous", next: "Next", fileCache: "File cache",
    cacheHint: "Upload mcp.json or openapi.json for a firmware version. Cached tools/list and OpenAPI skip the device.",
    version: "Version", filename: "Filename", jsonFile: "JSON file", upload: "Upload", noCache: "No cached files yet.",
    delete: "Delete", pageOf: "Page {page} of {pages}", bytes: "{n} bytes",
    invalidToken: "Invalid admin token.", chooseJson: "Choose a JSON file.", mustJson: "File must be JSON.",
  },
  zh: {
    language: "语言", subtitle: "设备白名单与文件缓存", admin: "管理",
    loginHint: "输入管理令牌以管理允许的设备。", adminToken: "管理令牌", enter: "进入",
    addDevice: "添加设备", digest: "摘要", add: "添加", whitelist: "白名单", noDevices: "暂无设备。",
    previous: "上一页", next: "下一页", fileCache: "文件缓存",
    cacheHint: "为固件版本上传 mcp.json 或 openapi.json。缓存命中时 tools/list 与 OpenAPI 不必访问设备。",
    version: "版本", filename: "文件名", jsonFile: "JSON 文件", upload: "上传", noCache: "暂无缓存文件。",
    delete: "删除", pageOf: "第 {page} / {pages} 页", bytes: "{n} 字节",
    invalidToken: "管理令牌无效。", chooseJson: "请选择 JSON 文件。", mustJson: "文件必须是 JSON。",
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
  if (vars) Object.keys(vars).forEach((k) => { s = s.replace(new RegExp("\\{" + k + "\\}", "g"), String(vars[k])); });
  return s;
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
}
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
const cacheItemsEl = document.getElementById("cacheItems");
const cacheEmptyEl = document.getElementById("cacheEmpty");
const cachePageLabel = document.getElementById("cachePageLabel");
const cachePrevBtn = document.getElementById("cachePrev");
const cacheNextBtn = document.getElementById("cacheNext");
let page = 1;
let total = 0;
let pageSize = 50;
let cachePage = 1;
let cacheTotal = 0;
let cachePageSize = 50;

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
    showError(loginError, t("invalidToken"));
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
    del.textContent = t("delete");
    del.addEventListener("click", () => remove(item.digest));
    row.appendChild(meta);
    row.appendChild(del);
    itemsEl.appendChild(row);
  }
  const pages = Math.max(1, Math.ceil(total / pageSize) || 1);
  pageLabel.textContent = t("pageOf", { page, pages });
  prevBtn.disabled = page <= 1;
  nextBtn.disabled = page >= pages;
  await loadCache();
}

async function loadCache() {
  const { res, body } = await api("/admin/file-cache?page=" + cachePage);
  if (res.status === 401) {
    sessionStorage.removeItem(TOKEN_KEY);
    setAuthed(false);
    showError(loginError, t("invalidToken"));
    return;
  }
  if (!res.ok) {
    showError(panelError, failureMessage(body, "Request failed (" + res.status + ")"));
    return;
  }
  cachePage = body.page;
  cachePageSize = body.pageSize;
  cacheTotal = body.total;
  const items = body.items || [];
  cacheItemsEl.innerHTML = "";
  cacheEmptyEl.classList.toggle("hidden", items.length !== 0);
  for (const item of items) {
    const row = document.createElement("div");
    row.className = "item";
    const meta = document.createElement("div");
    meta.className = "meta";
    const name = document.createElement("p");
    name.className = "digest";
    name.textContent = item.version + " / " + item.filename;
    const when = document.createElement("p");
    when.className = "when";
    when.textContent = t("bytes", { n: item.bytes });
    meta.appendChild(name);
    meta.appendChild(when);
    const del = document.createElement("button");
    del.className = "btn danger";
    del.type = "button";
    del.textContent = t("delete");
    del.addEventListener("click", () => removeCache(item.version, item.filename));
    row.appendChild(meta);
    row.appendChild(del);
    cacheItemsEl.appendChild(row);
  }
  const pages = Math.max(1, Math.ceil(cacheTotal / cachePageSize) || 1);
  cachePageLabel.textContent = t("pageOf", { page: cachePage, pages });
  cachePrevBtn.disabled = cachePage <= 1;
  cacheNextBtn.disabled = cachePage >= pages;
}

async function removeCache(version, filename) {
  showError(panelError, "");
  const path = "/admin/file-cache/" + encodeURIComponent(version) + "/" + encodeURIComponent(filename);
  const { res, body } = await api(path, { method: "DELETE" });
  if (!res.ok) {
    showError(panelError, failureMessage(body, "Delete failed (" + res.status + ")"));
    return;
  }
  await loadCache();
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

document.getElementById("cacheForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  showError(panelError, "");
  const version = document.getElementById("cacheVersion").value.trim();
  const filename = document.getElementById("cacheFilename").value;
  const file = document.getElementById("cacheFile").files[0];
  if (!file) {
    showError(panelError, t("chooseJson"));
    return;
  }
  let content;
  try { content = JSON.parse(await file.text()); }
  catch {
    showError(panelError, t("mustJson"));
    return;
  }
  const { res, body } = await api("/admin/file-cache", { method: "POST", body: JSON.stringify({ version, filename, content }) });
  if (!res.ok) {
    showError(panelError, failureMessage(body, "Upload failed (" + res.status + ")"));
    return;
  }
  document.getElementById("cacheFile").value = "";
  cachePage = 1;
  await loadCache();
});

prevBtn.addEventListener("click", async () => { if (page > 1) { page -= 1; await load(); } });
nextBtn.addEventListener("click", async () => { page += 1; await load(); });
cachePrevBtn.addEventListener("click", async () => { if (cachePage > 1) { cachePage -= 1; await loadCache(); } });
cacheNextBtn.addEventListener("click", async () => { cachePage += 1; await loadCache(); });

document.getElementById("lang").onchange = (e) => {
  lang = e.target.value === "zh" ? "zh" : "en";
  localStorage.setItem(LANG_KEY, lang);
  applyI18n();
  if (token()) load();
};
applyI18n();
if (token()) load();
</script>
</body>
</html>)HTML";
    return kHtml;
}

namespace {

std::string gzip_compress_with_level(std::string_view input, int level) {
    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    std::string out;
    out.resize(deflateBound(&zs, zs.avail_in) + 16);
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());

    int rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&zs);
        return {};
    }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

} // namespace

std::string gzip_compress(std::string_view input) {
    return gzip_compress_with_level(input, Z_DEFAULT_COMPRESSION);
}

std::string landing_page_gzip(std::string_view origin, std::string_view digest) {
    return gzip_compress(landing_page_html(origin, digest));
}

std::string pairing_page_gzip(std::string_view digest, std::string_view redirect_uri, std::string_view state) {
    return gzip_compress(pairing_page_html(digest, redirect_uri, state));
}

std::string_view oauth_echo_page_gzip() {
    static const std::string kGzip = gzip_compress_with_level(oauth_echo_page_html(), Z_BEST_COMPRESSION);
    return kGzip;
}

std::string_view admin_page_gzip() {
    static const std::string kGzip = gzip_compress_with_level(admin_page_html(), Z_BEST_COMPRESSION);
    return kGzip;
}

} // namespace saihub
