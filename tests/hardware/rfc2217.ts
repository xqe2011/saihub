/**
 * Flash the checked-in UART echo fixture over RFC 2217, then verify reset and binary echo.
 * Requires Python with esptool and PySerial. The test overwrites the target's application.
 * SaiHub wiring: RX pin 2, TX pin 3, reset/EN pin 4 (RTS), BOOT/GPIO28 pin 5 (DTR), 5V and common ground.
 */
import { parseArgs } from "node:util";
import { cloudStreamUrl, hardwareTarget, startCloudTcpProxy, type CloudTcpProxy, type StreamStatus } from "../library/cloud-stream.ts";

const fixture = Bun.fileURLToPath(new URL("../fixtures/rfc2217/firmware.hex", import.meta.url));
const routingToken = process.env.SAIHUB_ROUTING_TOKEN?.trim();

function log(message: string): void {
  console.log(`[hardware-rfc2217] ${message}`);
}

const verifyEcho = `
import serial
import sys
import time

port = serial.serial_for_url(sys.argv[1], baudrate=115200, timeout=5, do_not_open=True)
port.dtr = False
port.rts = False
with port:
    port.reset_input_buffer()
    port.rts = True
    time.sleep(0.1)
    port.rts = False
    ready = b"SAIHUB RFC2217 READY\\r\\n"
    boot_log = port.read_until(ready)
    if not boot_log.endswith(ready):
        raise RuntimeError(f"Missing fixture boot marker after reset: {boot_log[-256:]!r}")
    print("[hardware-rfc2217] target rebooted via RTS and reported ready", flush=True)

    payload = bytes(range(256)) + b"\\xff\\xff"
    port.write(payload)
    echoed = port.read(len(payload))
    if echoed != payload:
        raise RuntimeError(f"Binary echo mismatch: expected {payload.hex()}, received {echoed.hex()}")
    print(f"[hardware-rfc2217] verified {len(echoed)} echoed bytes, including adjacent 0xff bytes", flush=True)
`;

async function run(command: string[]): Promise<void> {
  const child = Bun.spawn(command, { stdout: "inherit", stderr: "inherit" });
  const code = await child.exited;
  if (code !== 0) throw new Error(`${command[0]} exited with status ${code}`);
}

async function request<T = void>(base: URL, path: string, body?: object): Promise<T> {
  const command = ["curl", "--noproxy", "*", "--fail-with-body", "--silent", "--show-error", "--max-time", "10"];
  if (routingToken) command.push("--header", `Authorization: Bearer ${routingToken}`);
  if (body !== undefined) command.push("-H", "Content-Type: application/json", "--data", JSON.stringify(body));
  const url = `${base.origin}${base.pathname.replace(/\/$/, "")}${path}`;
  const child = Bun.spawn([...command, url], { stdout: "pipe", stderr: "inherit" });
  const [response, code] = await Promise.all([new Response(child.stdout).text(), child.exited]);
  if (code !== 0) throw new Error(`${path} failed: ${response || `curl exited with status ${code}`}`);
  return (response ? JSON.parse(response) : undefined) as T;
}

async function main(): Promise<void> {
  const { values: args } = parseArgs({ options: {
    host: { type: "string", default: process.env.SAIHUB_TARGET ?? process.env.SAIHUB_IP ?? "192.168.88.165" },
    help: { type: "boolean", default: false },
  } });
  if (args.help) {
    console.log("bun tests/hardware/rfc2217.ts [--host HOST]");
    console.log("Host defaults to SAIHUB_TARGET, SAIHUB_IP, or 192.168.88.165.");
    console.log("Cloud mode uses SAIHUB_TARGET=http://RELAY/device/DIGEST and SAIHUB_ROUTING_TOKEN.");
    console.log("Requires Python with esptool and PySerial, and UART 0 disabled in native mode.");
    console.log("Flashes the checked-in HEX fixture and verifies reset and binary echo.");
    return;
  }

  const base = hardwareTarget(args.host, Boolean(routingToken));
  log(`target ${base.origin}${base.pathname}`);
  if (!(await Bun.file(fixture).exists())) throw new Error(`Missing fixture: ${fixture}`);
  const { uarts } = await request<{ uarts: { id: number; enable: boolean; mode: string }[] }>(base, "/uart/");
  const uart = uarts.find(({ id }) => id === 0);
  if (!uart || uart.enable || uart.mode !== "native") throw new Error("UART 0 must be disabled in native mode");
  const { id, ...previous } = uart;
  const configPath = `/uart/${id}/config`;
  const powerPath = "/power/5v";
  const power = await request<{ enable: boolean; reason?: string }>(base, powerPath);
  if (power.reason) throw new Error(`Target power fault: ${power.reason}`);

  log("5V power and RFC 2217 configuration");
  try {
    if (!power.enable) {
      await request(base, powerPath, { enable: true });
      await Bun.sleep(1000);
    }
    await request(base, configPath, { enable: true, mode: "rfc2217", pins: { rx: 2, tx: 3, rts: 4, dtr: 5 } });
    let proxy: CloudTcpProxy | undefined;
    try {
      const statusPath = `/uart/${id}/rfc2217`;
      const status = await request<StreamStatus>(base, statusPath);
      if (routingToken) {
        cloudStreamUrl(status, base, "rfc2217");
        proxy = await startCloudTcpProxy(async () => {
          const current = await request<StreamStatus>(base, statusPath);
          return cloudStreamUrl(current, base, "rfc2217");
        }, log);
        log(`RFC2217 cloud proxy listening on ${proxy.host}:${proxy.port}`);
      }
      const port = proxy?.port ?? status.port;
      if (typeof port !== "number" || !Number.isInteger(port) || port < 1 || port > 65535) {
        throw new Error(`Invalid RFC 2217 port: ${port}`);
      }
      const url = `rfc2217://${proxy?.host ?? base.hostname}:${port}`;
      log(`automatic download, flash and verification via ${url}`);
      await run(["python3", "-m", "esptool", "--chip", "esp32c5", "--port", url, "--baud", "460800",
        "--before", "default-reset", "--after", "hard-reset", "write-flash", "0x0", fixture]);
      log("RTS reset and binary echo");
      await run(["python3", "-c", verifyEcho, url]);
    } finally {
      try {
        await proxy?.close();
      } finally {
        log("restoring UART and 5V settings");
        await request(base, configPath, previous);
      }
    }
  } finally {
    if (!power.enable) await request(base, powerPath, { enable: false });
  }
  log("PASS: flash verification, RTS reset, binary echo and cleanup");
}

if (import.meta.main) {
  try {
    await main();
  } catch (error) {
    console.error(`[hardware-rfc2217] FAIL: ${error instanceof Error ? error.message : String(error)}`);
    process.exitCode = 1;
  }
}
