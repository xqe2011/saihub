import { URL } from "node:url";
import { readFileSync } from "node:fs";

type Json = Record<string, any>;
type PinState = { pin: number; mode: string; openDrain: boolean; pullUp: boolean; pullDown: boolean };
type SpiEvent = { receive: number[]; send: number[]; startTime: number; endTime: number };
type TraceEvent = { pin: number; edge: string; level: number; time: number };
type Snapshot = { pins: PinState[]; uart: Json; spi: Json; power: Record<string, boolean> };
type Result = { status: number; body: any; text: string };
type Check = { name: string; passed: boolean; detail?: string };

// TEST_PINS is either one pin-to-pin row, or two board rows with corresponding columns connected.
const help = process.argv.includes("--help");
const timeout = Number(process.env.TEST_TIMEOUT_MS ?? 15000);
const rounds = Number(process.env.TEST_ROUNDS ?? 3);
const soak = Number(process.env.TEST_SOAK_ITERATIONS ?? 32);
// Drain regularly during payload tests.
const SPI_DRAIN_BATCH = 3;
const reportPath = process.env.TEST_REPORT;
const testFilter = process.env.TEST_SECTIONS ? new RegExp(process.env.TEST_SECTIONS, "i") : undefined;
const checks: Check[] = [];
const failures: string[] = [];
const report: Json = { started: new Date().toISOString(), checks, failures, requests: [], cleanup: [] };
let boards: Board[] = [];
let pinRows: number[][] = [];
let sequence = 1;
const seed = Number(process.env.TEST_SEED ?? 20261101);
let randomState = seed >>> 0;
let stopping = false;
process.on("SIGINT", () => { stopping = true; });
process.on("SIGTERM", () => { stopping = true; });

function assert(ok: unknown, name: string, detail?: string): asserts ok {
  checks.push({ name, passed: Boolean(ok), ...(detail ? { detail } : {}) });
  if (!ok) throw new Error(name + (detail ? ": " + detail : ""));
}

function equal(actual: unknown, expected: unknown, name: string): void {
  assert(JSON.stringify(actual) === JSON.stringify(expected), name,
    JSON.stringify({ expected, actual }).slice(0, 1400));
}

function bytes(length: number): number[] {
  return Array.from({ length }, () => {
    randomState ^= randomState << 13;
    randomState ^= randomState >>> 17;
    randomState ^= randomState << 5;
    return randomState >>> 24;
  });
}

function log(message: string): void {
  console.log("[hardware-pair] " + message);
}

function bodyConfig(config: Json): Json {
  const { id, ...body } = config;
  if (body.role === "slave") delete body.clockHz;
  return body;
}

class Board {
  url: URL;
  row: number[];
  snapshot?: Snapshot;
  uart: Json = { enable: false, baudRate: 115200, dataBits: 8, parity: "none", stopBits: 1,
    encoding: "byte", pins: { rx: null, tx: null } };
  spi: Json;
  locks = new Set<string>();
  touched = false;

  constructor(address: string, row: number[]) {
    this.url = new URL(address.includes("://") ? address : "http://" + address);
    this.row = row;
    this.spi = this.spiConfig("master", 0, "msb", 1000000, false);
  }

  get name(): string { return this.url.host; }

  async raw(method: string, path: string, body?: unknown, extra: Record<string, string> = {}): Promise<Result> {
    if (stopping) throw new Error("Test interrupted; restoring board state");
    const url = new URL(this.url.toString().replace(/\/$/, "") + "/" + path.replace(/^\//, ""));
    const serialized = body === undefined ? undefined : JSON.stringify(body);
    const headers: Record<string, string> = { Accept: "application/json", ...extra };
    if (serialized !== undefined) {
      headers["Content-Type"] = "application/json";
      headers["Content-Length"] = String(Buffer.byteLength(serialized));
    }
    if (process.env.TEST_ROUTING_TOKEN) headers.Authorization = "Bearer " + process.env.TEST_ROUTING_TOKEN;
    const started = performance.now();
    // Match smoke.ts: direct board requests must not use the computer's proxy environment.
    const args = ["curl", "--noproxy", "*", "--silent", "--show-error", "--max-time", String(timeout / 1000),
      "--request", method, "--url", url.toString(), "--write-out", "\\n%{http_code}"];
    for (const [name, value] of Object.entries(headers)) args.push("--header", name + ": " + value);
    if (serialized !== undefined) args.push("--data-binary", serialized);
    const child = Bun.spawn(args, { stdout: "pipe", stderr: "pipe" });
    const [stdout, stderr, exitCode] = await Promise.all([
      new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited,
    ]);
    if (exitCode !== 0) {
      report.requests.push({ board: this.name, method, path, transportError: stderr.trim() });
      throw new Error(this.name + " " + method + " " + path + ": " + stderr.trim());
    }
    const separator = stdout.lastIndexOf("\n");
    if (separator < 0) throw new Error("HTTP status missing from response");
    const status = Number(stdout.slice(separator + 1));
    const text = stdout.slice(0, separator);
    let parsed: unknown = text;
    try { parsed = text ? JSON.parse(text) : undefined; } catch {}
    report.requests.push({ board: this.name, method, path, status,
      elapsedMs: Math.round(performance.now() - started) });
    return { status, body: parsed, text };
  }

  async rest(method: string, path: string, body?: unknown, expected = 200): Promise<any> {
    const result = await this.raw(method, path, body);
    assert(result.status === expected, this.name + " " + method + " " + path + " status",
      "expected " + expected + ", got " + result.status + " " + result.text.slice(0, 400));
    if (expected === 204 || expected === 202) return;
    if (expected >= 400) return result.body;
    assert(typeof result.body === "object" && result.body !== null, this.name + " " + path + " JSON");
    return result.body;
  }

  async rpc(method: string, params?: Json): Promise<Json> {
    const result = await this.raw("POST", "/mcp", { jsonrpc: "2.0", id: sequence++, method, params }, {
      Accept: "application/json, text/event-stream", "MCP-Protocol-Version": "2025-06-18",
    });
    assert(result.status === 200 && result.body?.result && !result.body.error,
      this.name + " MCP " + method, result.text.slice(0, 400));
    return result.body.result;
  }

  async tool(name: string, args: Json = {}): Promise<any> {
    const result = await this.rpc("tools/call", { name, arguments: args });
    assert(!result.isError && typeof result.content?.[0]?.text === "string",
      this.name + " tool " + name, JSON.stringify(result).slice(0, 400));
    return JSON.parse(result.content[0].text);
  }

  async pin(pin: number, mode: string, mcp = false, extra: Json = {}): Promise<void> {
    const config = { mode, openDrain: false, pullUp: false, pullDown: false, ...extra };
    if (mcp) await this.tool("configure_pins", { pins: [pin], ...config });
    else await this.rest("PUT", "/pin/" + pin, config, 204);
  }

  async level(pin: number, level: number, mcp = false): Promise<void> {
    if (mcp) await this.tool("set_pin_levels", { pins: [pin], level });
    else await this.rest("POST", "/pin/" + pin + "/level", { level }, 204);
  }

  async levels(mcp = false): Promise<number[]> {
    if (mcp) return (await this.tool("get_pin_levels", { pins: this.row })).levels;
    const inventory = await this.rest("GET", "/pin/");
    return this.row.map((pin) => inventory.pins.find((state: Json) => state.pin === pin).level);
  }

  spiConfig(role: string, mode: number, order: string, hz: number, enable = true): Json {
    return { enable, role, cpol: mode >> 1, cpha: mode & 1, bitOrder: order,
      ...(role === "master" ? { clockHz: hz } : {}),
      pins: { mosi: this.row[1], miso: this.row[0], clk: this.row[2], cs: this.row[3] } };
  }

  async configureSpi(config: Json, mcp = false): Promise<void> {
    if (mcp) await this.tool("configure_spi", { id: 0, ...config });
    else await this.rest("POST", "/spi/0/config", config, 204);
    this.spi = config;
  }

  async configureUart(config: Json, mcp = false): Promise<void> {
    if (mcp) await this.tool("configure_uart", { id: 0, ...config });
    else await this.rest("POST", "/uart/0/config", config, 204);
    this.uart = config;
  }

  async release(): Promise<void> {
    if (this.spi.enable) await this.configureSpi({ ...this.spi, enable: false });
    if (this.uart.enable) await this.configureUart({ ...this.uart, enable: false });
    for (const pin of this.row) await this.pin(pin, "disable");
  }

  async transfer(data: number[], mcp = false): Promise<number[]> {
    const result = mcp ? await this.tool("spi_master_transceive", { id: 0, data })
      : await this.rest("POST", "/spi/0/master/transceive", { data });
    return result.data;
  }

  async events(mcp = false): Promise<SpiEvent[]> {
    const result = mcp ? await this.tool("spi_slave_receive", { id: 0 })
      : await this.rest("GET", "/spi/0/slave/receive");
    return result.events;
  }

  async mapping(mapping: Json[], mcp = false): Promise<void> {
    if (mcp) await this.tool("spi_slave_transmit", { id: 0, mapping });
    else await this.rest("POST", "/spi/0/slave/transmit", { mapping }, 204);
  }

  async trace(pin: number, edge = "both", duration = 20000, mcp = false): Promise<TraceEvent[]> {
    const result = mcp ? await this.tool("trace_pins", { pins: [pin], edge, duration })
      : await this.rest("GET", "/pin/" + pin + "/trace?edge=" + edge + "&duration=" + duration);
    return result.events;
  }
}

async function releaseAll(): Promise<void> {
  // Release both ends before changing which board drives a wire.
  for (const board of boards) await board.release();
}

async function section(name: string, action: () => Promise<void>, release = true): Promise<boolean> {
  const preflight = name.includes("metadata and MCP") || name === "GPIO wiring matrix, both directions";
  if (testFilter && !preflight && !testFilter.test(name)) {
    report.skipped ??= [];
    report.skipped.push(name);
    return true;
  }
  log(name);
  let passed = true;
  try { await action(); } catch (error) {
    failures.push(name + ": " + (error instanceof Error ? error.message : String(error)));
    log("FAIL " + failures.at(-1));
    passed = false;
  } finally {
    if (release) {
      try { await releaseAll(); } catch (error) { throw new Error("Cannot safely release test pins: " + error); }
    }
  }
  if (reportPath) await Bun.write(reportPath, JSON.stringify(report, null, 2) + "\n");
  return passed;
}

async function preflight(): Promise<void> {
  const identities = new Set<string>();
  for (const board of boards) {
    const system = await board.rest("GET", "/system");
    assert(typeof system.digest === "string" && !identities.has(system.digest), "Two distinct board identities");
    identities.add(system.digest);
    report.boards ??= [];
    report.boards.push({ address: board.url.toString(), pins: board.row, system });
    const pins = (await board.rest("GET", "/pin/")).pins as PinState[];
    const uart = (await board.rest("GET", "/uart/")).uarts[0];
    const spi = (await board.rest("GET", "/spi/")).spis[0];
    assert(!uart.enable && !spi.enable, board.name + " UART and SPI must initially be disabled");
    for (const pin of board.row) {
      assert(pins.find((state) => state.pin === pin)?.mode === "disable",
        board.name + " pin " + pin + " must initially be disabled");
    }
    const power: Record<string, boolean> = {};
    for (const rail of ["3v3", "5v"]) power[rail] = (await board.rest("GET", "/power/" + rail)).enable;
    board.snapshot = { pins, uart, spi, power };
    report.boards.at(-1).snapshot = board.snapshot;
  }
  for (const board of boards) board.touched = true;
}

async function metadata(board: Board): Promise<void> {
  const spec = await board.rest("GET", "/openapi.json");
  equal(spec.openapi, "3.1.0", board.name + " OpenAPI version");
  equal(spec.components.schemas.SpiSlaveMapEntry.properties.miso.maxItems, 255, "Slave map limit");
  equal(spec.components.schemas.SpiSlaveEvent.properties.receive.maxItems, 255, "Slave event limit");
  await board.rest("OPTIONS", "/pin/", undefined, 204);
  await board.rest("GET", "/does-not-exist", undefined, 404);
  await board.rest("GET", "/mcp", undefined, 405);
  const init = await board.rpc("initialize", { protocolVersion: "2025-06-18", capabilities: {},
    clientInfo: { name: "hardware-pair", version: "1" } });
  equal(init.protocolVersion, "2025-06-18", "MCP negotiated version");
  const notification = await board.raw("POST", "/mcp", {
    jsonrpc: "2.0", method: "notifications/initialized",
  }, { "MCP-Protocol-Version": "2025-06-18" });
  equal(notification.status, 202, "MCP notification status");
  equal(await board.rpc("ping"), {}, "MCP ping");
  const listed = await board.rpc("tools/list");
  const expected = JSON.parse(readFileSync(new URL("../../mcp.json", import.meta.url), "utf8")).tools as Json[];
  for (const tool of expected) {
    assert(listed.tools.some((entry: Json) => entry.name === tool.name), "MCP inventory " + tool.name);
  }
  equal((await board.tool("list_pins")).pins.length, 8, "MCP pin inventory");
  equal((await board.tool("list_uarts")).uarts[0].id, 0, "MCP UART inventory");
  equal((await board.tool("list_spis")).spis[0].id, 0, "MCP SPI inventory");
  const invalid = await board.raw("POST", "/mcp", { jsonrpc: "1.0", id: 1, method: "ping" });
  equal(invalid.status, 400, "Invalid MCP envelope");
}

async function powerBuzzerScripts(board: Board): Promise<void> {
  for (const rail of ["3v3", "5v"]) {
    const original = board.snapshot!.power[rail];
    await board.rest("POST", "/power/" + rail, { enable: !original }, 204);
    equal((await board.tool("get_output_power_state", { rail })).enable, !original, "REST power toggle");
    await board.tool("set_output_power_state", { rail, enable: original });
    equal((await board.rest("GET", "/power/" + rail)).enable, original, "MCP power restore");
  }
  await board.rest("POST", "/buzzer", { sequence: "." }, 204);
  await board.tool("play_buzzer", { sequence: "." });
  await board.rest("POST", "/buzzer", { sequence: "invalid" }, 400);
  for (const mcp of [false, true]) {
    const pin = board.row[0]!;
    const script = "configure_pins{pins={" + pin + "},"
      + "mode='digitalOutput',openDrain=false,pullUp=false,pullDown=false}\n"
      + "set_pin_levels{pins={" + pin + "},level=1}\n"
      + "local v=get_pin_levels{pins={" + pin + "}}.levels[1]\n"
      + "set_pin_levels{pins={" + pin + "},level=0}\n"
      + "print('pair-script')\nreturn {ok=v==1}";
    const args = { script, maxCalls: 8, timeout: 1000000 };
    const result = mcp ? await board.tool("run_script", args) : await board.rest("POST", "/script", args);
    assert(result.result?.ok && result.output.includes("pair-script"), "Script hardware calls and output");
    await board.pin(pin, "disable");
  }
  await board.rest("POST", "/script", { script: "invalid (", maxCalls: 2, timeout: 100000 }, 400);
  await board.rest("POST", "/script", {
    script: "list_pins{}\nlist_pins{}", maxCalls: 1, timeout: 200000,
  }, 422);
  await board.rest("POST", "/script", { script: "sleep(200000)", maxCalls: 1, timeout: 50000 }, 422);
  const oversized = await board.raw("POST", "/script", {
    script: "local t={} for i=1,200 do t[i]=string.rep('x',1024) end return true",
    maxCalls: 1, timeout: 1000000,
  });
  assert(oversized.status === 422 && oversized.text.includes("memory"), "Script memory budget");
  const afterOom = await board.tool("run_script", { script: "return true", maxCalls: 1, timeout: 100000 });
  equal(afterOom.result, true, "Script runner recovered after memory rejection");
}

async function wiring(): Promise<void> {
  for (const [source, sink] of [boards, boards.toReversed()]) {
    await releaseAll();
    for (const pin of sink!.row) await sink!.pin(pin, "digitalInput", false, { pullDown: true });
    for (let column = 0; column < source!.row.length; column++) {
      const output = source!.row[column]!;
      for (const mode of ["digitalOutput", "digitalInputOutput", "openDrain"]) {
        const mcp = mode !== "digitalOutput";
        const openDrain = mode === "openDrain";
        if (openDrain) await sink!.pin(sink!.row[column]!, "digitalInput", mcp, { pullUp: true });
        await source!.pin(output, mode === "openDrain" ? "digitalOutput" : mode, mcp, { openDrain });
        for (const level of [0, 1, 0, 1]) {
          await source!.level(output, level, mcp);
          const expected = source!.row.map((_, index) => index === column ? level : 0);
          equal(await sink!.levels(!mcp), expected, source!.name + "->" + sink!.name + " wire " + column);
        }
        await source!.pin(output, "disable");
        if (openDrain) await sink!.pin(sink!.row[column]!, "digitalInput", false, { pullDown: true });
      }
    }
  }
}

function frequency(events: TraceEvent[]): number {
  const rising = events.filter((event) => event.edge === "raising").map((event) => event.time);
  const periods = rising.slice(1).map((time, index) => time - rising[index]!).filter((period) => period > 0);
  periods.sort((a, b) => a - b);
  assert(periods.length >= 2, "Enough PWM periods captured");
  return 1000000 / periods[Math.floor(periods.length / 2)]!;
}

async function pwmTracePulse(): Promise<void> {
  for (const [source, sink] of [boards, boards.toReversed()]) {
    await releaseAll();
    for (let column = 0; column < source!.row.length; column++) {
      const output = source!.row[column]!;
      const input = sink!.row[column]!;
      await sink!.pin(input, "digitalInput");
      await source!.pin(output, "pwmOutput", true);
      for (const [index, hz] of [500, 2000, 10000, 50000].entries()) {
        const duty = [10, 25, 50, 90][index]!;
        if (index % 2) await source!.tool("set_pin_pwms", { pins: [output], frequency: hz, duty });
        else await source!.rest("POST", "/pin/" + output + "/pwm", { frequency: hz, duty }, 204);
        const state = (await source!.tool("get_pin_pwms", { pins: [output] })).pwms[0];
        assert(Math.abs(state.frequency - hz) <= Math.max(2, hz * 0.02), "PWM frequency readback");
        assert(Math.abs(state.duty - duty) <= 1, "PWM duty readback");
        // High-frequency readback is checked above; interrupt tracing is not a 50 kHz waveform analyzer.
        if (hz <= 10000) {
          const events = await sink!.trace(input, "both", 10000, index % 2 === 0);
          assert(events.some((event) => event.edge === "raising") && events.some((event) => event.edge === "falling"),
            "PWM physical edges " + source!.name + ":" + output + " at " + hz);
          if (hz <= 2000) {
            const measured = frequency(events);
            assert(Math.abs(measured - hz) / hz < 0.1, "Measured PWM frequency", String(measured));
          }
        }
      }
      for (const duty of [0, 100]) {
        await source!.rest("POST", "/pin/" + output + "/pwm", { frequency: 1000, duty }, 204);
        equal((await sink!.levels())[column], duty === 0 ? 0 : 1, "PWM constant endpoint");
      }
      await source!.pin(output, "disable");
      await sink!.pin(input, "disable");
    }
    // Four independent output channels, with the other board providing four independent probes.
    for (let column = 0; column < 4; column++) {
      await source!.pin(source!.row[column]!, "pwmOutput");
      await sink!.pin(sink!.row[column]!, "digitalInput");
      await source!.rest("POST", "/pin/" + source!.row[column] + "/pwm",
        { frequency: [500, 1000, 2000, 4000][column], duty: 50 }, 204);
    }
    for (let column = 0; column < 4; column++) {
      assert((await sink!.trace(sink!.row[column]!, "both", 10000)).length > 0, "Four-channel PWM probe");
    }
    await releaseAll();
    const output = source!.row[0]!;
    const input = sink!.row[0]!;
    await source!.pin(output, "digitalOutput");
    await source!.level(output, 0);
    await sink!.pin(input, "digitalInput");
    const trace = sink!.trace(input, "both", 1000000, true);
    await Bun.sleep(100);
    await source!.tool("pulse_pins", { pins: [output], width: 2000, level: 1 });
    const edges = await trace;
    assert(edges.some((event) => event.edge === "raising") && edges.some((event) => event.edge === "falling"),
      "Pulse captured on opposite board");
    equal((await sink!.levels())[0], 0, "Pulse returned low");
  }
}

async function uartPair(encoding = "byte", condition: Json = {}, mcp = false): Promise<void> {
  await releaseAll();
  for (const [index, board] of boards.entries()) {
    await board.configureUart({ enable: true, baudRate: 115200, dataBits: 8, parity: "none", stopBits: 1,
      encoding, ...condition, pins: { tx: board.row[index], rx: board.row[1 - index] } }, mcp);
  }
  // Enabling either receiver before the opposite transmitter is idle can collect startup bytes.
  await Bun.sleep(20);
  for (const board of boards) await board.rest("POST", "/uart/0/flush", undefined, 204);
}

async function uartReceive(board: Board, expected: number[] | string, mcp: boolean): Promise<void> {
  let received: number[] | string = typeof expected === "string" ? "" : [];
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline && received.length < expected.length) {
    const result = mcp ? await board.tool("uart_receive", { id: 0 }) : await board.rest("GET", "/uart/0/receive");
    received = typeof received === "string" ? received + result.data : [...received, ...result.data];
    if (received.length < expected.length) await Bun.sleep(10);
  }
  equal(received, expected, board.name + " UART received exact payload");
}

async function uart(): Promise<void> {
  const conditions = [
    { baudRate: 9600, dataBits: 8, parity: "none", stopBits: 1 },
    { baudRate: 57600, dataBits: 7, parity: "even", stopBits: 1 },
    { baudRate: 115200, dataBits: 8, parity: "odd", stopBits: 2 },
    { baudRate: 1000000, dataBits: 8, parity: "none", stopBits: 1 },
  ];
  for (const [index, condition] of conditions.entries()) {
    await uartPair("byte", condition, index % 2 === 0);
    for (const [source, sink] of [boards, boards.toReversed()]) {
      const data = bytes(64).map((byte) => byte & (condition.dataBits === 7 ? 127 : 255));
      await source!.tool("uart_transmit", { id: 0, data });
      await uartReceive(sink!, data, false);
    }
  }
  for (const encoding of ["byte", "utf8"]) {
    await uartPair(encoding);
    for (const [source, sink] of [boards, boards.toReversed()]) {
      const data = encoding === "byte" ? bytes(256) : "paired UART 你好 π ".repeat(5);
      await source!.tool("uart_transmit", { id: 0, data });
      await uartReceive(sink!, data, false);
      const empty = encoding === "byte" ? [] : "";
      await source!.rest("POST", "/uart/0/transmit", { data: empty }, 204);
      equal((await sink!.tool("uart_receive", { id: 0 })).data, empty, "Empty UART transmit");
      await source!.rest("POST", "/uart/0/transmit", { data }, 204);
      await Bun.sleep(30);
      await sink!.tool("uart_flush", { id: 0 });
      equal((await sink!.rest("GET", "/uart/0/receive")).data, empty, "UART flush");
      const oversized = encoding === "byte" ? bytes(2049) : "x".repeat(2049);
      await source!.rest("POST", "/uart/0/transmit", { data: oversized }, 400);
    }
  }
  // Keep maximum-size requests isolated so a failure does not hide the other UART checks.
  for (const encoding of ["byte", "utf8"]) {
    for (const [source, sink] of [boards, boards.toReversed()]) {
      await section("UART maximum " + encoding + " payload " + source!.name, async () => {
        await uartPair(encoding);
        const data = encoding === "byte" ? bytes(2048) : "x".repeat(2048);
        await source!.tool("uart_transmit", { id: 0, data });
        await uartReceive(sink!, data, false);
      });
    }
  }
  await uartPair();
  const source = boards[0]!;
  const sink = boards[1]!;
  const chunks = [[1, 2, 3, 4], [11, 12, 13, 14], [21, 22, 23, 24], [31, 32, 33, 34]];
  await Promise.all(chunks.map((data) => source.tool("uart_transmit", { id: 0, data })));
  const all: number[] = [];
  const end = Date.now() + 3000;
  while (all.length < 16 && Date.now() < end) {
    all.push(...(await sink.rest("GET", "/uart/0/receive")).data);
    if (all.length < 16) await Bun.sleep(10);
  }
  const observed = Array.from({ length: 4 }, (_, index) => JSON.stringify(all.slice(index * 4, index * 4 + 4)));
  equal(observed.sort(), chunks.map((chunk) => JSON.stringify(chunk)).sort(), "Concurrent UART intact chunks");
  await source.rest("PUT", "/pin/" + source.row[0], { mode: "digitalOutput" }, 423);
  await source.rest("POST", "/spi/0/config", source.spiConfig("master", 0, "msb", 1000000), 409);
}

async function locksAndConcurrency(board: Board): Promise<void> {
  const pin = board.row[0]!;
  await board.pin(pin, "digitalOutput");
  await board.level(pin, 0);
  const resources = [{ type: "pin", pins: [pin], method: ["read", "write"] }];
  const race = await Promise.all([
    board.raw("POST", "/lock", { resources }), board.raw("POST", "/lock", { resources }),
  ]);
  for (const winner of race.filter((result) => result.status === 201)) board.locks.add(winner.body.id);
  equal(race.map((result) => result.status).sort(), [201, 409], "Concurrent lock acquisition");
  const id = race.find((result) => result.status === 201)!.body.id;
  await board.rest("GET", "/pin/" + pin + "/level", undefined, 412);
  equal((await board.tool("get_pin_levels", { pins: [pin], lockId: id })).levels, [0], "MCP uses REST lock");
  equal((await board.tool("renew_lock", { id })).id, id, "Lock renewal");
  await board.tool("delete_lock", { id });
  board.locks.delete(id);
  await board.rest("DELETE", "/lock/" + id, undefined, 204);
  const second = await board.tool("create_lock", { resources });
  board.locks.add(second.id);
  await board.rest("PUT", "/lock/" + second.id);
  await board.rest("DELETE", "/lock/" + second.id, undefined, 204);
  board.locks.delete(second.id);
  const reads = await Promise.all(Array.from({ length: 8 }, () => board.tool("get_pin_levels", { pins: [pin] })));
  assert(reads.every((result) => result.levels[0] === 0), "Concurrent pin reads");
  const scripts = await Promise.all([0, 1].map(() => board.raw("POST", "/script", {
    script: "sleep(250000)\nreturn true", maxCalls: 1, timeout: 500000,
  })));
  equal(scripts.map((result) => result.status).sort(), [200, 409], "Concurrent script exclusion");
}

async function spiSetup(master: Board, slave: Board, mode = 0, order = "msb", hz = 1000000): Promise<void> {
  for (const board of boards) {
    if (board.spi.enable) await board.configureSpi({ ...board.spi, enable: false });
  }
  await master.configureSpi(master.spiConfig("master", mode, order, hz));
  await slave.configureSpi(slave.spiConfig("slave", mode, order, hz));
}

async function spiEvents(slave: Board, sent: number[][], received: number[][], mcp = false): Promise<void> {
  const observed = await slave.events(mcp);
  equal(observed.length, sent.length, "SPI slave event count");
  for (const [index, event] of observed.entries()) {
    equal(event.receive, sent[index], "SPI slave MOSI bytes");
    equal(event.send, received[index], "SPI slave MISO bytes");
    assert(event.startTime > 0 && event.startTime <= event.endTime, "SPI event timestamps");
  }
}

async function spiMatrix(): Promise<void> {
  const lengths = [1, 2, 3, 4, 7, 16, 31, 32, 63, 64, 65, 127, 128, 129, 254, 255];
  const frequencies = JSON.parse(process.env.TEST_SPI_HZ ?? "[100000,1000000,5000000,10000000]") as number[];
  assert(Array.isArray(frequencies) && frequencies.length > 0
    && frequencies.every((hz) => Number.isInteger(hz) && hz >= 1 && hz <= 10000000), "TEST_SPI_HZ");
  report.settings.spiHz = frequencies;
  for (let round = 0; round < rounds; round++) {
    const pair = round % 2 ? boards.toReversed() : boards;
    for (const [master, slave] of [pair, pair.toReversed()]) {
      for (const mode of (round % 2 ? [3, 2, 1, 0] : [0, 1, 2, 3])) {
        for (const order of ["msb", "lsb"]) {
          for (const hz of frequencies) {
            await spiSetup(master!, slave!, mode, order, hz);
            const reply = bytes(255);
            await slave!.mapping([{ type: "prefix", mosi: [], miso: reply }]);
            const sent = [[0xA5]];
            const received = [await master!.transfer(sent[0]!)];
            equal(received[0], [0], "SPI initial response");
            const drain = async () => {
              await spiEvents(slave!, sent, received);
              sent.length = 0;
              received.length = 0;
            };
            if (sent.length >= SPI_DRAIN_BATCH) await drain();
            for (const length of lengths) {
              const data = bytes(length);
              const rx = await master!.transfer(data);
              sent.push(data);
              received.push(rx);
              const label = "SPI " + master!.name + "->" + slave!.name + " round " + (round + 1)
                + " mode " + mode + " " + order + " " + hz + "Hz/" + length;
              equal(rx, reply.slice(0, length), label);
              report.spiPayloads = (report.spiPayloads ?? 0) + 1;
              if (sent.length >= SPI_DRAIN_BATCH) await drain();
            }
            if (sent.length > 0) await drain();
          }
        }
        log("SPI round " + (round + 1) + " " + master!.name + "->" + slave!.name + " mode " + mode + " passed");
      }
    }
  }
}

async function spiFeatures(): Promise<void> {
  for (const [master, slave] of [boards, boards.toReversed()]) {
    await spiSetup(master!, slave!);
    const rules = [
      { type: "prefix", mosi: [], miso: [9, 8] },
      { type: "prefix", mosi: [0xA1], miso: [1, 2, 3] },
      { type: "prefix", mosi: [0xA1, 0xB2], miso: [4, 5, 6, 7] },
      { type: "exact", mosi: [0xA1, 0xB2, 0xC3], miso: [10, 20, 30, 40] },
    ];
    await slave!.mapping(rules, true);
    for (const [command, response] of [
      [[0xA1, 0xB2, 0xC3], [10, 20, 30, 40, 0, 0]],
      [[0xA1, 0xB2, 0x99], [4, 5, 6, 7, 0, 0]],
      [[0xA1, 0], [1, 2, 3, 0, 0, 0]],
      [[0x77], [9, 8, 0, 0, 0, 0]],
    ]) {
      const received = [await master!.transfer(command!), await master!.transfer([0, 0, 0, 0, 0, 0])];
      equal(received[1], response, "SPI exact/prefix priority and padding");
      await spiEvents(slave!, [command!, [0, 0, 0, 0, 0, 0]], received, true);
    }
    for (const type of ["exact", "prefix"]) {
      const command = bytes(255);
      const reply = bytes(255);
      await slave!.mapping([{ type, mosi: command, miso: reply }], true);
      await master!.transfer(command, true);
      await slave!.events();
      const received = await master!.transfer(bytes(255), true);
      equal(received, reply, "255-byte " + type + " match and response");
      const events = await slave!.events(true);
      equal(events.length, 1, "MCP full-length slave event count");
      equal(events[0]!.send, reply, "MCP full-length slave event");
    }
    await master!.rest("POST", "/spi/0/master/transceive", { data: bytes(256) }, 400);
    for (const field of ["mosi", "miso"]) {
      await slave!.rest("POST", "/spi/0/slave/transmit", {
        mapping: [{ type: "exact", mosi: [1], miso: [1], [field]: bytes(256) }],
      }, 400);
    }
    await slave!.rest("POST", "/spi/0/master/transceive", { data: [1] }, 422);
    await master!.rest("GET", "/spi/0/slave/receive", undefined, 422);
    await slave!.mapping([]);
    await master!.transfer([1]);
    await slave!.events();
    equal(await master!.transfer([0, 0]), [0, 0], "Clear SPI response map");
    await slave!.events();
    equal(await master!.transfer([]), [], "Empty SPI master transfer");
    equal(await slave!.events(), [], "Empty transfer produced no event");
    const reply = bytes(255);
    await slave!.mapping([{ type: "prefix", mosi: [], miso: reply }]);
    await master!.transfer([1]);
    await slave!.events();
    const sent: number[][] = [];
    const received: number[][] = [];
    for (let index = 0; index < soak; index++) {
      const tx = bytes(255);
      const rx = await master!.transfer(tx);
      equal(rx, reply, "SPI 255-byte soak response");
      sent.push(tx);
      received.push(rx);
      if (sent.length === SPI_DRAIN_BATCH || index === soak - 1) {
        await spiEvents(slave!, sent, received);
        sent.length = 0;
        received.length = 0;
      }
    }
    await section("SPI scripts " + master!.name + "->" + slave!.name, async () => {
      const script = await master!.tool("run_script", {
        script: "return spi_master_transceive{id=0,data={" + bytes(255).join(",") + "}}",
        maxCalls: 1, timeout: 1000000,
      });
      equal(script.result.data, reply, "255-byte SPI through script");
      const decoded = await slave!.tool("run_script", {
        script: "local e=spi_slave_receive{id=0}.events return {count=#e,send=e[1].send}",
        maxCalls: 1, timeout: 1000000,
      });
      equal(decoded.result.count, 1, "SPI events decoded in script");
      equal(decoded.result.send, reply, "255-byte SPI event arrays decoded in script");
    }, false);
    const pins = master!.spi.pins;
    await master!.configureSpi({ ...master!.spi, pins: { ...pins, miso: null } });
    equal(await master!.transfer([1, 2, 3]), [], "SPI master without MISO");
    await slave!.events();
  }
}

async function traceAfterSpi(): Promise<void> {
  await releaseAll();
  for (const [source, sink] of [boards, boards.toReversed()]) {
    const output = source!.row[3]!;
    const input = sink!.row[3]!;
    await source!.pin(output, "pwmOutput");
    await source!.rest("POST", "/pin/" + output + "/pwm", { frequency: 500, duty: 50 }, 204);
    await sink!.pin(input, "digitalInput");
    const events = await sink!.trace(input, "both", 20000);
    assert(events.some((event) => event.edge === "raising") && events.some((event) => event.edge === "falling"),
      "GPIO trace works on the former slave CS pin " + sink!.name);
    await releaseAll();
  }
}

async function cleanup(): Promise<void> {
  for (const board of boards) {
    if (!board.touched || !board.snapshot) continue;
    const errors: string[] = [];
    const attempt = async (action: () => Promise<unknown>) => {
      try { await action(); } catch (error) { errors.push(String(error)); }
    };
    for (const id of board.locks) await attempt(() => board.tool("delete_lock", { id }));
    await attempt(() => board.release());
    await attempt(() => board.configureUart(bodyConfig(board.snapshot!.uart)));
    await attempt(() => board.configureSpi(bodyConfig(board.snapshot!.spi)));
    for (const state of board.snapshot.pins.filter((pin) => board.row.includes(pin.pin))) {
      const config = { mode: state.mode, openDrain: state.openDrain, pullUp: state.pullUp, pullDown: state.pullDown };
      await attempt(() => board.rest("PUT", "/pin/" + state.pin, config, 204));
    }
    for (const [rail, enable] of Object.entries(board.snapshot.power)) {
      await attempt(() => board.rest("POST", "/power/" + rail, { enable }, 204));
    }
    await attempt(async () => {
      const pins = (await board.rest("GET", "/pin/")).pins;
      for (const initial of board.snapshot!.pins.filter((pin) => board.row.includes(pin.pin))) {
        const actual = pins.find((pin: Json) => pin.pin === initial.pin);
        for (const key of ["mode", "openDrain", "pullUp", "pullDown"] as const) {
          equal(actual[key], initial[key], "Restore " + board.name + " pin " + initial.pin + " " + key);
        }
      }
      assert(!(await board.rest("GET", "/spi/")).spis[0].enable, "SPI disabled after cleanup");
      assert(!(await board.rest("GET", "/uart/")).uarts[0].enable, "UART disabled after cleanup");
      for (const [rail, enable] of Object.entries(board.snapshot!.power)) {
        equal((await board.rest("GET", "/power/" + rail)).enable, enable, "Restore output power");
      }
    });
    report.cleanup.push({ board: board.name, restored: errors.length === 0, errors });
    failures.push(...errors.map((error) => board.name + " cleanup: " + error));
  }
}

async function main(): Promise<void> {
  if (help) {
    console.log("TEST_BOARDS='[\"10.229.29.35\",\"10.229.29.248\"]' "
      + "TEST_PINS='[1,2,4,5]' bun tests/hardware/pair.ts");
    console.log("TEST_PINS='[[1,2,4,5],[0,3,5,6]]' maps corresponding columns between the two boards.");
    console.log("Connect those wires plus GND; leave power outputs unloaded. "
      + "Selected pins, UART and SPI must be idle.");
    console.log("First four columns: MISO, MOSI, clock, select. UART uses the first two columns.");
    console.log("Optional: TEST_ROUNDS=3 TEST_SOAK_ITERATIONS=32 TEST_TIMEOUT_MS=15000 TEST_SEED=20261101");
    console.log("Optional: TEST_SPI_HZ='[100000,1000000,5000000,10000000]' TEST_REPORT=/tmp/pair-results.json");
    console.log("Optional: TEST_SECTIONS selects section names by regex; metadata and wiring preflight always run.");
    console.log("Covers REST/MCP, mapped GPIO, PWM/trace/pulse, UART, SPI, power, "
      + "buzzer, scripts, locks and concurrency.");
    return;
  }
  const addresses = JSON.parse(process.env.TEST_BOARDS ?? "[]") as string[];
  const parsedPins = JSON.parse(process.env.TEST_PINS ?? "[1,2,4,5]") as number[] | number[][];
  assert(Array.isArray(addresses) && addresses.length === 2
    && addresses.every((address) => typeof address === "string") && addresses[0] !== addresses[1],
  "TEST_BOARDS must be a JSON array of two distinct board addresses");
  pinRows = Array.isArray(parsedPins[0])
    ? parsedPins as number[][] : [parsedPins as number[], parsedPins as number[]];
  assert(pinRows.length === 2 && pinRows[0]!.length === pinRows[1]!.length, "TEST_PINS must have two matching rows");
  for (const row of pinRows) {
    assert(Array.isArray(row) && row.length >= 4 && row.length <= 8 && new Set(row).size === row.length
      && row.every((pin) => Number.isInteger(pin) && pin >= 0 && pin <= 7),
    "TEST_PINS contains 4-8 unique pins per board");
  }
  assert(Number.isInteger(rounds) && rounds >= 1 && rounds <= 100, "TEST_ROUNDS must be 1-100");
  assert(Number.isInteger(soak) && soak >= 1 && soak <= 10000, "TEST_SOAK_ITERATIONS must be 1-10000");
  assert(Number.isFinite(timeout) && timeout > 0, "TEST_TIMEOUT_MS must be positive");
  assert(Number.isInteger(seed) && seed > 0 && seed <= 0xffffffff, "TEST_SEED must be 1-4294967295");
  boards = addresses.map((address, index) => new Board(address, pinRows[index]!));
  report.settings = { addresses, pinRows, rounds, soak, timeout, seed: randomState,
    sections: process.env.TEST_SECTIONS ?? "all" };
  await preflight();
  for (const board of boards) await section(board.name + " metadata and MCP", () => metadata(board));
  for (const board of boards) await section(board.name + " power, buzzer and scripts", () => powerBuzzerScripts(board));
  if (!await section("GPIO wiring matrix, both directions", wiring)) {
    throw new Error("Wiring validation failed; remaining electrical tests require the configured connections.");
  }
  await section("PWM, pulse and trace, both directions", pwmTracePulse);
  await section("UART framing, payloads, flush and concurrency", uart);
  for (const board of boards) {
    await section(board.name + " locks and concurrency", () => locksAndConcurrency(board));
  }
  await section("SPI modes, bit orders, clocks and 1-255-byte payloads", spiMatrix);
  await section("SPI maps, boundaries, MCP, scripts and soak", spiFeatures);
  await section("GPIO trace after releasing SPI", traceAfterSpi);
}

try {
  await main();
} catch (error) {
  failures.push(error instanceof Error ? error.message : String(error));
} finally {
  stopping = false;
  await cleanup();
  report.finished = new Date().toISOString();
  report.summary = { passed: checks.filter((check) => check.passed).length,
    failed: checks.filter((check) => !check.passed).length, failures: failures.length };
  if (reportPath && !help) await Bun.write(reportPath, JSON.stringify(report, null, 2) + "\n");
}
if (failures.length) {
  for (const failure of failures) console.error("[hardware-pair] FAIL " + failure);
  process.exitCode = 1;
} else if (!help) {
  log("PASS: " + (testFilter ? "selected" : "all paired") + " sections and cleanup; "
    + report.summary.passed + " checks, "
    + (report.spiPayloads ?? 0) + " SPI matrix payloads");
}
