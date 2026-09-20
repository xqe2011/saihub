import { URL } from "node:url";

const DEFAULT_TARGET = "192.168.88.160";
const OUTPUT_A = 6;
const OUTPUT_B = 7;
const UART_ID = 0;
const REQUEST_TIMEOUT_MS = Number.parseInt(process.env.SAIHUB_REQUEST_TIMEOUT_MS ?? "5000", 10);
const UART_TIMEOUT_MS = Number.parseInt(process.env.SAIHUB_UART_TIMEOUT_MS ?? "1500", 10);
const SOAK_ITERATIONS = Number.parseInt(process.env.SAIHUB_SOAK_ITERATIONS ?? "3", 10);
const EXPECTED_MCP_TOOLS = [
  "list_pins",
  "configure_pins",
  "get_pin_levels",
  "set_pin_levels",
  "pulse_pins",
  "get_pin_pwms",
  "set_pin_pwms",
  "trace_pins",
  "get_output_power_state",
  "set_output_power_state",
  "list_uarts",
  "configure_uart",
  "uart_transmit",
  "uart_receive",
  "uart_flush",
  "create_lock",
  "renew_lock",
  "delete_lock",
  "run_script",
] as const;

type JsonObject = Record<string, unknown>;
type PinMode = "disable" | "digitalInput" | "digitalOutput" | "digitalInputOutput" | "pwmOutput";
type PinState = {
  pin: number;
  mode: PinMode;
  openDrain: boolean;
  pullUp: boolean;
  pullDown: boolean;
  level: 0 | 1;
};
type PwmState = { frequency: number; duty: number; time: number };
type UartConfig = {
  id?: number;
  enable: boolean;
  baudRate: number;
  dataBits: 5 | 6 | 7 | 8;
  parity: "none" | "even" | "odd";
  stopBits: 1 | 2;
  encoding: "utf8" | "byte";
  pins: { rx: number | null; tx: number | null };
};
type HttpResult = { status: number; headers: Headers; text: string };
type McpEnvelope = { jsonrpc?: string; id?: number; result?: JsonObject; error?: JsonObject };

type Snapshot = {
  pins: Map<number, PinState>;
  pwm: Map<number, PwmState>;
  uart: UartConfig;
  power: Record<"3v3" | "5v", boolean>;
};

let nextMcpId = 1;
const createdRestLocks = new Set<string>();
const createdMcpLocks = new Set<string>();
let snapshot: Snapshot | undefined;
let mutationStarted = false;
const deferredFailures: string[] = [];

function fail(message: string): never {
  throw new Error(message);
}

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) fail(message);
}

function check(condition: unknown, message: string): void {
  if (!condition) {
    deferredFailures.push(message);
    log(`continuing after failure: ${message}`);
  }
}

function isObject(value: unknown): value is JsonObject {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function normalizeTarget(value: string): URL {
  const withProtocol = /^https?:\/\//i.test(value) ? value : `http://${value}`;
  const url = new URL(withProtocol);
  url.pathname = url.pathname.replace(/\/$/, "");
  return url;
}

const target = normalizeTarget(process.env.SAIHUB_TARGET ?? process.env.SAIHUB_IP ?? DEFAULT_TARGET);
const routingToken = process.env.SAIHUB_ROUTING_TOKEN?.trim();

function targetUrl(path: string): URL {
  const basePath = `${target.pathname.replace(/\/$/, "")}/`;
  return new URL(path.replace(/^\//, ""), `${target.protocol}//${target.host}${basePath}`);
}

function log(message: string): void {
  console.log(`[hardware-smoke] ${message}`);
}

async function rawRequest(method: string, path: string, body?: unknown, extraHeaders: Record<string, string> = {}): Promise<HttpResult> {
  if (method === "GET" && body !== undefined) {
    const url = targetUrl(path);
    for (const [key, value] of Object.entries(body as Record<string, unknown>)) {
      if (Array.isArray(value)) for (const item of value) url.searchParams.append(key, String(item));
      else url.searchParams.set(key, String(value));
    }
    path += url.search;
    body = undefined;
  }
  const serialized = body === undefined ? undefined : JSON.stringify(body);
  const headers: Record<string, string> = { Accept: "application/json", ...extraHeaders };
  if (routingToken) headers.Authorization = `Bearer ${routingToken}`;
  if (serialized !== undefined) headers["Content-Type"] = "application/json";
  const args = [
    "curl", "--noproxy", "*", "--silent", "--show-error", "--max-time", String(REQUEST_TIMEOUT_MS / 1000),
    "--request", method, "--url", targetUrl(path).toString(), "--write-out", "\n%{http_code}",
  ];
  for (const [name, value] of Object.entries(headers)) args.push("--header", `${name}: ${value}`);
  if (serialized !== undefined) args.push("--data-binary", serialized);
  const process = Bun.spawn(args, { stdout: "pipe", stderr: "pipe" });
  const [stdout, stderr, exitCode] = await Promise.all([
    new Response(process.stdout).text(), new Response(process.stderr).text(), process.exited,
  ]);
  if (exitCode !== 0) fail(`${method} ${path} transport failed: ${stderr.trim() || `curl exited ${exitCode}`}`);
  const separator = stdout.lastIndexOf("\n");
  assert(separator >= 0, `${method} ${path} transport omitted HTTP status`);
  const status = Number.parseInt(stdout.slice(separator + 1), 10);
  assert(Number.isInteger(status), `${method} ${path} transport returned invalid HTTP status`);
  return { status, headers: new Headers(), text: stdout.slice(0, separator) };
}

function parseJson(result: HttpResult, label: string): unknown {
  try {
    return JSON.parse(result.text) as unknown;
  } catch {
    fail(`${label} returned invalid JSON: ${result.text.slice(0, 240)}`);
  }
}

async function requestJson<T>(
  method: string,
  path: string,
  body?: unknown,
  expectedStatus = 200,
  extraHeaders: Record<string, string> = {},
): Promise<T> {
  const result = await rawRequest(method, path, body, extraHeaders);
  if (result.status !== expectedStatus) {
    let reason = result.text;
    try {
      const parsed = JSON.parse(result.text) as JsonObject;
      if (typeof parsed.reason === "string") reason = parsed.reason;
    } catch {
      // Preserve the response text for diagnostics.
    }
    fail(`${method} ${path} returned ${result.status}, expected ${expectedStatus}: ${reason}`);
  }
  if (expectedStatus === 204 || expectedStatus === 202) return undefined as T;
  return parseJson(result, `${method} ${path}`) as T;
}

async function expectStatus(method: string, path: string, expected: number, body?: unknown): Promise<void> {
  const result = await rawRequest(method, path, body);
  assert(result.status === expected, `${method} ${path} returned ${result.status}, expected ${expected}: ${result.text}`);
}

async function mcpRequest(method: string, params?: JsonObject, notification = false): Promise<McpEnvelope | undefined> {
  const payload: JsonObject = { jsonrpc: "2.0", method };
  if (!notification) payload.id = nextMcpId++;
  if (params !== undefined) payload.params = params;
  const result = await rawRequest("POST", "/mcp", payload, {
    Accept: "application/json, text/event-stream",
    "MCP-Protocol-Version": "2025-06-18",
  });
  if (notification) {
    assert(result.status === 202, `MCP notification ${method} returned ${result.status}, expected 202`);
    assert(result.text.length === 0, `MCP notification ${method} returned a non-empty body`);
    return undefined;
  }
  assert(result.status === 200, `MCP ${method} returned HTTP ${result.status}: ${result.text}`);
  const envelope = parseJson(result, `MCP ${method}`);
  assert(isObject(envelope), `MCP ${method} returned a non-object envelope`);
  const typed = envelope as McpEnvelope;
  if (typed.error !== undefined) fail(`MCP ${method} returned JSON-RPC error: ${JSON.stringify(typed.error)}`);
  assert(isObject(typed.result), `MCP ${method} omitted result`);
  return typed;
}

async function mcpTool<T>(name: string, args: JsonObject): Promise<T> {
  const envelope = await mcpRequest("tools/call", { name, arguments: args });
  const result = envelope?.result;
  assert(isObject(result), `MCP tool ${name} omitted result`);
  const content = result.content;
  assert(Array.isArray(content) && content.length > 0, `MCP tool ${name} returned no content`);
  const first = content[0];
  assert(isObject(first) && first.type === "text" && typeof first.text === "string", `MCP tool ${name} returned invalid content`);
  let payload: unknown;
  try {
    payload = JSON.parse(first.text) as unknown;
  } catch {
    fail(`MCP tool ${name} returned invalid JSON text: ${first.text}`);
  }
  if (result.isError === true) {
    const reason = isObject(payload) && typeof payload.reason === "string" ? payload.reason : JSON.stringify(payload);
    fail(`MCP tool ${name} failed: ${reason}`);
  }
  return payload as T;
}

function pinConfig(state: PinState): JsonObject {
  return { mode: state.mode, openDrain: state.openDrain, pullUp: state.pullUp, pullDown: state.pullDown };
}

function uartBody(config: UartConfig, enable = config.enable): JsonObject {
  return {
    enable,
    baudRate: config.baudRate,
    dataBits: config.dataBits,
    parity: config.parity,
    stopBits: config.stopBits,
    encoding: config.encoding,
    pins: config.pins,
  };
}

const disabledUart: UartConfig = {
  enable: false,
  baudRate: 115200,
  dataBits: 8,
  parity: "none",
  stopBits: 1,
  encoding: "byte",
  pins: { rx: null, tx: null },
};

function loopbackUart(
  rx: number,
  tx: number,
  baudRate = 115200,
  dataBits: 5 | 6 | 7 | 8 = 8,
  parity: "none" | "even" | "odd" = "none",
  stopBits: 1 | 2 = 1,
  encoding: "utf8" | "byte" = "byte",
): JsonObject {
  return { enable: true, baudRate, dataBits, parity, stopBits, encoding, pins: { rx, tx } };
}

async function captureSnapshot(): Promise<Snapshot> {
  const pinList = await requestJson<{ pins: PinState[]; time: number }>("GET", "/pin/");
  assert(Array.isArray(pinList.pins) && pinList.pins.length === 8, "REST pin inventory must contain pins 0-7");
  const pins = new Map(pinList.pins.map((pin) => [pin.pin, pin]));
  for (let pin = 0; pin <= 7; pin += 1) {
    const state = pins.get(pin);
    assert(state !== undefined, `REST pin inventory omitted pin ${pin}`);
    assert(state.level === 0 || state.level === 1, `Pin ${pin} returned invalid level ${String(state.level)}`);
  }

  const pwm = new Map<number, PwmState>();
  for (let pin = 0; pin <= 7; pin += 1) {
    if (pins.get(pin)?.mode === "pwmOutput") pwm.set(pin, await requestJson<PwmState>("GET", `/pin/${pin}/pwm`));
  }

  const uartList = await requestJson<{ uarts: UartConfig[]; time: number }>("GET", "/uart/");
  assert(Array.isArray(uartList.uarts) && uartList.uarts.length === 1, "REST UART inventory must contain UART 0");
  const uart = uartList.uarts[0];
  assert(uart !== undefined && uart.id === UART_ID, "REST UART inventory omitted UART 0");
  if (uart.enable) {
    const assigned = [uart.pins.rx, uart.pins.tx].filter((pin): pin is number => pin !== null);
    assert(assigned.every((pin) => pin === OUTPUT_A || pin === OUTPUT_B), "Refusing to disturb an enabled UART using pins other than 6 and 7");
  }

  const power3v3 = await requestJson<{ enable: boolean; time: number }>("GET", "/power/3v3");
  const power5v = await requestJson<{ enable: boolean; time: number }>("GET", "/power/5v");
  return { pins, pwm, uart, power: { "3v3": power3v3.enable, "5v": power5v.enable } };
}

async function testRestMetadata(): Promise<void> {
  log("REST metadata, preflight, and negative request conditions");
  const spec = await requestJson<JsonObject>("GET", "/openapi.json");
  assert(spec.openapi === "3.1.0", `Unexpected OpenAPI version ${String(spec.openapi)}`);
  await expectStatus("GET", "/mcp", 405);
  await expectStatus("DELETE", "/mcp", 405);
  await expectStatus("OPTIONS", "/pin/", 204);
  await expectStatus("GET", "/does-not-exist", 405);
  await expectStatus("PUT", `/pin/${OUTPUT_A}`, 415);
  await expectStatus("PUT", "/pin/99", 404, { mode: "digitalInput", pullUp: false, pullDown: false });
}

async function testMcpProtocol(): Promise<void> {
  log("MCP initialization, notification, ping, and tool inventory");
  const initialized = await mcpRequest("initialize", {
    protocolVersion: "2025-06-18",
    capabilities: {},
    clientInfo: { name: "hardware-smoke", version: "1.0.0" },
  });
  assert(initialized?.result?.protocolVersion === "2025-06-18", "MCP initialize negotiated an unexpected protocol version");
  await mcpRequest("notifications/initialized", undefined, true);
  const ping = await mcpRequest("ping");
  assert(isObject(ping?.result) && Object.keys(ping.result).length === 0, "MCP ping result was not empty");
  await mcpRequest("notifications/cancelled", { requestId: 999, reason: "condition test" }, true);
  const batch = await rawRequest("POST", "/mcp", [{ jsonrpc: "2.0", id: 1, method: "ping" }], {
    Accept: "application/json", "MCP-Protocol-Version": "2025-06-18",
  });
  const batchReason = parseJson(batch, "MCP batch rejection") as JsonObject;
  assert(batch.status === 400 && (batch.text.includes("JSON-RPC batch not supported") ||
    batchReason.reason === "body must be a JSON object"), "MCP batch rejection was incorrect");
  const invalid = await rawRequest("POST", "/mcp", { jsonrpc: "1.0", id: 1, method: "ping" }, {
    Accept: "application/json", "MCP-Protocol-Version": "2025-06-18",
  });
  assert(invalid.status === 400 && invalid.text.includes("Invalid Request"), "MCP invalid request rejection was incorrect");
  const badAccept = await rawRequest("POST", "/mcp", { jsonrpc: "2.0", id: 1, method: "ping" }, { Accept: "text/plain" });
  assert(badAccept.status === 406, `MCP invalid Accept returned ${badAccept.status}, expected 406`);
  const badVersion = await rawRequest("POST", "/mcp", { jsonrpc: "2.0", id: 1, method: "ping" }, {
    Accept: "application/json", "MCP-Protocol-Version": "1900-01-01",
  });
  assert(badVersion.status === 400 && badVersion.text.includes("Unsupported"), "MCP unsupported protocol version was accepted");
  const badOrigin = await rawRequest("POST", "/mcp", { jsonrpc: "2.0", id: 1, method: "ping" }, {
    Accept: "application/json", Origin: "file://local-test",
  });
  assert(badOrigin.status === 403, `MCP invalid origin returned ${badOrigin.status}, expected 403`);
  for (const protocolVersion of ["2024-11-05", "2025-03-26", "2025-06-18"]) {
    const supported = await rawRequest("POST", "/mcp", { jsonrpc: "2.0", id: 1, method: "ping" }, {
      Accept: "application/json", "MCP-Protocol-Version": protocolVersion,
    });
    assert(supported.status === 200, `MCP supported protocol ${protocolVersion} returned ${supported.status}`);
  }
  try {
    const listed = await mcpRequest("tools/list");
    const tools = listed?.result?.tools;
    assert(Array.isArray(tools), "MCP tools/list omitted tools");
    const names = new Set(tools.map((tool) => (isObject(tool) ? tool.name : undefined)));
    for (const name of EXPECTED_MCP_TOOLS) assert(names.has(name), `MCP tools/list omitted ${name}`);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    deferredFailures.push(message);
    log(`continuing after protocol failure: ${message}`);
  }
}

async function testInventorySurfaces(state: Snapshot): Promise<void> {
  log("REST and MCP inventory");
  const mcpPins = await mcpTool<{ pins: PinState[] }>("list_pins", {});
  const mcpUarts = await mcpTool<{ uarts: UartConfig[] }>("list_uarts", {});
  assert(mcpPins.pins.length === 8, "MCP list_pins did not return eight pins");
  assert(mcpUarts.uarts.length === 1, "MCP list_uarts did not return UART 0");
  for (const rail of ["3v3", "5v"] as const) {
    const power = await mcpTool<{ enable: boolean }>("get_output_power_state", { rail });
    assert(power.enable === state.power[rail], `MCP ${rail} power state disagrees with REST`);
  }
}

async function testPower(state: Snapshot): Promise<void> {
  log("REST and MCP active power output toggles");
  for (const rail of ["3v3", "5v"] as const) {
    const original = state.power[rail];
    await requestJson<void>("POST", `/power/${rail}`, { enable: !original }, 204);
    const restChanged = await requestJson<{ enable: boolean }>("GET", `/power/${rail}`);
    assert(restChanged.enable === !original, `REST ${rail} power output did not toggle`);
    await mcpTool("set_output_power_state", { rail, enable: original });
    const mcpRestored = await mcpTool<{ enable: boolean }>("get_output_power_state", { rail });
    assert(mcpRestored.enable === original, `MCP ${rail} power output did not restore`);
    await mcpTool("set_output_power_state", { rail, enable: !original });
    const mcpChanged = await requestJson<{ enable: boolean }>("GET", `/power/${rail}`);
    assert(mcpChanged.enable === !original, `MCP ${rail} power output did not toggle`);
    await requestJson<void>("POST", `/power/${rail}`, { enable: original }, 204);
  }
}

async function configureRestPin(pin: number, mode: PinMode, pullDown = false): Promise<void> {
  await requestJson<void>("PUT", `/pin/${pin}`, { mode, openDrain: false, pullUp: false, pullDown }, 204);
}

async function waitForRestLevel(pin: number, expected: 0 | 1): Promise<number> {
  const deadline = Date.now() + 200;
  let actual = -1;
  do {
    actual = (await requestJson<{ level: number }>("GET", `/pin/${pin}/level`)).level;
    if (actual === expected) return actual;
    await Bun.sleep(10);
  } while (Date.now() < deadline);
  return actual;
}

async function waitForMcpLevel(pin: number, expected: 0 | 1): Promise<number> {
  const deadline = Date.now() + 200;
  let actual = -1;
  do {
    actual = (await mcpTool<{ levels: number[] }>("get_pin_levels", { pins: [pin] })).levels[0] ?? -1;
    if (actual === expected) return actual;
    await Bun.sleep(10);
  } while (Date.now() < deadline);
  return actual;
}

async function testRestPins(): Promise<void> {
  log("REST pin configuration, levels, batch calls, pulse, PWM, and trace");
  await configureRestPin(OUTPUT_A, "digitalOutput");
  await requestJson<void>("POST", `/pin/${OUTPUT_A}/level`, { level: 0 }, 204);
  await configureRestPin(OUTPUT_B, "digitalInput", true);
  for (const level of [0, 1] as const) {
    await requestJson<void>("POST", `/pin/${OUTPUT_A}/level`, { level }, 204);
    const sampled = await waitForRestLevel(OUTPUT_B, level);
    check(sampled === level, `REST loopback ${OUTPUT_A}->${OUTPUT_B} expected ${level}, got ${sampled}`);
  }

  await requestJson<void>("PUT", "/pin/config", {
    pins: [OUTPUT_A], mode: "digitalOutput", openDrain: false, pullUp: false, pullDown: false,
  }, 204);
  await requestJson<void>("POST", "/pin/level", { pins: [OUTPUT_A], level: 0 }, 204);
  const levels = await requestJson<{ levels: number[] }>("GET", "/pin/level", { pins: [OUTPUT_A, OUTPUT_B] });
  assert(levels.levels.length === 2 && levels.levels[0] === 0 && levels.levels[1] === 0, "REST batch level round-trip failed");
  await requestJson<void>("POST", `/pin/${OUTPUT_A}/pulse`, { width: 2000, level: 1 }, 204);
  const pulseLevel = await requestJson<{ level: number }>("GET", `/pin/${OUTPUT_B}/level`);
  assert(pulseLevel.level === 0, "REST pulse did not return to its inverse level");
  await requestJson<void>("POST", "/pin/pulse", { pins: [OUTPUT_A], width: 1000, level: 1 }, 204);

  await configureRestPin(OUTPUT_A, "pwmOutput");
  await requestJson<void>("POST", `/pin/${OUTPUT_A}/pwm`, { frequency: 1000, duty: 25 }, 204);
  const pwm = await requestJson<PwmState>("GET", `/pin/${OUTPUT_A}/pwm`);
  assert(Math.abs(pwm.frequency - 1000) < 2 && Math.abs(pwm.duty - 25) < 1, "REST PWM state did not round-trip");

  const trace = await requestJson<{ events: Array<{ edge: string; level: number }> }>(
    "GET", `/pin/${OUTPUT_B}/trace?edge=both&duration=50000`,
  );
  assert(trace.events.some((event) => event.edge === "raising" && event.level === 1), "REST trace missed PWM rising edges");
  assert(trace.events.some((event) => event.edge === "falling" && event.level === 0), "REST trace missed PWM falling edges");
  const raisingTrace = await requestJson<{ events: Array<{ edge: string; level: number }> }>(
    "GET", `/pin/${OUTPUT_B}/trace?edge=raising&duration=30000`,
  );
  assert(raisingTrace.events.length > 0 && raisingTrace.events.every((event) => event.edge === "raising" && event.level === 1),
    "REST raising-only trace returned an incorrect edge");
  const batchTrace = await requestJson<{ events: Array<{ pin: number; edge: string }> }>(
    "GET", "/pin/trace", { pins: [OUTPUT_B], edge: "falling", duration: 30000 },
  );
  assert(batchTrace.events.length > 0 && batchTrace.events.every((event) => event.pin === OUTPUT_B && event.edge === "falling"),
    "REST batch falling-only trace returned an incorrect edge");

  for (const condition of [{ frequency: 500, duty: 10 }, { frequency: 2000, duty: 50 }, { frequency: 10000, duty: 90 },
    { frequency: 50000, duty: 25 }]) {
    await requestJson<void>("POST", `/pin/${OUTPUT_A}/pwm`, condition, 204);
    const state = await requestJson<PwmState>("GET", `/pin/${OUTPUT_A}/pwm`);
    assert(Math.abs(state.frequency - condition.frequency) < Math.max(2, condition.frequency * 0.02),
      `REST PWM frequency ${condition.frequency} did not round-trip`);
    assert(Math.abs(state.duty - condition.duty) < 1, `REST PWM duty ${condition.duty} did not round-trip`);
  }
  await requestJson<void>("POST", `/pin/${OUTPUT_A}/pwm`, { frequency: 1000, duty: 0 }, 204);
  assert((await waitForRestLevel(OUTPUT_B, 0)) === 0, "REST 0% PWM did not hold the connected input low");
  await requestJson<void>("POST", `/pin/${OUTPUT_A}/pwm`, { frequency: 1000, duty: 100 }, 204);
  assert((await waitForRestLevel(OUTPUT_B, 1)) === 1, "REST 100% PWM did not hold the connected input high");
  await configureRestPin(OUTPUT_A, "disable");

  for (const outputMode of ["digitalOutput", "digitalInputOutput"] as const) {
    await configureRestPin(OUTPUT_A, outputMode);
    await configureRestPin(OUTPUT_B, "digitalInput");
    await requestJson<void>("POST", `/pin/${OUTPUT_A}/level`, { level: 1 }, 204);
    assert((await waitForRestLevel(OUTPUT_B, 1)) === 1, `REST ${outputMode} failed to drive high`);
  }
  await configureRestPin(OUTPUT_A, "disable");
  await configureRestPin(OUTPUT_B, "disable");
}

async function testMcpPins(): Promise<void> {
  log("MCP pin configuration, levels, pulse, PWM, and trace");
  await mcpTool("configure_pins", {
    pins: [OUTPUT_B], mode: "digitalOutput", openDrain: false, pullUp: false, pullDown: false,
  });
  await mcpTool("set_pin_levels", { pins: [OUTPUT_B], level: 0 });
  await mcpTool("configure_pins", {
    pins: [OUTPUT_A], mode: "digitalInput", openDrain: false, pullUp: false, pullDown: true,
  });
  for (const level of [0, 1] as const) {
    await mcpTool("set_pin_levels", { pins: [OUTPUT_B], level });
    const sampled = await waitForMcpLevel(OUTPUT_A, level);
    check(sampled === level, `MCP loopback ${OUTPUT_B}->${OUTPUT_A} expected ${level}, got ${sampled}`);
  }
  await mcpTool("set_pin_levels", { pins: [OUTPUT_B], level: 0 });
  await mcpTool("pulse_pins", { pins: [OUTPUT_B], width: 2000, level: 1 });
  const afterPulse = await mcpTool<{ levels: number[] }>("get_pin_levels", { pins: [OUTPUT_A] });
  assert(afterPulse.levels[0] === 0, "MCP pulse did not return to its inverse level");

  await mcpTool("configure_pins", {
    pins: [OUTPUT_B], mode: "pwmOutput", openDrain: false, pullUp: false, pullDown: false,
  });
  await mcpTool("set_pin_pwms", { pins: [OUTPUT_B], frequency: 1000, duty: 25 });
  const pwm = await mcpTool<{ pwms: Array<{ frequency: number; duty: number }> }>("get_pin_pwms", { pins: [OUTPUT_B] });
  assert(pwm.pwms.length === 1 && Math.abs(pwm.pwms[0]!.frequency - 1000) < 2, "MCP PWM state did not round-trip");
  const trace = await mcpTool<{ events: Array<{ pin: number; edge: string; level: number }> }>("trace_pins", {
    pins: [OUTPUT_A], edge: "both", duration: 50000,
  });
  assert(trace.events.some((event) => event.pin === OUTPUT_A && event.edge === "raising" && event.level === 1),
    "MCP trace missed PWM rising edges");
  assert(trace.events.some((event) => event.pin === OUTPUT_A && event.edge === "falling" && event.level === 0),
    "MCP trace missed PWM falling edges");
  for (const edge of ["raising", "falling"] as const) {
    const filtered = await mcpTool<{ events: Array<{ pin: number; edge: string }> }>("trace_pins", {
      pins: [OUTPUT_A], edge, duration: 30000,
    });
    assert(filtered.events.length > 0 && filtered.events.every((event) => event.pin === OUTPUT_A && event.edge === edge),
      `MCP ${edge}-only trace returned an incorrect edge`);
  }
  for (const condition of [{ frequency: 500, duty: 10 }, { frequency: 2000, duty: 50 }, { frequency: 10000, duty: 90 },
    { frequency: 50000, duty: 25 }]) {
    await mcpTool("set_pin_pwms", { pins: [OUTPUT_B], ...condition });
    const state = await mcpTool<{ pwms: Array<{ frequency: number; duty: number }> }>("get_pin_pwms", { pins: [OUTPUT_B] });
    assert(Math.abs(state.pwms[0]!.frequency - condition.frequency) < Math.max(2, condition.frequency * 0.02),
      `MCP PWM frequency ${condition.frequency} did not round-trip`);
    assert(Math.abs(state.pwms[0]!.duty - condition.duty) < 1, `MCP PWM duty ${condition.duty} did not round-trip`);
  }
  await mcpTool("configure_pins", {
    pins: [OUTPUT_B], mode: "disable", openDrain: false, pullUp: false, pullDown: false,
  });
}

async function collectRestUart(marker: number[]): Promise<number[]> {
  const received: number[] = [];
  const deadline = Date.now() + UART_TIMEOUT_MS;
  while (Date.now() < deadline && received.length < marker.length) {
    const chunk = await requestJson<{ data: number[] }>("GET", `/uart/${UART_ID}/receive`);
    assert(Array.isArray(chunk.data), "REST UART receive did not return byte data");
    received.push(...chunk.data);
    if (received.length < marker.length) await Bun.sleep(20);
  }
  return received;
}

async function collectMcpUart(marker: number[]): Promise<number[]> {
  const received: number[] = [];
  const deadline = Date.now() + UART_TIMEOUT_MS;
  while (Date.now() < deadline && received.length < marker.length) {
    const chunk = await mcpTool<{ data: number[] }>("uart_receive", { id: UART_ID });
    assert(Array.isArray(chunk.data), "MCP UART receive did not return byte data");
    received.push(...chunk.data);
    if (received.length < marker.length) await Bun.sleep(20);
  }
  return received;
}

async function testPwmResourcesAndTraceRaces(): Promise<void> {
  log("independent PWM resources, frequency churn, trace saturation, and overlapping traces");
  const resourcePins = [0, 1, 2, 3, 4, 5, 6, 7];
  try {
    // Start from a known allocation state; earlier sections may leave PWM timers/channels active.
    for (const pin of resourcePins) await configureRestPin(pin, "disable");
    const pwmPins = [0, 1, 2, 3];
    for (const pin of pwmPins) {
      await configureRestPin(pin, "pwmOutput");
      await requestJson<void>("POST", `/pin/${pin}/pwm`, { frequency: 2000, duty: 20 + pin * 10 }, 204);
    }
    const independent = await Promise.all(pwmPins.map((pin) => requestJson<PwmState>("GET", `/pin/${pin}/pwm`)));
    assert(independent.every((state) => Math.abs(state.frequency - 2000) < 4),
      "Independent PWM allocations returned inconsistent frequencies");
    const outputExhausted = await rawRequest("PUT", "/pin/4", {
      mode: "pwmOutput", openDrain: false, pullUp: false, pullDown: false,
    });
    assert(outputExhausted.status === 422 && outputExhausted.text.includes("PWM output limit reached"),
      `Fifth PWM output returned ${outputExhausted.status}, expected resource exhaustion`);

    const independentConditions = [[0, 500], [1, 2000], [2, 10000], [3, 50000]] as const;
    for (const [pin, frequency] of independentConditions) {
      await requestJson<void>("POST", `/pin/${pin}/pwm`, { frequency, duty: 40 }, 204);
    }
    const distinct = await Promise.all(pwmPins.map((pin) => requestJson<PwmState>("GET", `/pin/${pin}/pwm`)));
    assert(distinct.every((state, index) => Math.abs(state.frequency - independentConditions[index]![1]) < 4),
      "Independent PWM frequency updates affected another output");
    for (let iteration = 0; iteration < 8; iteration += 1) {
      const frequency = iteration % 2 === 0 ? 1000 : 20000;
      await mcpTool("set_pin_pwms", { pins: [0], frequency, duty: 10 + iteration * 10 });
    }
  } finally {
    for (const pin of resourcePins) await configureRestPin(pin, "disable");
  }

  await configureRestPin(OUTPUT_A, "pwmOutput");
  await requestJson<void>("POST", `/pin/${OUTPUT_A}/pwm`, { frequency: 50000, duty: 50 }, 204);
  await configureRestPin(OUTPUT_B, "digitalInput");
  const saturated = await requestJson<{ events: Array<{ time: number }> }>(
    "GET", `/pin/${OUTPUT_B}/trace?edge=both&duration=50000`,
  );
  assert(saturated.events.length > 0 && saturated.events.length <= 1024, "High-frequency trace returned an invalid event count");
  await configureRestPin(OUTPUT_A, "disable");
  await configureRestPin(OUTPUT_B, "disable");
}

async function testOverlappingTraceRace(): Promise<void> {
  log("concurrent trace isolation");
  await configureRestPin(OUTPUT_A, "pwmOutput");
  await requestJson<void>("POST", `/pin/${OUTPUT_A}/pwm`, { frequency: 1000, duty: 50 }, 204);
  await configureRestPin(OUTPUT_B, "digitalInput");
  const disjoint = await Promise.all([
    rawRequest("GET", `/pin/${OUTPUT_A}/trace?edge=both&duration=100000`),
    rawRequest("GET", `/pin/${OUTPUT_B}/trace?edge=both&duration=100000`),
  ]);
  assert(disjoint.every((result) => result.status === 200),
    `Disjoint traces returned ${disjoint.map((result) => result.status).join(", ")}, expected 200/200`);
  const overlapSettled = await Promise.allSettled([
    rawRequest("GET", `/pin/${OUTPUT_B}/trace?edge=raising&duration=100000`),
    rawRequest("GET", `/pin/${OUTPUT_B}/trace?edge=falling&duration=100000`),
  ]);
  const overlapping = overlapSettled.flatMap((result) => result.status === "fulfilled" ? [result.value] : []);
  const transportFailures = overlapSettled.flatMap((result) => result.status === "rejected" ? [String(result.reason)] : []);
  assert(transportFailures.length === 0, `Overlapping trace reset or starved a connection: ${transportFailures.join("; ")}`);
  const statuses = overlapping.map((result) => result.status).sort((a, b) => a - b);
  assert(statuses.length === 2 && statuses[0] === 200 && statuses[1] === 409,
    `Overlapping traces returned ${statuses.join(", ")}, expected one 200 and one 409`);
  const conflict = overlapping.find((result) => result.status === 409);
  assert(conflict?.text.includes("active trace request"), `Trace conflict returned an unexpected reason: ${conflict?.text}`);
  await requestJson("GET", `/pin/${OUTPUT_B}/trace?edge=both&duration=20000`);
}

async function testRestUart(): Promise<void> {
  log("REST UART loopback across baud and framing conditions");
  const conditions = [
    { baudRate: 9600, dataBits: 8 as const, parity: "none" as const, stopBits: 1 as const },
    { baudRate: 57600, dataBits: 7 as const, parity: "even" as const, stopBits: 1 as const },
    { baudRate: 115200, dataBits: 8 as const, parity: "odd" as const, stopBits: 2 as const },
  ];
  for (const [index, condition] of conditions.entries()) {
    await requestJson<void>("POST", `/uart/${UART_ID}/config`,
      loopbackUart(OUTPUT_B, OUTPUT_A, condition.baudRate, condition.dataBits, condition.parity, condition.stopBits), 204);
    await requestJson<void>("POST", `/uart/${UART_ID}/flush`, undefined, 204);
    const marker = [0x31 + index, 0x41, 0x55, 0x7e];
    await requestJson<void>("POST", `/uart/${UART_ID}/transmit`, { data: marker }, 204);
    const received = await collectRestUart(marker);
    assert(JSON.stringify(received) === JSON.stringify(marker),
      `REST UART ${condition.baudRate}/${condition.dataBits}/${condition.parity}/${condition.stopBits} mismatch: ${JSON.stringify(received)}`);
  }
  await requestJson<void>("POST", `/uart/${UART_ID}/config`, uartBody(disabledUart), 204);
}

async function testMcpUart(): Promise<void> {
  log("MCP UART loopback across baud and framing conditions in reverse direction");
  const conditions = [
    { baudRate: 19200, dataBits: 8 as const, parity: "none" as const, stopBits: 1 as const },
    { baudRate: 38400, dataBits: 7 as const, parity: "odd" as const, stopBits: 2 as const },
    { baudRate: 115200, dataBits: 8 as const, parity: "even" as const, stopBits: 1 as const },
  ];
  for (const [index, condition] of conditions.entries()) {
    await mcpTool("configure_uart", {
      id: UART_ID,
      ...loopbackUart(OUTPUT_A, OUTPUT_B, condition.baudRate, condition.dataBits, condition.parity, condition.stopBits),
    });
    await mcpTool("uart_flush", { id: UART_ID });
    const marker = [0x61 + index, 0x42, 0x56, 0x7d];
    await mcpTool("uart_transmit", { id: UART_ID, data: marker });
    const received = await collectMcpUart(marker);
    assert(JSON.stringify(received) === JSON.stringify(marker),
      `MCP UART ${condition.baudRate}/${condition.dataBits}/${condition.parity}/${condition.stopBits} mismatch: ${JSON.stringify(received)}`);
  }
  await mcpTool("configure_uart", { id: UART_ID, ...uartBody(disabledUart) });
}

async function testUartPayloadsAndRaces(): Promise<void> {
  log("UART UTF-8, empty/maximum payloads, flush, mixed interfaces, and concurrent transmit");
  await requestJson<void>("POST", `/uart/${UART_ID}/config`, loopbackUart(OUTPUT_B, OUTPUT_A, 115200, 8, "none", 1, "utf8"), 204);
  await requestJson<void>("POST", `/uart/${UART_ID}/flush`, undefined, 204);
  const text = "local-loopback-你好-π";
  await requestJson<void>("POST", `/uart/${UART_ID}/transmit`, { data: text }, 204);
  let receivedText = "";
  const textDeadline = Date.now() + UART_TIMEOUT_MS;
  while (Date.now() < textDeadline && Buffer.byteLength(receivedText) < Buffer.byteLength(text)) {
    const chunk = await mcpTool<{ data: string }>("uart_receive", { id: UART_ID });
    receivedText += chunk.data;
    if (Buffer.byteLength(receivedText) < Buffer.byteLength(text)) await Bun.sleep(20);
  }
  assert(receivedText === text, `Mixed REST/MCP UTF-8 UART mismatch: ${JSON.stringify(receivedText)}`);
  await requestJson<void>("POST", `/uart/${UART_ID}/transmit`, { data: "discard-me" }, 204);
  await Bun.sleep(20);
  await mcpTool("uart_flush", { id: UART_ID });
  assert((await requestJson<{ data: string }>("GET", `/uart/${UART_ID}/receive`)).data === "", "UART flush left pending UTF-8 data");
  await requestJson<void>("POST", `/uart/${UART_ID}/transmit`, { data: "" }, 204);
  await requestJson<void>("POST", `/uart/${UART_ID}/config`, uartBody(disabledUart), 204);

  await requestJson<void>("POST", `/uart/${UART_ID}/config`, loopbackUart(OUTPUT_B, OUTPUT_A, 115200, 8, "none", 1, "utf8"), 204);
  await mcpTool("uart_flush", { id: UART_ID });
  const maximumText = "x".repeat(2048);
  await mcpTool("uart_transmit", { id: UART_ID, data: maximumText });
  let maximumReceived = "";
  const maximumDeadline = Date.now() + UART_TIMEOUT_MS;
  while (Date.now() < maximumDeadline && maximumReceived.length < maximumText.length) {
    maximumReceived += (await requestJson<{ data: string }>("GET", `/uart/${UART_ID}/receive`)).data;
    if (maximumReceived.length < maximumText.length) await Bun.sleep(20);
  }
  assert(maximumReceived === maximumText, `Maximum compact UART payload mismatch: ${maximumReceived.length} bytes`);
  const oversizedText = await rawRequest("POST", `/uart/${UART_ID}/transmit`, { data: `${maximumText}x` });
  assert(oversizedText.status === 400, `Oversized UART payload returned ${oversizedText.status}, expected 400`);
  await requestJson<void>("POST", `/uart/${UART_ID}/config`, uartBody(disabledUart), 204);

  await mcpTool("configure_uart", { id: UART_ID, ...loopbackUart(OUTPUT_A, OUTPUT_B) });
  await mcpTool("uart_flush", { id: UART_ID });
  const maximum = Array.from({ length: 2048 }, (_, index) => (index * 31) & 0xff);
  const byteEnvelope = await rawRequest("POST", "/mcp", {
    jsonrpc: "2.0", id: nextMcpId++, method: "tools/call", params: { name: "uart_transmit", arguments: { id: UART_ID, data: maximum } },
  }, { Accept: "application/json", "MCP-Protocol-Version": "2025-06-18" });
  assert(byteEnvelope.status === 200,
    `MCP maximum byte-array UART payload returned ${byteEnvelope.status}: ${byteEnvelope.text.slice(0, 160)}`);
  const byteResponse = parseJson(byteEnvelope, "MCP maximum byte-array UART payload");
  assert(isObject(byteResponse) && !isObject(byteResponse.error), "MCP maximum byte-array UART payload returned an error envelope");
  const receivedMaximum = await collectRestUart(maximum);
  assert(JSON.stringify(receivedMaximum) === JSON.stringify(maximum), `Maximum byte UART payload mismatch: ${receivedMaximum.length} bytes`);
  const oversized = await rawRequest("POST", `/uart/${UART_ID}/transmit`, { data: [...maximum, 1] });
  assert(oversized.status === 400, `Oversized byte UART payload returned ${oversized.status}, expected 400`);
  await mcpTool("uart_flush", { id: UART_ID });
  const chunks = [[1, 2, 3, 4], [11, 12, 13, 14], [21, 22, 23, 24], [31, 32, 33, 34]];
  await Promise.all(chunks.map((data, index) => index % 2 === 0
    ? requestJson<void>("POST", `/uart/${UART_ID}/transmit`, { data }, 204)
    : mcpTool("uart_transmit", { id: UART_ID, data })));
  const expectedLength = chunks.reduce((total, chunk) => total + chunk.length, 0);
  const received: number[] = [];
  const deadline = Date.now() + UART_TIMEOUT_MS;
  while (Date.now() < deadline && received.length < expectedLength) {
    received.push(...(await requestJson<{ data: number[] }>("GET", `/uart/${UART_ID}/receive`)).data);
    if (received.length < expectedLength) await Bun.sleep(20);
  }
  assert(received.length === expectedLength, `Concurrent UART transmit lost bytes: expected ${expectedLength}, got ${received.length}`);
  for (const chunk of chunks) {
    assert(chunk.every((byte) => received.includes(byte)), `Concurrent UART transmit lost chunk ${JSON.stringify(chunk)}`);
  }
  await mcpTool("configure_uart", { id: UART_ID, ...uartBody(disabledUart) });
}

function hardwareScript(output: number, input: number, marker: string): string {
  return [
    `configure_pins{pins={${output}}, mode="digitalOutput", openDrain=false, pullUp=false, pullDown=false}`,
    `configure_pins{pins={${input}}, mode="digitalInput", openDrain=false, pullUp=false, pullDown=true}`,
    `set_pin_levels{pins={${output}}, level=0}`,
    `local low = get_pin_levels{pins={${input}}}.levels[1]`,
    `set_pin_levels{pins={${output}}, level=1}`,
    `local high = get_pin_levels{pins={${input}}}.levels[1]`,
    `set_pin_levels{pins={${output}}, level=0}`,
    `print("${marker}", low, high)`,
    `return {low=low, high=high, ok=(low == 0 and high == 1)}`,
  ].join("\n");
}

function assertScriptResult(result: unknown, marker: string, label: string): void {
  assert(isObject(result), `${label} returned an invalid script envelope`);
  assert(isObject(result.result) && result.result.ok === true, `${label} hardware calls failed: ${JSON.stringify(result.result)}`);
  assert(typeof result.output === "string" && result.output.includes(marker), `${label} omitted its output marker`);
  assert(typeof result.calls === "number" && result.calls === 7, `${label} reported an unexpected call count`);
  assert(typeof result.elapsed === "number" && result.elapsed >= 0, `${label} reported an invalid elapsed time`);
}

async function testScripts(): Promise<void> {
  log("REST and MCP scripts under success, sleep, and caught-error conditions");
  const rest = await requestJson<unknown>("POST", "/script", {
    script: hardwareScript(OUTPUT_A, OUTPUT_B, "rest-script-ok"), maxCalls: 12, timeout: 1000000,
  });
  assertScriptResult(rest, "rest-script-ok", "REST script");
  const mcp = await mcpTool<unknown>("run_script", {
    script: hardwareScript(OUTPUT_B, OUTPUT_A, "mcp-script-ok"), maxCalls: 12, timeout: 1000000,
  });
  assertScriptResult(mcp, "mcp-script-ok", "MCP run_script");

  const utility = await requestJson<JsonObject>("POST", "/script", {
    script: [
      "local ok, err = pcall(function() set_pin_levels{pins={6}, level=1} end)",
      "sleep(1000)",
      "print('caught-error', ok)",
      "return {caught=(not ok), hasReason=(type(err.reason) == 'string')}",
    ].join("\n"),
    maxCalls: 4,
    timeout: 200000,
  });
  assert(isObject(utility.result) && utility.result.caught === true && utility.result.hasReason === true,
    "REST script did not catch and expose a tool error");
  assert(typeof utility.output === "string" && utility.output.includes("caught-error"), "REST utility script output was not captured");

  const invalid = await rawRequest("POST", "/script", { script: "this is not valid (", maxCalls: 2, timeout: 100000 });
  assert(invalid.status === 400, `REST invalid script returned ${invalid.status}, expected 400`);
  const maxCalls = await rawRequest("POST", "/script", {
    script: "list_pins{}\nlist_pins{}\nreturn true", maxCalls: 1, timeout: 200000,
  });
  assert(maxCalls.status === 422 && maxCalls.text.includes("maxCalls"), "REST script maxCalls limit was not enforced");
  const timeout = await rawRequest("POST", "/script", { script: "sleep(200000)\nreturn true", maxCalls: 1, timeout: 50000 });
  assert(timeout.status === 422 && timeout.text.includes("timeout"), "REST script timeout was not enforced");
  const pulseBudget = await rawRequest("POST", "/script", {
    script: "configure_pins{pins={6}, mode='digitalOutput', pullUp=false, pullDown=false}\npulse_pins{pins={6}, width=200000, level=1}",
    maxCalls: 4,
    timeout: 100000,
  });
  assert(pulseBudget.status === 422 && pulseBudget.text.includes("remaining script timeout"),
    "REST script pulse budget was not enforced");
  const uncaught = await rawRequest("POST", "/script", { script: "error('condition failure')", maxCalls: 1, timeout: 100000 });
  assert(uncaught.status === 422 && uncaught.text.includes("condition failure"), "REST uncaught script error was not reported");
}

async function testConcurrency(): Promise<void> {
  log("concurrent REST/MCP reads, idempotent writes, lock acquisition, and script exclusion");
  await configureRestPin(OUTPUT_A, "digitalOutput");
  await requestJson<void>("POST", `/pin/${OUTPUT_A}/level`, { level: 1 }, 204);
  await configureRestPin(OUTPUT_B, "digitalInput");

  const reads = await Promise.all([
    ...Array.from({ length: 8 }, () => requestJson<{ level: number }>("GET", `/pin/${OUTPUT_B}/level`).then((r) => r.level)),
    ...Array.from({ length: 8 }, () => mcpTool<{ levels: number[] }>("get_pin_levels", { pins: [OUTPUT_B] }).then((r) => r.levels[0])),
  ]);
  assert(reads.every((level) => level === 1), `Concurrent REST/MCP reads were inconsistent: ${JSON.stringify(reads)}`);

  await Promise.all([
    ...Array.from({ length: 4 }, () => requestJson<void>("POST", `/pin/${OUTPUT_A}/level`, { level: 0 }, 204)),
    ...Array.from({ length: 4 }, () => mcpTool("set_pin_levels", { pins: [OUTPUT_A], level: 0 })),
  ]);
  assert((await waitForRestLevel(OUTPUT_B, 0)) === 0, "Concurrent idempotent writes did not settle low");

  const lockBody = { resources: [{ type: "pin", pins: [OUTPUT_A, OUTPUT_B], method: ["read", "write"] }] };
  const lockRace = await Promise.all([rawRequest("POST", "/lock", lockBody), rawRequest("POST", "/lock", lockBody)]);
  const winners = lockRace.filter((result) => result.status === 201);
  const losers = lockRace.filter((result) => result.status === 409);
  if (winners.length === 1) {
    const lock = parseJson(winners[0]!, "concurrent lock winner");
    assert(isObject(lock) && typeof lock.id === "string", "Concurrent lock winner omitted id");
    createdRestLocks.add(lock.id);
    const mcpRead = await mcpTool<{ levels: number[] }>("get_pin_levels", { pins: [OUTPUT_B], lockId: lock.id });
    assert(mcpRead.levels[0] === 0, "MCP could not use the REST lock winner");
    await requestJson<void>("DELETE", `/lock/${encodeURIComponent(lock.id)}`, undefined, 204);
    createdRestLocks.delete(lock.id);
  }
  assert(winners.length === 1 && losers.length === 1,
    `Concurrent lock acquisition expected one 201 and one 409, got ${lockRace.map((result) => result.status).join(", ")}`);

  const scriptBody = { script: "sleep(250000)\nreturn {ok=true}", maxCalls: 1, timeout: 500000 };
  const scriptRace = await Promise.all([rawRequest("POST", "/script", scriptBody), rawRequest("POST", "/script", scriptBody)]);
  const scriptSuccesses = scriptRace.filter((result) => result.status === 200);
  const scriptBusy = scriptRace.filter((result) => result.status === 409);
  assert(scriptSuccesses.length === 1 && scriptBusy.length === 1,
    `Concurrent scripts expected one 200 and one 409, got ${scriptRace.map((result) => result.status).join(", ")}`);

  await configureRestPin(OUTPUT_A, "disable");
  await configureRestPin(OUTPUT_B, "disable");
}

async function testLockMethodsAndPeripheralRaces(): Promise<void> {
  log("read/write lock isolation, mixed resources, and UART-versus-lock acquisition");
  await configureRestPin(OUTPUT_A, "digitalOutput");
  await requestJson<void>("POST", `/pin/${OUTPUT_A}/level`, { level: 0 }, 204);
  const readLock = await requestJson<{ id: string }>("POST", "/lock", {
    resources: [{ type: "pin", pins: [OUTPUT_A], method: ["read"] }],
  }, 201);
  createdRestLocks.add(readLock.id);
  await requestJson<void>("POST", `/pin/${OUTPUT_A}/level`, { level: 1 }, 204);
  const deniedRead = await rawRequest("GET", `/pin/${OUTPUT_A}/level`);
  assert(deniedRead.status === 412, `Read lock did not block unlocked reads: ${deniedRead.status}`);
  await requestJson<{ level: number }>("GET", `/pin/${OUTPUT_A}/level`, undefined, 200, { "X-Lock-Id": readLock.id });
  await requestJson<void>("DELETE", `/lock/${readLock.id}`, undefined, 204);
  createdRestLocks.delete(readLock.id);

  const mixed = await requestJson<{ id: string }>("POST", "/lock", {
    resources: [
      { type: "pin", pins: [OUTPUT_A], method: ["read", "write"] },
      { type: "power", rails: ["3v3", "5v"], method: ["read"] },
      { type: "uart", ids: [UART_ID], method: ["read"] },
    ],
  }, 201);
  createdRestLocks.add(mixed.id);
  await requestJson<{ enable: boolean }>("GET", "/power/3v3", undefined, 200, { "X-Lock-Id": mixed.id });
  await requestJson<void>("DELETE", `/lock/${mixed.id}`, undefined, 204);
  createdRestLocks.delete(mixed.id);

  await configureRestPin(OUTPUT_A, "disable");
  const lockRequest = rawRequest("POST", "/lock", {
    resources: [{ type: "pin", pins: [OUTPUT_A, OUTPUT_B], method: ["read", "write"] }],
  });
  const uartRequest = rawRequest("POST", `/uart/${UART_ID}/config`, loopbackUart(OUTPUT_B, OUTPUT_A));
  const [lockResult, uartResult] = await Promise.all([lockRequest, uartRequest]);
  assert((lockResult.status === 201 && uartResult.status === 409) || (lockResult.status === 409 && uartResult.status === 204),
    `UART/lock race returned unexpected statuses ${lockResult.status}/${uartResult.status}`);
  if (lockResult.status === 201) {
    const lock = parseJson(lockResult, "UART race lock");
    assert(isObject(lock) && typeof lock.id === "string", "UART race lock omitted id");
    createdRestLocks.add(lock.id);
    await requestJson<void>("DELETE", `/lock/${lock.id}`, undefined, 204);
    createdRestLocks.delete(lock.id);
  }
  if (uartResult.status === 204) await requestJson<void>("POST", `/uart/${UART_ID}/config`, uartBody(disabledUart), 204);
}

async function testLocks(): Promise<void> {
  log("REST and MCP lock lifecycle");
  const lock = await requestJson<{ id: string }>("POST", "/lock", {
    resources: [{ type: "pin", pins: [OUTPUT_A, OUTPUT_B], method: ["read", "write"] }],
  }, 201);
  createdRestLocks.add(lock.id);
  const denied = await rawRequest("GET", `/pin/${OUTPUT_A}/level`);
  assert(denied.status === 412 || denied.status === 423, `Unlocked access during REST lock returned ${denied.status}`);
  const wrongLock = await rawRequest("GET", `/pin/${OUTPUT_A}/level`, undefined, { "X-Lock-Id": "unknown-smoke-lock" });
  assert(wrongLock.status === 412 || wrongLock.status === 423, `Unknown lock access returned ${wrongLock.status}`);
  const conflict = await rawRequest("POST", "/lock", {
    resources: [{ type: "pin", pins: [OUTPUT_A], method: ["write"] }],
  });
  assert(conflict.status === 409, `Conflicting REST lock returned ${conflict.status}, expected 409`);
  await requestJson<{ level: number }>("GET", `/pin/${OUTPUT_A}/level`, undefined, 200, { "X-Lock-Id": lock.id });
  const renewed = await requestJson<{ id: string }>("PUT", `/lock/${encodeURIComponent(lock.id)}`);
  assert(renewed.id === lock.id, "REST lock renewal returned a different id");
  await requestJson<void>("DELETE", `/lock/${encodeURIComponent(lock.id)}`, undefined, 204);
  await requestJson<void>("DELETE", `/lock/${encodeURIComponent(lock.id)}`, undefined, 204);
  createdRestLocks.delete(lock.id);

  const mcpLock = await mcpTool<{ id: string }>("create_lock", {
    resources: [{ type: "pin", pins: [OUTPUT_A, OUTPUT_B], method: ["read", "write"] }],
  });
  createdMcpLocks.add(mcpLock.id);
  const mcpRead = await mcpTool<{ levels: number[] }>("get_pin_levels", { pins: [OUTPUT_A], lockId: mcpLock.id });
  assert(mcpRead.levels.length === 1, "MCP locked pin read returned no level");
  const mcpRenewed = await mcpTool<{ id: string }>("renew_lock", { id: mcpLock.id });
  assert(mcpRenewed.id === mcpLock.id, "MCP lock renewal returned a different id");
  await mcpTool("delete_lock", { id: mcpLock.id });
  await mcpTool("delete_lock", { id: mcpLock.id });
  createdMcpLocks.delete(mcpLock.id);
}

async function restorePin(pin: number, state: PinState, pwm: PwmState | undefined): Promise<void> {
  await requestJson<void>("PUT", `/pin/${pin}`, pinConfig(state), 204);
  if (state.mode === "pwmOutput" && pwm !== undefined) {
    await requestJson<void>("POST", `/pin/${pin}/pwm`, { frequency: pwm.frequency, duty: pwm.duty }, 204);
  } else if (state.mode === "digitalOutput" || state.mode === "digitalInputOutput") {
    await requestJson<void>("POST", `/pin/${pin}/level`, { level: state.level }, 204);
  }
}

async function testSoak(): Promise<void> {
  log(`local pin/MCP/REST soak for ${SOAK_ITERATIONS} iterations`);
  for (let iteration = 0; iteration < SOAK_ITERATIONS; iteration += 1) {
    const output = iteration % 2 === 0 ? OUTPUT_A : OUTPUT_B;
    const input = output === OUTPUT_A ? OUTPUT_B : OUTPUT_A;
    await configureRestPin(output, "digitalOutput");
    await configureRestPin(input, "digitalInput", true);
    for (const level of [0, 1, 0] as const) {
      if (iteration % 2 === 0) await requestJson<void>("POST", `/pin/${output}/level`, { level }, 204);
      else await mcpTool("set_pin_levels", { pins: [output], level });
      assert((await waitForRestLevel(input, level)) === level, `Soak iteration ${iteration} missed level ${level}`);
    }
    await configureRestPin(output, "pwmOutput");
    const frequency = [500, 2000, 10000][iteration % 3]!;
    await requestJson<void>("POST", `/pin/${output}/pwm`, { frequency, duty: 50 }, 204);
    const trace = await mcpTool<{ events: unknown[] }>("trace_pins", { pins: [input], edge: "both", duration: 20000 });
    assert(trace.events.length > 0, `Soak iteration ${iteration} captured no edges`);
    await configureRestPin(output, "disable");
    await configureRestPin(input, "disable");
  }
}

async function runSection(name: string, action: () => Promise<void>, after?: () => Promise<void>): Promise<void> {
  try {
    await action();
  } catch (error) {
    const message = `${name}: ${error instanceof Error ? error.message : String(error)}`;
    deferredFailures.push(message);
    log(`continuing after section failure: ${message}`);
  } finally {
    if (after !== undefined) {
      try {
        await after();
      } catch (error) {
        const message = `${name} cleanup: ${error instanceof Error ? error.message : String(error)}`;
        deferredFailures.push(message);
        log(`continuing after section cleanup failure: ${message}`);
      }
    }
  }
}

async function releaseTestLocks(): Promise<void> {
  const results = await Promise.allSettled([
    ...Array.from(createdRestLocks, async (id) => {
      await requestJson<void>("DELETE", `/lock/${encodeURIComponent(id)}`, undefined, 204);
      createdRestLocks.delete(id);
    }),
    ...Array.from(createdMcpLocks, async (id) => {
      await mcpTool("delete_lock", { id });
      createdMcpLocks.delete(id);
    }),
  ]);
  const errors = results.flatMap((result) => result.status === "rejected" ? [result.reason] : []);
  if (errors.length > 0) throw new AggregateError(errors, "Failed to release test locks");
}

async function disableTestPins(): Promise<void> {
  await configureRestPin(OUTPUT_A, "disable");
  await configureRestPin(OUTPUT_B, "disable");
}

async function cleanup(): Promise<void> {
  if (!mutationStarted || snapshot === undefined) return;
  log("restoring pre-test state");
  const errors: string[] = [];
  const attempt = async (label: string, action: () => Promise<unknown>): Promise<void> => {
    try {
      await action();
    } catch (error) {
      errors.push(`${label}: ${error instanceof Error ? error.message : String(error)}`);
    }
  };

  await attempt("disable UART", () => requestJson<void>("POST", `/uart/${UART_ID}/config`, uartBody(disabledUart), 204));
  for (const id of createdRestLocks) {
    await attempt(`delete REST lock ${id}`, () => requestJson<void>("DELETE", `/lock/${encodeURIComponent(id)}`, undefined, 204));
  }
  for (const id of createdMcpLocks) await attempt(`delete MCP lock ${id}`, () => mcpTool("delete_lock", { id }));
  if (snapshot.uart.enable) {
    await attempt("restore UART", () => requestJson<void>("POST", `/uart/${UART_ID}/config`, uartBody(snapshot!.uart), 204));
  } else {
    for (let pin = 0; pin <= 7; pin += 1) {
      const state = snapshot.pins.get(pin);
      if (state !== undefined) await attempt(`restore pin ${pin}`, () => restorePin(pin, state, snapshot!.pwm.get(pin)));
    }
  }

  for (const rail of ["3v3", "5v"] as const) {
    await attempt(`restore ${rail} power`, async () => {
      await requestJson<void>("POST", `/power/${rail}`, { enable: snapshot!.power[rail] }, 204);
      const state = await requestJson<{ enable: boolean }>("GET", `/power/${rail}`);
      assert(state.enable === snapshot!.power[rail], `${rail} power state was not restored`);
    });
  }
  if (errors.length > 0) fail(`Cleanup was incomplete:\n- ${errors.join("\n- ")}`);
}

async function main(): Promise<void> {
  if (process.argv.includes("--help")) {
    console.log("SAIHUB_TARGET=192.168.88.160 bun tests/hardware/smoke.ts");
    console.log("Cloud: SAIHUB_TARGET=https://<cloud-host>/device/<device-digest> SAIHUB_ROUTING_TOKEN=<token> bun tests/hardware/smoke.ts");
    console.log("Only connected pins 6 and 7 are driven. Other pins are inventory-checked but never configured or driven.");
    return;
  }
  assert(Number.isFinite(REQUEST_TIMEOUT_MS) && REQUEST_TIMEOUT_MS > 0, "SAIHUB_REQUEST_TIMEOUT_MS must be positive");
  assert(Number.isFinite(UART_TIMEOUT_MS) && UART_TIMEOUT_MS > 0, "SAIHUB_UART_TIMEOUT_MS must be positive");
  assert(Number.isInteger(SOAK_ITERATIONS) && SOAK_ITERATIONS >= 0 && SOAK_ITERATIONS <= 100,
    "SAIHUB_SOAK_ITERATIONS must be an integer from 0 to 100");
  log(`target ${target.origin}${target.pathname}`);
  await testRestMetadata();
  await testMcpProtocol();
  snapshot = await captureSnapshot();
  mutationStarted = true;
  await runSection("inventory", () => testInventorySurfaces(snapshot!));
  await runSection("power", () => testPower(snapshot!));
  await runSection("disable UART", () => requestJson<void>("POST", `/uart/${UART_ID}/config`, uartBody(disabledUart), 204));
  await runSection("REST pins", testRestPins, disableTestPins);
  await runSection("MCP pins", testMcpPins, disableTestPins);
  await runSection("PWM resources and trace races", testPwmResourcesAndTraceRaces);
  await runSection("REST UART", testRestUart);
  await runSection("reset after REST UART", () => requestJson<void>("POST", `/uart/${UART_ID}/config`, uartBody(disabledUart), 204));
  await runSection("MCP UART", testMcpUart);
  await runSection("reset after MCP UART", () => requestJson<void>("POST", `/uart/${UART_ID}/config`, uartBody(disabledUart), 204));
  await runSection("UART payloads and races", testUartPayloadsAndRaces);
  await runSection("reset after UART stress", () => requestJson<void>("POST", `/uart/${UART_ID}/config`, uartBody(disabledUart), 204));
  await runSection("scripts", testScripts);
  await runSection("concurrency", testConcurrency, releaseTestLocks);
  await runSection("lock methods and peripheral races", testLockMethodsAndPeripheralRaces, releaseTestLocks);
  await runSection("locks", testLocks, releaseTestLocks);
  await runSection("soak", testSoak);
  await runSection("overlapping trace race", testOverlappingTraceRace);
  if (deferredFailures.length > 0) {
    for (const message of deferredFailures) console.error(`[hardware-smoke] FAIL: ${message}`);
    fail(`${deferredFailures.length} smoke assertion(s) failed; see FAIL lines above`);
  }
}

let failure: unknown;
try {
  await main();
} catch (error) {
  failure = error;
} finally {
  try {
    await cleanup();
  } catch (cleanupError) {
    failure = failure === undefined ? cleanupError : new AggregateError([failure, cleanupError], "Test and cleanup both failed");
  }
}

if (failure !== undefined) {
  console.error(failure instanceof Error ? failure.stack ?? failure.message : String(failure));
  process.exitCode = 1;
} else if (!process.argv.includes("--help")) {
  log("PASS: REST, MCP, pins 6/7, UART loopback, scripts, locks, trace, PWM, and active power toggles");
}
