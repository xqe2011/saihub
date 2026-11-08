/**
 * Flash the binary SWD fixture and exercise DAP, OpenOCD, GDB and physical IO.
 * Requires exclusive access to Saihub's DAP port and replaces the target application with the checked-in fixture.
 */
import { createHash } from "node:crypto";
import { once } from "node:events";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { createConnection, createServer, type Socket } from "node:net";
import { homedir, tmpdir } from "node:os";
import { join } from "node:path";
import { parseArgs } from "node:util";

const FIXTURE = new URL("../fixtures/swd/", import.meta.url);
type Manifest = {
  file: string; size: number; sha256: string; flashAddress: number; dpidr: number; cpuid: number;
  magic: number; fixtureVersion: number; systemClockHz: number; defaultPeriodMs: number; scratchWords: number;
  symbols: Record<string, number>;
};

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) throw new Error(message);
}

function bytes(...values: number[]): Buffer {
  return Buffer.from(values);
}

function words(values: number[]): Buffer {
  const data = Buffer.alloc(values.length * 4);
  values.forEach((value, index) => data.writeUInt32LE(value >>> 0, index * 4));
  return data;
}

function readWords(data: Buffer): number[] {
  assert(data.length % 4 === 0, "Invalid word response length");
  return Array.from({ length: data.length / 4 }, (_, index) => data.readUInt32LE(index * 4));
}

function equal(actual: Buffer, expected: Buffer, message: string): void {
  assert(actual.equals(expected), `${message}: ${actual.toString("hex")}`);
}

class TcpConnection {
  private buffered: Buffer = Buffer.alloc(0);
  private failure?: Error;
  private changed?: () => void;

  private constructor(private readonly socket: Socket, private readonly timeoutMs: number) {
    socket.on("data", (chunk: Buffer) => {
      this.buffered = Buffer.concat([this.buffered, chunk]);
      this.changed?.();
    });
    const fail = (error: Error): void => {
      this.failure ??= error;
      this.changed?.();
    };
    socket.on("error", fail);
    socket.on("end", () => fail(new Error("TCP connection closed; stop other DAP clients before testing")));
    socket.on("close", () => fail(new Error("TCP connection closed; stop other DAP clients before testing")));
    socket.setNoDelay(true);
  }

  static async open(host: string, port: number, timeoutMs: number): Promise<TcpConnection> {
    const socket = createConnection({ host, port });
    const timeout = setTimeout(() => socket.destroy(new Error("TCP connection timed out")), timeoutMs);
    try {
      await once(socket, "connect");
      return new TcpConnection(socket, timeoutMs);
    } catch (error) {
      socket.destroy();
      throw error;
    } finally {
      clearTimeout(timeout);
    }
  }

  send(data: Uint8Array): void {
    assert(!this.socket.destroyed, "TCP connection closed; stop other DAP clients before testing");
    this.socket.write(data);
  }

  private async fill(ready: () => boolean): Promise<void> {
    const timeout = setTimeout(() => this.socket.destroy(new Error("TCP response timed out")), this.timeoutMs);
    try {
      while (!ready()) {
        if (this.failure) throw this.failure;
        await new Promise<void>((resolve) => { this.changed = resolve; });
      }
    } finally {
      this.changed = undefined;
      clearTimeout(timeout);
    }
  }

  async receive(size: number): Promise<Buffer> {
    await this.fill(() => this.buffered.length >= size);
    const data = this.buffered.subarray(0, size);
    this.buffered = this.buffered.subarray(size);
    return data;
  }

  async until(delimiter: number): Promise<Buffer> {
    await this.fill(() => this.buffered.includes(delimiter));
    const end = this.buffered.indexOf(delimiter);
    const data = this.buffered.subarray(0, end);
    this.buffered = this.buffered.subarray(end + 1);
    return data;
  }

  get closed(): boolean {
    return this.socket.destroyed;
  }

  destroy(): void {
    this.socket.destroy();
  }
}

export class Dap {
  private constructor(private readonly connection: TcpConnection) {}

  static async open(host: string, port: number, timeoutMs = 5000): Promise<Dap> {
    return new Dap(await TcpConnection.open(host, port, timeoutMs));
  }

  static frame(data: Uint8Array): Buffer {
    const header = Buffer.alloc(8);
    header.writeUInt32LE(0x00504144);
    header.writeUInt16LE(data.length, 4);
    header[6] = 1;
    return Buffer.concat([header, data]);
  }

  send(data: Uint8Array): void {
    this.connection.send(data);
  }

  async response(command: number): Promise<Buffer> {
    const header = await this.connection.receive(8);
    assert(header.readUInt32LE(0) === 0x00504144 && header[6] === 2 && header[7] === 0,
      "Invalid TCP response header");
    const size = header.readUInt16LE(4);
    assert(size >= 1 && size <= 1024, "Invalid TCP response size");
    const result = await this.connection.receive(size);
    assert(result[0] === command, "Response command mismatch");
    return result;
  }

  async command(data: Uint8Array): Promise<Buffer> {
    assert(data.length > 0, "Empty DAP command");
    this.send(Dap.frame(data));
    return this.response(data[0]!);
  }

  async transfer(request: number, value?: number): Promise<number> {
    const data = Buffer.concat([bytes(5, 0, 1, request), value === undefined ? bytes() : words([value])]);
    const response = await this.command(data);
    equal(response.subarray(1, 3), bytes(1, 1), "Register transfer failed");
    assert(response.length === (request & 2 ? 7 : 3), "Register transfer response length mismatch");
    return request & 2 ? response.readUInt32LE(3) : 0;
  }

  async connect(manifest: Pick<Manifest, "dpidr" | "cpuid">): Promise<void> {
    equal(await this.command(bytes(2, 1)), bytes(2, 1), "SWD connect failed");
    equal(await this.command(Buffer.concat([bytes(0x11), words([1000000])])), bytes(0x11, 0), "Clock setup failed");
    equal(await this.command(bytes(0x13, 0)), bytes(0x13, 0), "SWD setup failed");
    for (const data of [bytes(0x38, ...Array(7).fill(255)), bytes(0x10, 0x9e, 0xe7),
      bytes(0x38, ...Array(7).fill(255)), bytes(8, 0)]) {
      equal(await this.command(Buffer.concat([bytes(0x12), data])), bytes(0x12, 0), "SWJ sequence failed");
    }
    assert(await this.transfer(2) === manifest.dpidr, "Unexpected debug port ID");
    await this.transfer(0, 0x1e);
    await this.transfer(4, 0x50000000);
    const deadline = performance.now() + 3000;
    while (((await this.transfer(6)) & 0xa0000000) >>> 0 !== 0xa0000000) {
      assert(performance.now() < deadline, "Debug power-up timed out");
    }
    await this.transfer(8, 0);
    await this.transfer(1, 0x23000052); // AP CSW: word accesses, increment address.
    assert(await this.read(0xe000ed00) === manifest.cpuid, "Completed AP read returned the wrong CPU ID");
  }

  async read(address: number): Promise<number> {
    await this.transfer(5, address);
    return this.transfer(15);
  }

  async write(address: number, value: number): Promise<void> {
    await this.transfer(5, address);
    await this.transfer(13, value);
  }

  async block(address: number, count: number, values?: number[]): Promise<number[]> {
    assert(count >= 1 && count <= 255, "Invalid test block size");
    assert((address & 1023) + count * 4 <= 1024, "Block crosses AP increment boundary");
    assert(values === undefined || values.length === count, "Block value count mismatch");
    await this.transfer(5, address);
    const data = Buffer.concat([bytes(6, 0, count & 255, count >> 8, values === undefined ? 15 : 13),
      values === undefined ? bytes() : words(values)]);
    const response = await this.command(data);
    equal(response.subarray(1, 4), bytes(count & 255, count >> 8, 1), "Block transfer failed");
    assert(response.length === (values === undefined ? 4 + count * 4 : 4), "Block response length mismatch");
    return values === undefined ? readWords(response.subarray(4)) : [];
  }

  async close(): Promise<void> {
    if (this.connection.closed) return;
    try {
      equal(await this.command(bytes(3)), bytes(3, 0), "SWD disconnect failed");
    } finally {
      this.destroy();
    }
  }

  destroy(): void {
    this.connection.destroy();
  }
}

export async function testDapFraming(dap: Dap): Promise<void> {
  const frame = Dap.frame(bytes(0, 255));
  for (const part of [frame.subarray(0, 1), frame.subarray(1, 5), frame.subarray(5, 8), frame.subarray(8)]) {
    dap.send(part);
    await Bun.sleep(10); // Ensure the server also sees partial headers and payloads.
  }
  equal(await dap.response(0), bytes(0, 2, 0, 4), "Packet-size response mismatch");
  dap.send(Buffer.concat([Dap.frame(bytes(0, 0xf0)), Dap.frame(bytes(0, 0xfe))]));
  const capabilities = await dap.response(0);
  assert(capabilities.length === 3 && capabilities[1] === 1 && (capabilities[2]! & 1), "SWD capability missing");
  equal(await dap.response(0), bytes(0, 1, 8), "Packet count mismatch");
  const requests = Array.from({ length: 8 }, (_, index) => bytes(0, index % 2 === 0 ? 255 : 254));
  dap.send(Buffer.concat(requests.map(Dap.frame)));
  for (const request of requests) {
    equal(await dap.response(0), request[1] === 255 ? bytes(0, 2, 0, 4) : bytes(0, 1, 8),
      "Pipelined info responses are out of order");
  }
}

class Tcl {
  private constructor(private readonly connection: TcpConnection) {}

  static async open(port: number): Promise<Tcl> {
    return new Tcl(await TcpConnection.open("127.0.0.1", port, 10000));
  }

  async command(command: string): Promise<string> {
    this.connection.send(Buffer.from(`${command}\x1a`));
    return (await this.connection.until(0x1a)).toString();
  }

  async read(address: number): Promise<number> {
    const response = (await this.command(`mrw ${address}`)).trim();
    assert(response.length > 0, "Empty memory read response");
    const value = Number(response);
    assert(Number.isInteger(value) && value >= 0 && value <= 0xffffffff, "Invalid memory read response");
    return value;
  }

  async write(address: number, value: number): Promise<void> {
    await this.command(`mww ${address} ${value}`);
    assert(await this.read(address) === value, "Memory write did not take effect");
  }

  close(): void {
    this.connection.destroy();
  }
}

async function runCommand(command: string[], timeoutMs: number) {
  const child = Bun.spawn(command, { stdout: "pipe", stderr: "pipe" });
  let timedOut = false;
  const timeout = setTimeout(() => { timedOut = true; child.kill("SIGKILL"); }, timeoutMs);
  try {
    const [stdout, stderr, exitCode] = await Promise.all([
      new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited,
    ]);
    assert(!timedOut, `${command[0]} timed out after ${timeoutMs} ms:\n${stdout}${stderr}`);
    return { stdout, stderr, exitCode };
  } finally {
    clearTimeout(timeout);
  }
}

async function findOpenocd(override?: string): Promise<string> {
  const candidates = override ? [override] : [Bun.which("openocd")];
  if (!override) {
    const glob = new Bun.Glob(".espressif/tools/openocd-*/*/openocd-*/bin/openocd");
    for await (const path of glob.scan({ cwd: homedir(), absolute: true, dot: true })) candidates.push(path);
  }
  for (const candidate of candidates) {
    if (!candidate) continue;
    try {
      const result = await runCommand([candidate, "-c", "adapter driver cmsis-dap", "-c", "help cmsis-dap tcp",
        "-c", "shutdown"], 10000);
      if ((result.stdout + result.stderr).includes("cmsis-dap tcp host")) return candidate;
    } catch {
      // Continue searching if a local installation cannot be launched.
    }
  }
  throw new Error("No TCP-capable OpenOCD found; pass --openocd /path/to/openocd");
}

async function freePort(): Promise<number> {
  const server = createServer();
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const address = server.address();
  assert(address !== null && typeof address !== "string", "Could not allocate a local server port");
  await new Promise<void>((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  return address.port;
}

type TestContext = {
  host: string;
  dapPort: number;
  manifest: Manifest;
  imagePath: string;
  image: Buffer;
  common: string[];
  gdb: string;
  directory: string;
  symbol: (name: string) => number;
};
type TestReport = {
  host: string;
  fixtureSha256: string;
  started: string;
  finished?: string;
  checks: { name: string; passed: boolean; detail?: string }[];
  failures: string[];
};

function log(message: string): void {
  console.log(`[hardware-dap] ${message}`);
}

function errorMessage(error: unknown): string {
  if (error instanceof AggregateError) return error.errors.map(errorMessage).join("; ");
  return error instanceof Error ? error.message : String(error);
}

async function requestJson<T>(
  host: string, method: string, path: string, body?: unknown, expectedStatus = 200,
): Promise<T> {
  const command = [
    "curl", "--noproxy", "*", "--silent", "--show-error", "--max-time", "10",
    "--request", method, "--url", `http://${host}${path}`, "--write-out", "\n%{http_code}",
    "--header", "Accept: application/json",
  ];
  if (body !== undefined) {
    command.push("--header", "Content-Type: application/json", "--data-binary", JSON.stringify(body));
  }
  const result = await runCommand(command, 12000);
  assert(result.exitCode === 0, `${method} ${path} transport failed: ${result.stderr.trim()}`);
  const separator = result.stdout.lastIndexOf("\n");
  assert(separator >= 0, `${method} ${path} transport omitted HTTP status`);
  const status = Number.parseInt(result.stdout.slice(separator + 1), 10);
  const text = result.stdout.slice(0, separator);
  assert(status === expectedStatus, `${method} ${path} returned ${status}, expected ${expectedStatus}: ${text}`);
  if (expectedStatus === 204) return undefined as T;
  try {
    return JSON.parse(text) as T;
  } catch {
    throw new Error(`${method} ${path} returned invalid JSON: ${text.slice(0, 240)}`);
  }
}

async function cleanup(host: string): Promise<void> {
  log("disabling DAP and releasing test pins");
  try {
    await requestJson<void>(host, "POST", "/dap/config", {
      enable: false, pins: { swclk: 0, swdio: 1, nreset: null },
    }, 204);
  } finally {
    await requestJson<void>(host, "PUT", "/pin/2", { mode: "disable", pullUp: false, pullDown: false }, 204);
  }
}

async function configureTarget(host: string, dapPort: number): Promise<void> {
  log("3.3 V power, DAP configuration and probe input");
  await requestJson<void>(host, "POST", "/power/3v3", { enable: true }, 204);
  assert((await requestJson<{ enable: boolean }>(host, "GET", "/power/3v3")).enable, "3.3 V output failed to enable");
  const config = { enable: true, pins: { swclk: 0, swdio: 1, nreset: null } };
  await requestJson<void>(host, "POST", "/dap/config", config, 204);
  const status = await requestJson<{ port: number }>(host, "GET", "/dap/status");
  assert(status.port === dapPort, "DAP status port mismatch");
  await requestJson<void>(host, "PUT", "/pin/2", { mode: "digitalInput", pullUp: false, pullDown: false }, 204);
}

async function serveOpenocd(common: string[]): Promise<void> {
  log("starting local GDB server at 127.0.0.1:3333 (3.3 V enabled)");
  const server = Bun.spawn(common, { stdin: "inherit", stdout: "inherit", stderr: "inherit" });
  const stop = () => server.kill();
  process.on("SIGINT", stop);
  process.on("SIGTERM", stop);
  try {
    assert(await server.exited === 0, "OpenOCD server failed");
  } finally {
    process.off("SIGINT", stop);
    process.off("SIGTERM", stop);
  }
}

async function runSection(report: TestReport, name: string, action: () => Promise<void>): Promise<void> {
  log(name);
  try {
    await action();
    report.checks.push({ name, passed: true });
  } catch (error) {
    const detail = errorMessage(error);
    report.checks.push({ name, passed: false, detail });
    throw new Error(`${name}: ${detail}`, { cause: error });
  }
}

async function testDapConnection(context: TestContext): Promise<void> {
  const { host, dapPort, manifest } = context;
  const dap = await Dap.open(host, dapPort);
  try {
    await testDapFraming(dap);
    await dap.connect(manifest);
  } finally {
    await dap.close();
  }
}

async function testFlash(context: TestContext): Promise<void> {
  const { common, imagePath, manifest } = context;
  // Flashing requires no target source tree, compiler, ELF or debug symbols.
  const flash = await runCommand([...common, "-c", "gdb_port disabled", "-c", "tcl_port disabled",
    "-c", "telnet_port disabled", "-c", `program {${imagePath}} ${manifest.flashAddress} verify reset exit`], 60000);
  assert(flash.exitCode === 0, `Flash/verify failed:\n${flash.stdout}${flash.stderr}`);
}

async function testDebugger(context: TestContext): Promise<void> {
  const { common, gdb, directory, manifest, symbol, host } = context;
  const gdbPort = await freePort();
  let tclPort = await freePort();
  while (tclPort === gdbPort) tclPort = await freePort();
  const server = Bun.spawn([...common, "-c", `gdb_port ${gdbPort}`, "-c", `tcl_port ${tclPort}`,
    "-c", "telnet_port disabled"], { stdout: "pipe", stderr: "pipe" });
  const output = Promise.all([new Response(server.stdout).text(), new Response(server.stderr).text()]);
  let tcl: Tcl | undefined;
  try {
    const deadline = performance.now() + 10000;
    while (true) {
      if (server.exitCode !== null) throw new Error(`OpenOCD exited during startup:\n${(await output).join("")}`);
      try {
        tcl = await Tcl.open(tclPort);
        // Tcl opens before target examination finishes; wait for init to complete.
        await tcl.command("targets");
        break;
      } catch (error) {
        tcl?.close();
        tcl = undefined;
        const code = (error as NodeJS.ErrnoException).code;
        if (code !== "ECONNREFUSED" && !(error instanceof Error && error.message.includes("timed out"))) throw error;
        assert(performance.now() < deadline, "OpenOCD startup timed out");
        await Bun.sleep(100);
      }
    }
    log("GDB hardware breakpoint, single step and data watchpoint");
    const counter = symbol("debugHeartbeatCount"), tick = symbol("DebugTick");
    const script = join(directory, "check.gdb");
    await writeFile(script, [
      "set pagination off",
      "set confirm off",
      "set remotetimeout 10",
      `target extended-remote 127.0.0.1:${gdbPort}`,
      "monitor reset halt",
      `hbreak *${tick}`,
      "continue",
      `if $pc != ${tick}`,
      "  quit 1",
      "end",
      "set $oldpc = $pc",
      "stepi",
      "if $pc == $oldpc",
      "  quit 1",
      "end",
      "delete breakpoints",
      `set $before = *(unsigned int*)${counter}`,
      `watch -location *(unsigned int*)${counter}`,
      "continue",
      `if *(unsigned int*)${counter} != $before + 1`,
      "  quit 1",
      "end",
      "delete breakpoints",
      "monitor reset run",
      "detach",
      "echo DAP_GDB_PASS\\n",
      "",
    ].join("\n"));
    const result = await runCommand([gdb, "-q", "-nx", "-batch", "-x", script], 30000);
    assert(result.exitCode === 0 && result.stdout.includes("DAP_GDB_PASS"),
      `GDB breakpoint/step/watchpoint failed:\n${result.stdout}${result.stderr}`);
    log("fixture identity, startup and system clock");
    await Bun.sleep(200);
    for (const [name, expected] of [["debugFixtureMagic", manifest.magic],
      ["debugFixtureVersion", manifest.fixtureVersion], ["debugReady", 1],
      ["SystemCoreClock", manifest.systemClockHz]] as const) {
      assert(await tcl.read(symbol(name)) === expected, `Fixture field mismatch: ${name}`);
    }
    log("halt freezes execution; resume restarts it");
    await tcl.command("halt");
    const count = await tcl.read(counter);
    await Bun.sleep(200);
    assert(await tcl.read(counter) === count, "Heartbeat advanced while halted");
    await tcl.command("resume");
    await Bun.sleep(700);
    assert(await tcl.read(counter) > count, "Heartbeat did not advance after resume");
    log("debugger-controlled GPIO low/high observed on pin 2");
    for (const level of [0, 1, 0, 1]) {
      await tcl.write(symbol("debugOutputMode"), level);
      await Bun.sleep(650);
      assert(await tcl.read(symbol("debugOutputLevel")) === level, "Fixture output state mismatch");
      const observed = await requestJson<{ level: number }>(host, "GET", "/pin/2/level");
      assert(observed.level === level, "Physical output signal mismatch");
    }
    log("programmable heartbeat and physical edge timing");
    await tcl.write(symbol("debugHeartbeatPeriodMs"), 200);
    await tcl.write(symbol("debugOutputMode"), 2);
    await Bun.sleep(700);
    const { events } = await requestJson<{ events: { time: number; level: number }[] }>(
      host, "GET", "/pin/2/trace?edge=both&duration=1400000");
    const intervals = events.slice(1).map((event, index) => (event.time - events[index]!.time) / 1000);
    assert(events.length >= 5, "Too few heartbeat edges");
    assert(events.slice(1).every((event, index) => event.level !== events[index]!.level), "Repeated edge levels");
    assert(intervals.every((value) => value >= 180 && value <= 240), `Heartbeat timing mismatch: ${intervals}`);
    await tcl.write(symbol("debugHeartbeatPeriodMs"), manifest.defaultPeriodMs);
    await tcl.command("halt");
  } finally {
    if (tcl) {
      try {
        await tcl.write(symbol("debugHeartbeatPeriodMs"), manifest.defaultPeriodMs);
        await tcl.write(symbol("debugOutputMode"), 2);
        if (await tcl.command("stm32f1x.cpu curstate") === "halted") await tcl.command("resume");
      } catch (error) {
        log(`could not restore heartbeat before stopping OpenOCD: ${errorMessage(error)}`);
      }
      tcl.close();
    }
    server.kill();
    const timeout = setTimeout(() => server.kill("SIGKILL"), 5000);
    try {
      await server.exited;
      await output;
    } finally {
      clearTimeout(timeout);
    }
  }
}

async function testDapTransfers(context: TestContext): Promise<void> {
  const { host, dapPort, manifest, image, symbol } = context;
  const dap = await Dap.open(host, dapPort);
  try {
    await dap.connect(manifest);
    await dap.write(0xe000edf0, 0xa05f0003); // Debug enable + halt.
    assert((await dap.read(0xe000edf0)) & (1 << 17), "Target did not halt");
    log("empty blocks and repeated DP reads");
    for (const request of [13, 15]) {
      equal(await dap.command(bytes(6, 0, 0, 0, request)), bytes(6, 0, 0, 1), "Empty block transfer failed");
    }
    equal(await dap.command(bytes(6, 0, 4, 0, 2)),
      Buffer.concat([bytes(6, 4, 0, 1), words(Array(4).fill(manifest.dpidr))]),
      "DP block read returned the wrong values");
    log("mixed DAP_Transfer writes/reads return current values");
    // Alternating distinct AP addresses in one packet catches stale posted results.
    const packet = Buffer.concat([bytes(5, 0, 4, 5), words([symbol("debugFixtureMagic")]), bytes(15, 5),
      words([symbol("debugFixtureVersion")]), bytes(15)]);
    const expected = Buffer.concat([bytes(5, 4, 1), words([manifest.magic, manifest.fixtureVersion])]);
    equal(await dap.command(packet), expected, "Mixed AP reads returned stale values");
    log("DAP_TransferBlock full firmware read-back");
    const chunks: Buffer[] = [];
    let address = manifest.flashAddress, length = 0;
    while (length < image.length) {
      const count = Math.min(252, (1024 - (address & 1023)) / 4, (image.length - length) / 4);
      const chunk = words(await dap.block(address, count));
      chunks.push(chunk);
      address += chunk.length;
      length += chunk.length;
    }
    equal(Buffer.concat(chunks), image, "DAP block flash read-back differs from the fixture");
    const setup = Buffer.concat([bytes(5, 0, 1, 5), words([manifest.flashAddress])]);
    dap.send(Buffer.concat([Dap.frame(setup), Dap.frame(bytes(6, 0, 255, 0, 15))]));
    equal(await dap.response(5), bytes(5, 1, 1), "Pipelined address setup failed");
    equal(await dap.response(6), Buffer.concat([bytes(6, 255, 0, 1), image.subarray(0, 1020)]),
      "Maximum-sized block response failed");
    log("block RAM writes, single/block reads and restoration");
    const scratch = symbol("debugScratch"), count = manifest.scratchWords;
    const saved = await dap.block(scratch, count);
    let seed = 20261107;
    const patterns = [0, 0xffffffff, 0xaaaaaaaa, 0x55555555];
    while (patterns.length < count) {
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      patterns.push(seed);
    }
    try {
      await dap.block(scratch, count, patterns);
      equal(words(await dap.block(scratch, count)), words(patterns), "Block RAM round trip failed");
      const individual: number[] = [];
      for (let index = 0; index < count; index++) individual.push(await dap.read(scratch + index * 4));
      equal(words(individual), words(patterns), "Individual RAM reads differ from block writes");
      const values = patterns.map((value) => (value ^ 0xa5a5a5a5) >>> 0);
      const setup = Buffer.concat([bytes(5, 0, count + 1, 5), words([scratch])]);
      const packet = Buffer.concat([setup, ...values.map((value) => Buffer.concat([bytes(13), words([value])]))]);
      equal(await dap.command(packet), bytes(5, count + 1, 1), "Ordinary transfer write run failed");
      equal(await dap.command(Buffer.concat([setup, Buffer.alloc(count, 15)])),
        Buffer.concat([bytes(5, count + 1, 1), words(values)]), "Ordinary transfer read run returned stale values");
      equal(words(await dap.block(scratch, count)), words(values), "Block read differs from ordinary write run");
      await dap.block(scratch, count, patterns);
      const single = (patterns[0]! ^ 0xffffffff) >>> 0;
      await dap.block(scratch, 1, [single]);
      equal(words(await dap.block(scratch, 1)), words([single]), "Single-word block round trip failed");
      assert(await dap.read(scratch) === single, "Ordinary read after a block returned stale data");
      equal(words(await dap.block(scratch + 4, count - 1)), words(patterns.slice(1)),
        "Single-word write changed adjacent RAM");
    } finally {
      await dap.block(scratch, count, saved);
      equal(words(await dap.block(scratch, count)), words(saved), "Scratch RAM restoration failed");
    }
  } finally {
    try {
      await dap.write(symbol("debugHeartbeatPeriodMs"), manifest.defaultPeriodMs);
      await dap.write(symbol("debugOutputMode"), 2);
      await dap.write(0xe000edf0, 0xa05f0001); // Resume, leaving the LED blinking.
    } finally {
      await dap.close();
    }
  }
}

async function main(): Promise<void> {
  const { values: args } = parseArgs({ options: {
    host: { type: "string", default: process.env.SAIHUB_IP ?? "192.168.88.165" },
    "dap-port": { type: "string", default: "4441" },
    openocd: { type: "string", default: process.env.OPENOCD },
    gdb: { type: "string", default: "arm-none-eabi-gdb" },
    report: { type: "string" }, serve: { type: "boolean", default: false }, help: { type: "boolean", default: false },
  } });
  if (args.help) {
    console.log("bun tests/hardware/dap.ts [--host HOST] [--dap-port 4441] [--openocd PATH] [--gdb PATH]");
    console.log("Optional: --report PATH writes JSON results, including failures and cleanup errors.");
    console.log("Flashes the checked-in SWD fixture and tests DAP, OpenOCD, GDB and physical IO.");
    console.log("Requires exclusive DAP access. Wiring: SWCLK pin 0, SWDIO pin 1, fixture output pin 2.");
    console.log("--serve enables 3.3 V and runs local OpenOCD without flashing or testing.");
    return;
  }
  const dapPort = Number(args["dap-port"]);
  assert(Number.isInteger(dapPort) && dapPort > 0 && dapPort <= 65535, "--dap-port must be an integer from 1 to 65535");
  log(`target http://${args.host}; DAP port ${dapPort}`);
  log("fixture integrity and debugger tools");
  const manifest: Manifest = JSON.parse(await readFile(new URL("manifest.json", FIXTURE), "utf8"));
  const imagePath = Bun.fileURLToPath(new URL(manifest.file, FIXTURE));
  const image = await readFile(imagePath);
  assert(image.length === manifest.size && image.length % 4 === 0, "Fixture size mismatch");
  assert(createHash("sha256").update(image).digest("hex") === manifest.sha256, "Fixture digest mismatch");
  const openocd = await findOpenocd(args.openocd);
  const gdb = Bun.which(args.gdb);
  assert(args.serve || gdb, "arm-none-eabi-gdb is required; use --gdb to select it");
  const common = [openocd, "-c", `set SAIHUB_HOST ${args.host}`, "-c", `set SAIHUB_DAP_PORT ${dapPort}`,
    "-f", Bun.fileURLToPath(new URL("openocd.cfg", FIXTURE))];
  if (args.serve) {
    try {
      await configureTarget(args.host, dapPort);
      await serveOpenocd(common);
    } finally {
      await cleanup(args.host);
    }
    return;
  }
  const directory = await mkdtemp(join(tmpdir(), "saihub-dap-"));
  const context: TestContext = {
    host: args.host, dapPort, manifest, imagePath, image, common, gdb: gdb!, directory,
    symbol: (name: string): number => {
      const address = manifest.symbols[name];
      assert(address !== undefined, `Fixture symbol missing: ${name}`);
      return address;
    },
  };
  const report: TestReport = {
    host: args.host, fixtureSha256: manifest.sha256, started: new Date().toISOString(), checks: [], failures: [],
  };
  let failure: unknown;
  try {
    await configureTarget(args.host, dapPort);
    await runSection(report, "TCP framing, SWD connect and completed AP reads", () => testDapConnection(context));
    await runSection(report, "OpenOCD flash and read-back verification", () => testFlash(context));
    await runSection(report, "debugger execution and physical IO", () => testDebugger(context));
    await runSection(report, "DAP register and block transfers", () => testDapTransfers(context));
  } catch (error) {
    failure = error;
  } finally {
    try {
      await cleanup(args.host);
    } catch (error) {
      failure = failure === undefined ? error : new AggregateError([failure, error], "Test and cleanup both failed");
    }
    log("removing temporary test files");
    try {
      await rm(directory, { recursive: true, force: true });
    } catch (error) {
      failure = failure === undefined ? error : new AggregateError([failure, error], "Test and cleanup both failed");
    }
    report.finished = new Date().toISOString();
    if (failure !== undefined) report.failures.push(errorMessage(failure));
    if (args.report) {
      try {
        await writeFile(args.report, `${JSON.stringify(report, null, 2)}\n`);
      } catch (error) {
        failure = failure === undefined ? error : new AggregateError([failure, error], "Test and report both failed");
      }
    }
  }
  if (failure !== undefined) throw failure;
  log(`PASS: ${report.checks.length} DAP sections and cleanup; fixture LED blinking`);
}

if (import.meta.main) {
  try {
    await main();
  } catch (error) {
    console.error(`[hardware-dap] FAIL: ${errorMessage(error)}`);
    process.exitCode = 1;
  }
}
