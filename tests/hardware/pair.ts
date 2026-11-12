import { URL } from "node:url";

type Json = Record<string, any>;
type PinState = { pin: number; mode: string; openDrain: boolean; pullUp: boolean; pullDown: boolean };
type TransferEvent = { receive: number[]; send: number[]; startTime: number; endTime: number };
type TraceEvent = { pin: number; edge: string; level: number; time: number };
type Snapshot = { pins: PinState[]; uart: Json; spi: Json; i2c: Json };
type Result = { status: number; body: any; text: string };
type Check = { name: string; passed: boolean; detail?: string };

// Single-board API, resource, script and concurrency checks belong in smoke.ts.
// TEST_PINS is either one pin-to-pin row, or two board rows with corresponding columns connected.
const help = process.argv.includes("--help");
const timeout = Number(process.env.TEST_TIMEOUT_MS ?? 15000);
const rounds = Number(process.env.TEST_ROUNDS ?? 1);
const soak = Number(process.env.TEST_SOAK_ITERATIONS ?? 8);
// Drain regularly during payload tests.
const SPI_DRAIN_BATCH = 3;
const I2C_MATRIX_SECTION = "I2C probes, clocks and 1-255-byte write/read payloads";
const I2C_FEATURES_SECTION = "I2C mapped responses, NACK recovery, MCP and soak";
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
let sectionDepth = 0;
process.on("SIGINT", () => { stopping = true; });
process.on("SIGTERM", () => { stopping = true; });

function assert(ok: unknown, name: string, detail?: string): asserts ok {
  checks.push({ name, passed: Boolean(ok), ...(detail ? { detail } : {}) });
  if (!ok) throw new Error(name + (detail ? ": " + detail : ""));
}

function equal(actual: unknown, expected: unknown, name: string): void {
  const matches = JSON.stringify(actual) === JSON.stringify(expected);
  assert(matches, name, matches ? undefined : JSON.stringify({ expected, actual }).slice(0, 1400));
}

function bytes(length: number): number[] {
  return Array.from({ length }, () => {
    randomState ^= randomState << 13;
    randomState ^= randomState >>> 17;
    randomState ^= randomState << 5;
    return randomState >>> 24;
  });
}

function toHex(data: number[]): string {
  return data.map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

function fromHex(hex: string): number[] {
  const out: number[] = [];
  for (let index = 0; index < hex.length; index += 2) out.push(Number.parseInt(hex.slice(index, index + 2), 16));
  return out;
}

function wireData(data: number[] | string): string {
  return typeof data === "string" ? data : toHex(data);
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
    encoding: "byte", pins: { rx: null, tx: null, rts: null, dtr: null } };
  spi: Json;
  i2c: Json;
  touched = false;

  constructor(address: string, row: number[]) {
    this.url = new URL(address.includes("://") ? address : "http://" + address);
    this.row = row;
    this.spi = this.spiConfig("master", 0, "msb", 1000000, false);
    this.i2c = this.i2cConfig("master", 100000, 0x42, false);
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
    let status: number;
    let text: string;
    try {
      // Use pooled connections and bypass proxy settings for direct board requests.
      const response = await fetch(url, { method, headers, body: serialized, proxy: "",
        signal: AbortSignal.timeout(timeout) });
      status = response.status;
      text = await response.text();
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      report.requests.push({ board: this.name, method, path, transportError: message });
      throw new Error(this.name + " " + method + " " + path + ": " + message);
    }
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
    if (mcp) await this.configurePins([pin], mode, extra);
    else await this.rest("PUT", "/pin/" + pin, config, 204);
  }

  async configurePins(pins: number[], mode: string, extra: Json = {}): Promise<void> {
    await this.tool("configure_pins", { pins, mode, openDrain: false, pullUp: false, pullDown: false, ...extra });
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

  i2cConfig(role: string, hz = 100000, address = 0x42, enable = true): Json {
    return { enable, role, pullUp: true, addressBits: 7,
      ...(role === "master" ? { clockHz: hz, sclWaitUs: 10000 } : { address }),
      pins: { sda: this.row[0], scl: this.row[1] } };
  }

  async configureI2c(config: Json, mcp = false): Promise<void> {
    if (mcp) await this.tool("configure_i2c", { id: 0, ...config });
    else await this.rest("POST", "/i2c/0/config", config, 204);
    this.i2c = config;
  }

  async release(): Promise<void> {
    if (this.i2c.enable) await this.configureI2c({ ...this.i2c, enable: false });
    if (this.spi.enable) await this.configureSpi({ ...this.spi, enable: false });
    if (this.uart.enable) await this.configureUart({ ...this.uart, enable: false });
    await this.configurePins(this.row, "disable");
  }

  async transfer(data: number[], mcp = false): Promise<number[]> {
    const payload = toHex(data);
    const result = mcp ? await this.tool("spi_master_transceive", { id: 0, data: payload })
      : await this.rest("POST", "/spi/0/master/transceive", { data: payload });
    return fromHex(result.data);
  }

  async events(mcp = false): Promise<TransferEvent[]> {
    const result = mcp ? await this.tool("spi_slave_receive", { id: 0 })
      : await this.rest("GET", "/spi/0/slave/receive");
    return (result.events as Json[]).map((event) => ({
      startTime: event.startTime, endTime: event.endTime, receive: fromHex(String(event.receive ?? "")),
      send: fromHex(String(event.send ?? "")),
    }));
  }

  async mapping(mapping: Json[], mcp = false): Promise<void> {
    const encoded = mapping.map((entry) => ({
      ...entry, mosi: Array.isArray(entry.mosi) ? toHex(entry.mosi) : entry.mosi,
      miso: Array.isArray(entry.miso) ? toHex(entry.miso) : entry.miso,
    }));
    if (mcp) await this.tool("spi_slave_transmit", { id: 0, mapping: encoded });
    else await this.rest("POST", "/spi/0/slave/transmit", { mapping: encoded }, 204);
  }

  async i2cTransfer(write: number[], readLen = 0, mcp = false, address = 0x42): Promise<number[]> {
    const body = { address, write: toHex(write), readLen, timeoutMs: 1000 };
    let result: any;
    try {
      result = mcp ? await this.tool("i2c_master_transceive", { id: 0, ...body })
        : await this.rest("POST", "/i2c/0/master/transceive", body);
    } catch (error) {
      throw new Error("I2C " + this.name + " " + this.i2c.clockHz + "Hz address " + address
        + " write=" + write.length + " read=" + readLen + " via " + (mcp ? "MCP" : "REST") + ": " + error);
    }
    assert(typeof result.data === "string" && /^([0-9a-fA-F]{2})*$/.test(result.data), "I2C response hex bytes");
    const data = fromHex(result.data);
    equal(data.length, readLen, "I2C response length");
    report.i2cTransfers = (report.i2cTransfers ?? 0) + 1;
    return data;
  }

  async i2cEvents(mcp = false): Promise<TransferEvent[]> {
    const result = mcp ? await this.tool("i2c_slave_receive", { id: 0 })
      : await this.rest("GET", "/i2c/0/slave/receive");
    assert(Array.isArray(result.events), "I2C slave event array");
    return (result.events as Json[]).map((event) => {
      for (const key of ["receive", "send"]) {
        assert(typeof event[key] === "string" && /^([0-9a-fA-F]{2})*$/.test(event[key]), "I2C event hex " + key);
      }
      return { startTime: event.startTime, endTime: event.endTime,
        receive: fromHex(event.receive), send: fromHex(event.send) };
    });
  }

  async i2cMapping(mapping: Json[], mcp = false): Promise<void> {
    const encoded = mapping.map((entry) => ({
      ...entry, receive: wireData(entry.receive), send: wireData(entry.send),
    }));
    if (mcp) await this.tool("i2c_slave_transmit", { id: 0, mapping: encoded });
    else await this.rest("POST", "/i2c/0/slave/transmit", { mapping: encoded }, 204);
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

async function section(name: string, action: () => Promise<void>, release = true, minPins = 2): Promise<boolean> {
  const preflight = name === "GPIO wiring matrix, both directions";
  // Selecting a section also selects its nested cases, including with an anchored regex.
  if (testFilter && sectionDepth === 0 && !preflight && !testFilter.test(name)) {
    report.skipped ??= [];
    report.skipped.push(name);
    return true;
  }
  if (pinRows.some((row) => row.length < minPins)) {
    report.skipped ??= [];
    report.skipped.push(name);
    log("SKIP " + name + ": requires " + minPins + " connected pins per board");
    return true;
  }
  log(name);
  let passed = true;
  sectionDepth++;
  try { await action(); } catch (error) {
    failures.push(name + ": " + (error instanceof Error ? error.message : String(error)));
    log("FAIL " + failures.at(-1));
    passed = false;
  } finally {
    sectionDepth--;
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
    const i2c = (await board.rest("GET", "/i2c/")).i2cs[0];
    assert(!uart.enable && !spi.enable && !i2c.enable, board.name + " UART, SPI and I2C must initially be disabled");
    for (const pin of board.row) {
      assert(pins.find((state) => state.pin === pin)?.mode === "disable",
        board.name + " pin " + pin + " must initially be disabled");
    }
    board.snapshot = { pins, uart, spi, i2c };
    report.boards.at(-1).snapshot = board.snapshot;
  }
  for (const board of boards) board.touched = true;
}

async function wiring(): Promise<void> {
  for (const [source, sink] of [boards, boards.toReversed()]) {
    await releaseAll();
    await sink!.configurePins(sink!.row, "digitalInput", { pullDown: true });
    for (let column = 0; column < source!.row.length; column++) {
      const output = source!.row[column]!;
      const modes = column === 0 ? ["digitalOutput", "digitalInputOutput", "openDrain"] : ["digitalOutput"];
      for (const mode of modes) {
        const mcp = mode !== "digitalOutput";
        const openDrain = mode === "openDrain";
        if (openDrain) await sink!.pin(sink!.row[column]!, "digitalInput", mcp, { pullUp: true });
        await source!.pin(output, mode === "openDrain" ? "digitalOutput" : mode, mcp, { openDrain });
        for (const level of [0, 1]) {
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
      const frequencies = column === 0 ? [500, 2000, 10000] : [2000];
      for (const [index, hz] of frequencies.entries()) {
        const duty = [10, 25, 50, 90][index]!;
        if (index % 2) await source!.tool("set_pin_pwms", { pins: [output], frequency: hz, duty });
        else await source!.rest("POST", "/pin/" + output + "/pwm", { frequency: hz, duty }, 204);
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
      for (const duty of column === 0 ? [0, 100] : []) {
        await source!.rest("POST", "/pin/" + output + "/pwm", { frequency: 1000, duty }, 204);
        equal((await sink!.levels())[column], duty === 0 ? 0 : 1, "PWM constant endpoint");
      }
      await source!.pin(output, "disable");
      await sink!.pin(input, "disable");
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
      encoding, ...condition, pins: { tx: board.row[index], rx: board.row[1 - index], rts: null, dtr: null } }, mcp);
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
    received = typeof received === "string" ? received + result.data : [...received, ...fromHex(result.data)];
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
      await source!.tool("uart_transmit", { id: 0, data: wireData(data) });
      await uartReceive(sink!, data, false);
    }
  }
  for (const encoding of ["byte", "utf8"]) {
    await uartPair(encoding);
    for (const [source, sink] of [boards, boards.toReversed()]) {
      const data = encoding === "byte" ? bytes(256) : "paired UART 你好 π ".repeat(5);
      await source!.tool("uart_transmit", { id: 0, data: wireData(data) });
      await uartReceive(sink!, data, false);
      await source!.rest("POST", "/uart/0/transmit", { data: wireData(data) }, 204);
      await Bun.sleep(30);
      await sink!.tool("uart_flush", { id: 0 });
      equal((await sink!.rest("GET", "/uart/0/receive")).data, "", "UART flush");
    }
  }
  // Keep maximum-size requests isolated so a failure does not hide the other UART checks.
  for (const encoding of ["byte", "utf8"]) {
    for (const [source, sink] of [boards, boards.toReversed()]) {
      await section("UART maximum " + encoding + " payload " + source!.name, async () => {
        await uartPair(encoding);
        const data = encoding === "byte" ? bytes(2048) : "x".repeat(2048);
        await source!.tool("uart_transmit", { id: 0, data: wireData(data) });
        await uartReceive(sink!, data, false);
      });
    }
  }
  await uartPair();
  const source = boards[0]!;
  const sink = boards[1]!;
  const chunks = [[1, 2, 3, 4], [11, 12, 13, 14], [21, 22, 23, 24], [31, 32, 33, 34]];
  await Promise.all(chunks.map((data) => source.tool("uart_transmit", { id: 0, data: toHex(data) })));
  const all: number[] = [];
  const end = Date.now() + 3000;
  while (all.length < 16 && Date.now() < end) {
    all.push(...fromHex((await sink.rest("GET", "/uart/0/receive")).data));
    if (all.length < 16) await Bun.sleep(10);
  }
  const observed = Array.from({ length: 4 }, (_, index) => JSON.stringify(all.slice(index * 4, index * 4 + 4)));
  equal(observed.sort(), chunks.map((chunk) => JSON.stringify(chunk)).sort(), "Concurrent UART intact chunks");
}

async function uartControlLines(): Promise<void> {
  const source = boards[0]!;
  const sink = boards[1]!;
  for (const line of ["rts", "dtr"] as const) {
    await releaseAll();
    // Reuse the first wire for the control line and the second source pin for RX.
    await sink.pin(sink.row[0]!, "digitalInput", false, { pullDown: true });
    await source.configureUart({ enable: true, baudRate: 115200, dataBits: 8, parity: "none", stopBits: 1,
      encoding: "byte", pins: { rx: source.row[1], tx: null, rts: null, dtr: null, [line]: source.row[0] } });
    equal((await sink.levels())[0], 1, "UART " + line.toUpperCase() + " starts released (high)");
    await source.tool("uart_set_" + line, { id: 0, active: true });
    equal((await sink.levels(true))[0], 0, "UART " + line.toUpperCase() + " asserts low");
  }
}

async function i2cSetup(master: Board, slave: Board, hz = 100000, mcp = false, address = 0x42): Promise<void> {
  await releaseAll();
  await slave.configureI2c(slave.i2cConfig("slave", hz, address), mcp);
  await master.configureI2c(master.i2cConfig("master", hz), mcp);
  for (const board of [master, slave]) {
    const inventory = mcp ? await board.tool("list_i2cs") : await board.rest("GET", "/i2c/");
    equal(bodyConfig(inventory.i2cs[0]), board.i2c, board.name + " I2C configuration round-trip");
  }
}

async function i2cEvents(slave: Board, receive: number[], send: number[], mcp = false): Promise<void> {
  const observed: TransferEvent[] = [];
  const deadline = Date.now() + 2000;
  do {
    observed.push(...await slave.i2cEvents(mcp));
    if (observed.length > 0) break;
    await Bun.sleep(10);
  } while (Date.now() < deadline);
  equal(observed.length, 1, "I2C slave event count");
  const event = observed[0]!;
  equal(event.receive, receive, "I2C slave received exact bytes");
  equal(event.send, send, "I2C slave sent exact bytes");
  assert(event.startTime > 0 && event.startTime <= event.endTime, "I2C event timestamps");
  equal(await slave.i2cEvents(!mcp), [], "I2C events drained exactly once");
}

async function i2cMatrix(): Promise<void> {
  const frequencies = JSON.parse(process.env.TEST_I2C_HZ ?? "[10000,100000]") as number[];
  assert(Array.isArray(frequencies) && frequencies.length > 0
    && frequencies.every((hz) => Number.isInteger(hz) && hz >= 10000 && hz <= 1000000), "TEST_I2C_HZ");
  report.settings.i2cHz = frequencies;
  for (let round = 0; round < rounds; round++) {
    const pair = round % 2 ? boards.toReversed() : boards;
    for (const [master, slave] of [pair, pair.toReversed()]) {
      for (const [frequencyIndex, hz] of frequencies.entries()) {
        const label = master!.name + "->" + slave!.name + " round " + (round + 1) + " " + hz + "Hz";
        await section(I2C_MATRIX_SECTION + " " + label,
          () => i2cPayloads(master!, slave!, hz, (round + frequencyIndex) % 2 === 1, label));
      }
    }
  }
}

async function i2cPayloads(master: Board, slave: Board, hz: number, mcpConfig: boolean, label: string): Promise<void> {
  const lengths = [1, 2, 3, 4, 7, 16, 31, 32, 63, 64, 65, 127, 128, 129, 254, 255];
  await i2cSetup(master, slave, hz, mcpConfig);
  for (const mcp of [false, true]) {
    equal(await master.i2cTransfer([], 0, mcp), [], "I2C address probe ACK");
  }
  await slave.i2cEvents();
  for (const [index, length] of lengths.entries()) {
    const mcp = index % 2 === 1;
    const command = bytes(length);
    const reply = bytes(length);
    await slave.i2cMapping([], mcp);
    equal(await master.i2cTransfer(command, 0, mcp), [], "I2C " + label + "/" + length + " write-only");
    await i2cEvents(slave, command, [], !mcp);
    await slave.i2cMapping([{ type: "prefix", receive: [], send: reply }], !mcp);
    equal(await master.i2cTransfer([], length, !mcp), reply, "I2C " + label + "/" + length + " read-only");
    await i2cEvents(slave, [], reply, mcp);
    equal(await master.i2cTransfer(command, length, mcp), reply,
      "I2C " + label + "/" + length + " repeated-start write/read");
    await i2cEvents(slave, command, reply, !mcp);
    report.i2cPayloads = (report.i2cPayloads ?? 0) + 3;
  }
  log("I2C " + label + " passed");
}

async function i2cFeatures(): Promise<void> {
  for (const [master, slave] of [boards, boards.toReversed()]) {
    await section(I2C_FEATURES_SECTION + " " + master!.name + "->" + slave!.name,
      () => i2cMappedResponses(master!, slave!));
  }
}

async function i2cMappedResponses(master: Board, slave: Board): Promise<void> {
  const address = 0x53;
  await i2cSetup(master, slave, 100000, true, address);
  for (const mcp of [false, true]) {
    const body = { address: address + 1, write: "", readLen: 0, timeoutMs: 1000 };
    if (mcp) {
      const result = await master.rpc("tools/call", {
        name: "i2c_master_transceive", arguments: { id: 0, ...body },
      });
      assert(result.isError === true, "I2C missing address reports MCP error");
      const error = JSON.parse(result.content[0].text);
      assert(typeof error.reason === "string" && error.reason.length > 0, "I2C MCP NACK reason");
    } else {
      const error = await master.rest("POST", "/i2c/0/master/transceive", body, 422);
      assert(typeof error.reason === "string" && error.reason.length > 0, "I2C REST NACK reason");
    }
    equal(await master.i2cTransfer([], 0, mcp, address), [], "I2C recovers after missing address");
  }
  await slave.i2cEvents();
  const rules = [
    { type: "prefix", receive: [], send: [9, 8, 7, 6] },
    { type: "prefix", receive: [0xA1], send: [1, 2, 3, 4] },
    { type: "prefix", receive: [0xA1, 0xB2], send: [4, 5, 6, 7] },
    { type: "exact", receive: [0xA1, 0xB2, 0xC3], send: [10, 20, 30, 40] },
  ];
  for (const mcp of [false, true]) {
    await slave.i2cMapping(rules, mcp);
    for (const [command, reply] of [
      [[0xA1, 0xB2, 0xC3], [10, 20, 30, 40]],
      [[0xA1, 0xB2, 0x99], [4, 5, 6, 7]],
      [[0xA1, 0], [1, 2, 3, 4]],
      [[0x77], [9, 8, 7, 6]],
    ]) {
      equal(await master.i2cTransfer(command!, reply!.length, !mcp, address), reply,
        "I2C repeated-start exact/longest-prefix priority");
      await i2cEvents(slave, command!, reply!, mcp);
    }
    const command = bytes(255);
    const reply = bytes(255);
    await slave.i2cMapping([{ type: "exact", receive: command, send: reply }], mcp);
    equal(await master.i2cTransfer(command, 0, mcp, address), [], "I2C separate write");
    equal(await master.i2cTransfer([], reply.length, !mcp, address), reply, "I2C separate read and 255-byte map");
    await i2cEvents(slave, command, reply, !mcp);
    await slave.i2cMapping([], mcp);
    equal(await master.i2cTransfer([0x77], 0, !mcp, address), [], "I2C cleared map accepts writes");
    await i2cEvents(slave, [0x77], [], mcp);
    equal(await master.i2cTransfer([], 1, mcp, address), [0], "I2C cleared map returns zero");
    await i2cEvents(slave, [], [], !mcp);
  }
  const reply = bytes(255);
  await slave.i2cMapping([{ type: "prefix", receive: [], send: reply }]);
  for (let index = 0; index < soak; index++) {
    const command = bytes(255);
    equal(await master.i2cTransfer(command, reply.length, index % 2 === 1, address), reply,
      "I2C 255-byte soak response");
    await i2cEvents(slave, command, reply, index % 2 === 0);
  }
}

async function spiSetup(master: Board, slave: Board, mode = 0, order = "msb", hz = 1000000): Promise<void> {
  for (const board of boards) {
    const role = board === master ? "master" : "slave";
    if (board.spi.enable && board.spi.role !== role) await board.configureSpi({ ...board.spi, enable: false });
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
  const boundaryLengths = [1, 2, 3, 4, 7, 16, 31, 32, 63, 64, 65, 127, 128, 129, 254, 255];
  const frequencies = JSON.parse(process.env.TEST_SPI_HZ ?? "[100000,1000000,5000000,10000000]") as number[];
  assert(Array.isArray(frequencies) && frequencies.length > 0
    && frequencies.every((hz) => Number.isInteger(hz) && hz >= 1 && hz <= 10000000), "TEST_SPI_HZ");
  report.settings.spiHz = frequencies;
  const boundaryHz = frequencies.includes(1000000) ? 1000000 : frequencies[0];
  for (let round = 0; round < rounds; round++) {
    const pair = round % 2 ? boards.toReversed() : boards;
    for (const [master, slave] of [pair, pair.toReversed()]) {
      for (const mode of (round % 2 ? [3, 2, 1, 0] : [0, 1, 2, 3])) {
        for (const order of ["msb", "lsb"]) {
          for (const hz of frequencies) {
            // Keep 128 -> 129 consecutive; sweep other lengths once per direction per round.
            const lengths = mode === 0 && order === "msb" && hz === boundaryHz
              ? boundaryLengths : [128, 129, 1, 255];
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
    await slave!.mapping([]);
    await master!.transfer([1]);
    await slave!.events();
    equal(await master!.transfer([0, 0]), [0, 0], "Clear SPI response map");
    await slave!.events();
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
    await attempt(() => board.release());
    await attempt(() => board.configureUart(bodyConfig(board.snapshot!.uart)));
    await attempt(() => board.configureSpi(bodyConfig(board.snapshot!.spi)));
    await attempt(() => board.configureI2c(bodyConfig(board.snapshot!.i2c)));
    for (const state of board.snapshot.pins.filter((pin) => board.row.includes(pin.pin))) {
      const config = { mode: state.mode, openDrain: state.openDrain, pullUp: state.pullUp, pullDown: state.pullDown };
      await attempt(() => board.rest("PUT", "/pin/" + state.pin, config, 204));
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
      equal(bodyConfig((await board.rest("GET", "/i2c/")).i2cs[0]), bodyConfig(board.snapshot!.i2c),
        "I2C configuration restored after cleanup");
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
    console.log("Connect those wires plus GND. "
      + "Selected pins, UART, SPI and I2C must be idle.");
    console.log("First four columns: SPI MISO, MOSI, clock, select. UART uses the first two columns.");
    console.log("UART RTS/DTR reuse the first wire one at a time; the second source pin is RX. No extra wires needed.");
    console.log("I2C uses the first two columns as SDA, SCL, with pull-ups enabled on both boards.");
    console.log("Two-pin TEST_PINS='[1,2]' is supported; sections requiring four pins are skipped.");
    console.log("Optional: TEST_ROUNDS=1 TEST_SOAK_ITERATIONS=8 TEST_TIMEOUT_MS=15000 TEST_SEED=20261101");
    console.log("Optional: TEST_SPI_HZ='[100000,1000000,5000000,10000000]' TEST_REPORT=/tmp/pair-results.json");
    console.log("Optional: TEST_I2C_HZ='[10000,100000]' TEST_SECTIONS=I2C selects I2C tests plus wiring preflight.");
    console.log("Optional: TEST_SECTIONS selects section names by regex; wiring preflight always runs.");
    console.log("All SPI modes/orders/clocks use 1, 128, 129 and 255 bytes; "
      + "other lengths run once per direction/round.");
    console.log("Covers cross-board GPIO, PWM/trace/pulse, UART, SPI and I2C over REST/MCP. Run smoke.ts first.");
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
    assert(Array.isArray(row) && row.length >= 2 && row.length <= 8 && new Set(row).size === row.length
      && row.every((pin) => Number.isInteger(pin) && pin >= 0 && pin <= 7),
    "TEST_PINS contains 2-8 unique pins per board");
  }
  assert(Number.isInteger(rounds) && rounds >= 1 && rounds <= 100, "TEST_ROUNDS must be 1-100");
  assert(Number.isInteger(soak) && soak >= 1 && soak <= 10000, "TEST_SOAK_ITERATIONS must be 1-10000");
  assert(Number.isFinite(timeout) && timeout > 0, "TEST_TIMEOUT_MS must be positive");
  assert(Number.isInteger(seed) && seed > 0 && seed <= 0xffffffff, "TEST_SEED must be 1-4294967295");
  boards = addresses.map((address, index) => new Board(address, pinRows[index]!));
  report.settings = { addresses, pinRows, rounds, soak, timeout, seed: randomState,
    sections: process.env.TEST_SECTIONS ?? "all" };
  await preflight();
  if (!await section("GPIO wiring matrix, both directions", wiring)) {
    throw new Error("Wiring validation failed; remaining electrical tests require the configured connections.");
  }
  await section("PWM, pulse and trace, both directions", pwmTracePulse);
  await section("UART framing, payloads, flush and concurrency", uart);
  await section("UART RTS/DTR active-low control", uartControlLines);
  await section(I2C_MATRIX_SECTION, i2cMatrix);
  await section(I2C_FEATURES_SECTION, i2cFeatures);
  await section("SPI modes, bit orders, clocks and 1-255-byte payloads", spiMatrix, true, 4);
  await section("SPI mapped responses, MCP and soak", spiFeatures, true, 4);
  await section("GPIO trace after releasing SPI", traceAfterSpi, true, 4);
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
    + (report.spiPayloads ?? 0) + " SPI matrix payloads, " + (report.i2cTransfers ?? 0) + " I2C transfers");
}
