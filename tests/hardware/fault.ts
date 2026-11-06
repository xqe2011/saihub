import { createInterface, type Interface } from "node:readline";
import { URL } from "node:url";

// Same connection settings and curl transport as smoke.ts. Run separately: this test needs manual wiring changes.
const DEFAULT_TARGET = "192.168.88.160";
const REQUEST_TIMEOUT_MS = Number.parseInt(process.env.SAIHUB_REQUEST_TIMEOUT_MS ?? "5000", 10);
const FAULT_TIMEOUT_MS = 2000;
const RAILS = ["3v3", "5v"] as const;
const CONNECTORS = { "3v3": "3V3_SW (J2 pin 1) to GND (J2 pin 2)", "5v": "5V_SW (J2 pin 3) to GND (J2 pin 4)" };

type Rail = (typeof RAILS)[number];
type JsonObject = Record<string, unknown>;
type PowerState = { enable: boolean; time: number; reason?: string };
type Surface = {
  name: string;
  read: (rail: Rail) => Promise<unknown>;
  write: (rail: Rail, enable: boolean) => Promise<unknown>;
};

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(message);
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
let nextMcpId = 1;
let mutationStarted = false;
let cleaningUp = false;
let interrupted: string | undefined;
let terminal: Interface | undefined;
let answers: AsyncIterator<string> | undefined;
let passed = 0;

function log(message: string): void {
  console.log(`[hardware-fault] ${message}`);
}

function assertRunning(): void {
  assert(cleaningUp || interrupted === undefined, `Test interrupted by ${interrupted}`);
}

function targetUrl(path: string): URL {
  const basePath = `${target.pathname.replace(/\/$/, "")}/`;
  return new URL(path.replace(/^\//, ""), `${target.protocol}//${target.host}${basePath}`);
}

async function request(method: string, path: string, body?: unknown, expectedStatus = 200): Promise<unknown> {
  assertRunning();
  const headers: Record<string, string> = { Accept: "application/json" };
  if (routingToken) headers.Authorization = `Bearer ${routingToken}`;
  if (body !== undefined) headers["Content-Type"] = "application/json";
  if (path === "/mcp") {
    headers.Accept = "application/json, text/event-stream";
    headers["MCP-Protocol-Version"] = "2025-06-18";
  }
  const args = [
    "curl", "--noproxy", "*", "--silent", "--show-error", "--max-time", String(REQUEST_TIMEOUT_MS / 1000),
    "--request", method, "--url", targetUrl(path).toString(), "--write-out", "\n%{http_code}",
  ];
  for (const [name, value] of Object.entries(headers)) args.push("--header", `${name}: ${value}`);
  if (body !== undefined) args.push("--data-binary", JSON.stringify(body));
  const child = Bun.spawn(args, { stdout: "pipe", stderr: "pipe" });
  const [stdout, stderr, exitCode] = await Promise.all([
    new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited,
  ]);
  assert(exitCode === 0, `${method} ${path} transport failed: ${stderr.trim() || `curl exited ${exitCode}`}`);
  const separator = stdout.lastIndexOf("\n");
  assert(separator >= 0, `${method} ${path} transport omitted HTTP status`);
  const status = Number.parseInt(stdout.slice(separator + 1), 10);
  const text = stdout.slice(0, separator);
  assert(status === expectedStatus, `${method} ${path} returned ${status}, expected ${expectedStatus}: ${text}`);
  if (expectedStatus === 204 || expectedStatus === 202) {
    assert(text.length === 0, `${method} ${path} returned a non-empty body`);
    return undefined;
  }
  try {
    return JSON.parse(text) as unknown;
  } catch {
    throw new Error(`${method} ${path} returned invalid JSON: ${text.slice(0, 240)}`);
  }
}

async function mcpRequest(method: string, params?: JsonObject, notification = false): Promise<JsonObject> {
  const id = notification ? undefined : nextMcpId++;
  const envelope = await request("POST", "/mcp", { jsonrpc: "2.0", id, method, params }, notification ? 202 : 200);
  if (notification) return {};
  assert(isObject(envelope) && envelope.jsonrpc === "2.0" && envelope.id === id, `MCP ${method}: invalid envelope`);
  assert(envelope.error === undefined, `MCP ${method}: ${JSON.stringify(envelope.error)}`);
  assert(isObject(envelope.result), `MCP ${method} omitted result`);
  return envelope.result;
}

async function mcpTool(name: string, args: JsonObject): Promise<unknown> {
  const result = await mcpRequest("tools/call", { name, arguments: args });
  const content = result.content;
  assert(Array.isArray(content) && content.length > 0, `MCP ${name} returned no content`);
  const first: unknown = content[0];
  assert(isObject(first) && first.type === "text" && typeof first.text === "string", `MCP ${name}: invalid content`);
  assert(result.isError !== true, `MCP ${name} failed: ${first.text}`);
  try {
    return JSON.parse(first.text) as unknown;
  } catch {
    throw new Error(`MCP ${name} returned invalid JSON text: ${first.text.slice(0, 240)}`);
  }
}

async function runScript(viaMcp: boolean, script: string, calls: number): Promise<unknown> {
  const body = { script, maxCalls: calls, timeout: 1000000 };
  const response = viaMcp ? await mcpTool("run_script", body) : await request("POST", "/script", body);
  assert(isObject(response), "Script returned a non-object envelope");
  assert(response.calls === calls, `Script used ${String(response.calls)} tool calls, expected ${calls}`);
  assert(response.output === "", "Script returned unexpected output");
  assert(typeof response.elapsed === "number" && response.elapsed >= 0, "Script omitted elapsed time");
  return response.result;
}

const surfaces: Surface[] = [
  {
    name: "REST",
    read: (rail) => request("GET", `/power/${rail}`),
    write: (rail, enable) => request("POST", `/power/${rail}`, { enable }, 204),
  },
  {
    name: "MCP",
    read: (rail) => mcpTool("get_output_power_state", { rail }),
    write: (rail, enable) => mcpTool("set_output_power_state", { rail, enable }),
  },
  ...[false, true].map((viaMcp): Surface => ({
    name: `script/${viaMcp ? "MCP" : "REST"}`,
    read: (rail) => runScript(viaMcp, `return get_output_power_state{rail="${rail}"}`, 1),
    write: (rail, enable) => runScript(viaMcp, `set_output_power_state{rail="${rail}",enable=${enable}}`, 1),
  })),
];

async function readState(surface: Surface, rail: Rail): Promise<PowerState> {
  const state = await surface.read(rail);
  const label = `${surface.name} ${rail}`;
  assert(isObject(state) && typeof state.enable === "boolean", `${label} omitted enable state`);
  assert(typeof state.time === "number" && Number.isFinite(state.time) && state.time > 0, `${label} omitted time`);
  assert(state.reason === undefined || (typeof state.reason === "string" && state.reason.trim().length > 0),
    `${label} reason must be absent or a non-empty string`);
  return state as PowerState;
}

async function expectState(rail: Rail, enable: boolean, reason?: string): Promise<void> {
  // Scripts execute sequentially because only one script can run on the board at a time.
  for (const surface of surfaces) {
    const state = await readState(surface, rail);
    assert(state.enable === enable, `${surface.name} ${rail}: enable=${state.enable}, expected ${enable}`);
    assert(state.reason === reason,
      `${surface.name} ${rail}: reason=${JSON.stringify(state.reason)}, expected ${JSON.stringify(reason)}`);
  }
}

async function waitForFault(surface: Surface, rail: Rail): Promise<string> {
  const deadline = performance.now() + FAULT_TIMEOUT_MS;
  let state: PowerState;
  do {
    state = await readState(surface, rail);
    if (!state.enable && state.reason !== undefined) {
      assert(/detected at (?:\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z|\d+ us)\. Output disabled\.$/.test(state.reason),
        `${surface.name} ${rail} fault reason omitted its timestamp or disabled-output explanation: ${state.reason}`);
      return state.reason;
    }
    await Bun.sleep(50);
  } while (performance.now() < deadline);
  throw new Error(
    `${surface.name} ${rail} did not latch a fault within ${FAULT_TIMEOUT_MS} ms: ${JSON.stringify(state)}`,
  );
}

async function confirm(instructions: string): Promise<void> {
  assertRunning();
  if (terminal === undefined) {
    terminal = createInterface({ input: process.stdin, output: process.stdout });
    answers = terminal[Symbol.asyncIterator]();
    terminal.on("SIGINT", () => stop("SIGINT"));
  }
  log(instructions);
  process.stdout.write("Press Enter to continue, or type quit to stop: ");
  const answer = await answers!.next();
  assertRunning();
  assert(!answer.done, "Input closed before confirmation; test cancelled");
  assert(answer.value.trim().toLowerCase() !== "quit", "Test cancelled by user");
}

async function testRail(rail: Rail): Promise<void> {
  const other: Rail = rail === "3v3" ? "5v" : "3v3";
  log(`${rail}: healthy output toggles through REST, MCP, and both script transports`);
  for (const surface of surfaces) {
    await surface.write(rail, true);
    await expectState(rail, true);
    await surface.write(rail, false);
    await expectState(rail, false);
    await expectState(other, false);
  }

  await confirm(`Both outputs are off. Short ONLY ${CONNECTORS[rail]}. Leave the other rail unshorted.`);
  await request("POST", `/power/${other}`, { enable: true }, 204);
  await expectState(other, true);
  let reason = "";
  for (const surface of surfaces) {
    log(`${rail}: trigger protection via ${surface.name}`);
    // A successful write acknowledges the command; the following reads must report the protective shutdown.
    try {
      await surface.write(rail, true);
      reason = await waitForFault(surface, rail);
      await expectState(rail, false, reason);
      await Bun.sleep(100);
      await expectState(rail, false, reason);
      await expectState(other, true);
      await surface.write(rail, false);
      await expectState(rail, false, reason);
    } finally {
      // Disable even when the expected protection or response fails.
      await request("POST", `/power/${rail}`, { enable: false }, 204);
    }
    passed += 1;
    log(`PASS ${rail} ${surface.name}: shutdown, shared timestamped reason, latch, and rail isolation`);
  }

  await request("POST", `/power/${other}`, { enable: false }, 204);
  await expectState(other, false);
  await confirm(`Both outputs are off. REMOVE the ${rail}-to-GND short; leave both rails unshorted.`);
  await expectState(rail, false, reason);
  for (const surface of surfaces) {
    await surface.write(rail, true);
    await expectState(rail, true);
    await Bun.sleep(100);
    await expectState(rail, true);
    await surface.write(rail, false);
    await expectState(rail, false);
    await expectState(other, false);
    passed += 1;
    log(`PASS ${rail} ${surface.name}: retry clears reason and output recovers`);
  }
}

function stop(signal: string): void {
  interrupted = signal;
  terminal?.close();
}

async function cleanup(): Promise<void> {
  terminal?.close();
  if (!mutationStarted) return;
  cleaningUp = true;
  const errors: string[] = [];
  // Never restore an enabled snapshot: a physical short may still be present after an interrupted test.
  for (const rail of RAILS) {
    try {
      await request("POST", `/power/${rail}`, { enable: false }, 204);
      const state = await readState(surfaces[0]!, rail);
      assert(!state.enable, `${rail} remained enabled`);
    } catch (error) {
      errors.push(`${rail}: ${error instanceof Error ? error.message : String(error)}`);
    }
  }
  assert(errors.length === 0, `Could not confirm outputs off: ${errors.join("; ")}`);
  log("Cleanup: both power outputs are off. Remove any remaining shorting jumpers.");
}

async function main(): Promise<void> {
  if (process.argv.includes("--help")) {
    console.log("SAIHUB_TARGET=192.168.88.160 bun tests/hardware/fault.ts");
    console.log("Same env as smoke.ts: SAIHUB_TARGET (or SAIHUB_IP), SAIHUB_ROUTING_TOKEN, SAIHUB_REQUEST_TIMEOUT_MS.");
    console.log("Supports local and cloud device URLs. Default request timeout: 5000 ms.");
    console.log("Start with loads disconnected and both rails unshorted. Press Enter at each short/remove prompt.");
    console.log("Short only a switched power output to adjacent GND. Never connect the 3V3 and 5V outputs together.");
    console.log("Checks both rails over REST, MCP, and Lua scripts via REST and MCP. Leaves both power outputs off.");
    console.log("Type quit or press Ctrl+C to cancel; cleanup attempts to switch both outputs off.");
    return;
  }
  assert(Number.isFinite(REQUEST_TIMEOUT_MS) && REQUEST_TIMEOUT_MS > 0, "SAIHUB_REQUEST_TIMEOUT_MS must be positive");
  log(`target ${target.origin}${target.pathname}`);
  log("Start with external loads disconnected and both rails unshorted.");
  for (const rail of RAILS) await readState(surfaces[0]!, rail);
  const initialized = await mcpRequest("initialize", {
    protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "hardware-fault", version: "1.0.0" },
  });
  assert(initialized.protocolVersion === "2025-06-18", "MCP initialize negotiated an unexpected protocol version");
  await mcpRequest("notifications/initialized", undefined, true);
  mutationStarted = true;
  for (const rail of RAILS) await request("POST", `/power/${rail}`, { enable: false }, 204);
  // Start clean even if a previous test left a latched reason. An explicit retry is the only way to clear it.
  for (const rail of RAILS) {
    await request("POST", `/power/${rail}`, { enable: true }, 204);
    await expectState(rail, true);
    await request("POST", `/power/${rail}`, { enable: false }, 204);
    await expectState(rail, false);
  }
  for (const rail of RAILS) await testRail(rail);
}

const onSigint = () => stop("SIGINT");
const onSigterm = () => stop("SIGTERM");
process.on("SIGINT", onSigint);
process.on("SIGTERM", onSigterm);
let failure: unknown;
try {
  await main();
} catch (error) {
  failure = error;
} finally {
  try {
    await cleanup();
  } catch (error) {
    failure = failure === undefined ? error : new AggregateError([failure, error], "Test and cleanup both failed");
  }
  process.off("SIGINT", onSigint);
  process.off("SIGTERM", onSigterm);
}
if (failure !== undefined) {
  const errors = failure instanceof AggregateError ? failure.errors : [failure];
  const detail = errors.map((error) => error instanceof Error ? error.message : String(error)).join("; ");
  console.error(`[hardware-fault] FAIL: ${detail}`);
  process.exitCode = interrupted === "SIGINT" ? 130 : interrupted === "SIGTERM" ? 143 : 1;
} else if (!process.argv.includes("--help")) {
  log(`PASS: ${passed} fault/recovery cases across both rails; REST, MCP, and scripts; both outputs off`);
}
