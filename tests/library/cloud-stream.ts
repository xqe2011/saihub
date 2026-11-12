import { ok, strictEqual } from "node:assert/strict";
import { once } from "node:events";
import { createServer, type Socket } from "node:net";

type Service = "dap" | "rfc2217";
export type StreamStatus = { url?: string; prompt?: string; ip?: string; port?: number };
export type CloudTcpProxy = { host: string; port: number; close(): Promise<void> };

const LOOPBACK = "127.0.0.1";

export function hardwareTarget(value: string, cloud = false): URL {
  const target = new URL(/^https?:\/\//i.test(value) ? value : `http://${value}`);
  target.pathname = target.pathname.replace(/\/$/, "");
  if (cloud) {
    ok(/^\/device\/[^/]+$/.test(target.pathname) && !target.search && !target.hash,
      "Set SAIHUB_TARGET to the cloud device URL ending in /device/DIGEST");
  }
  return target;
}

export function cloudStreamUrl(status: StreamStatus, target: URL, service: Service): URL {
  ok(typeof status.prompt === "string" && status.prompt.length, `${service}: missing connection instructions`);
  ok(typeof status.url === "string", `${service}: missing cloud stream URL`);
  strictEqual(status.port, undefined, `${service}: cloud status returned a LAN port`);
  strictEqual(status.ip, undefined, `${service}: cloud status returned a LAN IP`);
  const url = new URL(status.url);
  ok(url.origin === target.origin.replace(/^http/, "ws"), `${service}: unexpected WebSocket origin`);
  ok(url.pathname === `${target.pathname}/tcp-stream/${service}`, `${service}: unexpected WebSocket path`);
  const tokens = url.searchParams.getAll("token");
  ok(tokens.length === 1 && /^[A-Za-z0-9_-]+$/.test(tokens[0]!), `${service}: expected one relay-issued token`);
  return url;
}

function bridgeConnection(tcp: Socket, url: URL): Promise<void> {
  const ws = new WebSocket(url.href);
  ws.binaryType = "arraybuffer";
  return new Promise<void>((resolve, reject) => {
    let failure: Error | undefined;
    let closeTimer: ReturnType<typeof setTimeout> | undefined;

    const finish = () => {
      clearTimeout(openTimer);
      clearTimeout(closeTimer);
      clearInterval(heartbeat);
      tcp.off("close", stop);
      tcp.destroy();
      if (failure) reject(failure);
      else resolve();
    };
    const stop = () => {
      if (closeTimer !== undefined) return;
      clearTimeout(openTimer);
      clearInterval(heartbeat);
      closeTimer = setTimeout(() => {
        if (ws.readyState !== WebSocket.CLOSED) ws.terminate();
        finish();
      }, 1000);
      if (ws.readyState < WebSocket.CLOSING) ws.close();
    };
    const openTimer = setTimeout(() => {
      failure = new Error("Cloud stream upgrade timed out");
      stop();
    }, 5000);
    const heartbeat = setInterval(() => {
      if (ws.readyState === WebSocket.OPEN) ws.send("ping");
    }, 10000);

    tcp.once("close", stop);
    tcp.on("data", (data) => {
      if (ws.readyState === WebSocket.OPEN) ws.send(data);
    });
    ws.addEventListener("open", () => {
      clearTimeout(openTimer);
      tcp.resume();
    });
    ws.addEventListener("message", ({ data }) => {
      if (data instanceof ArrayBuffer && !tcp.destroyed) tcp.write(Buffer.from(data));
    });
    ws.addEventListener("error", () => {
      failure ??= new Error("Cloud stream WebSocket failed");
      stop();
    });
    ws.addEventListener("close", ({ code }) => {
      if (closeTimer === undefined && code !== 1000) failure ??= new Error(`Cloud stream closed with code ${code}`);
      finish();
    });
  });
}

export async function startCloudTcpProxy(
  getUrl: () => Promise<URL>, log: (message: string) => void, port = 0,
): Promise<CloudTcpProxy> {
  const clients = new Set<Socket>();
  let pending = Promise.resolve();
  const server = createServer({ pauseOnConnect: true }, (tcp) => {
    clients.add(tcp);
    tcp.setNoDelay(true);
    tcp.on("error", () => tcp.destroy());
    tcp.once("close", () => clients.delete(tcp));
    // The relay permits one stream per service. Wait for the previous client to close before opening another.
    pending = pending.then(async () => {
      if (tcp.destroyed) return;
      const url = await getUrl();
      if (!tcp.destroyed) await bridgeConnection(tcp, url);
    }).catch((error: unknown) => {
      log(`cloud proxy failed: ${error instanceof Error ? error.message : String(error)}`);
      tcp.destroy();
    });
  });
  server.listen(port, LOOPBACK);
  await once(server, "listening");
  const address = server.address();
  ok(address && typeof address !== "string", "Could not determine the cloud proxy address");
  return {
    host: LOOPBACK,
    port: address.port,
    async close() {
      const closed = new Promise<void>((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
      for (const tcp of clients) tcp.destroy();
      await Promise.all([closed, pending]);
    },
  };
}
