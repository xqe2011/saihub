import { describe, expect, test } from "bun:test";
import { Device } from "../../../cloud/cloudflare/src/device.ts";
import type { Env } from "../../../cloud/cloudflare/src/env.ts";
import type { RequestMessage } from "../../../cloud/cloudflare/src/protocol.ts";

function fixture(timeout = "10000") {
  const sent: RequestMessage[] = [];
  let failSend = false;
  const socket = {
    deserializeAttachment: () => ({ authenticated: true }),
    send: (data: string) => {
      if (failSend) throw new Error("offline");
      sent.push(JSON.parse(data) as RequestMessage);
    },
  } as unknown as WebSocket;
  const ctx = {
    getWebSockets: () => [socket],
    storage: { deleteAlarm: async () => {} },
  } as unknown as DurableObjectState;
  const device = new Device(ctx, { REQUEST_TIMEOUT_MS: timeout } as Env);
  const request = (index: number) => device.fetch(new Request(`https://cloud/proxy?path=/request/${index}`));
  const reply = (index: number) => device.webSocketMessage(socket, JSON.stringify({
    type: "response", requestId: sent[index]!.requestId, status: 200,
    headers: { "content-type": "application/json" }, body: { ok: true },
  }));
  return { device, socket, sent, request, reply, fail: () => { failSend = true; } };
}

const tick = () => new Promise<void>((resolve) => setTimeout(resolve, 0));

describe("cloud headroom", () => {
  test("caps pending at eight and promotes one FIFO request per response", async () => {
    const f = fixture();
    const responses = Array.from({ length: 11 }, (_, i) => f.request(i));
    await tick();
    expect(f.sent.map((m) => m.path)).toEqual(Array.from({ length: 8 }, (_, i) => `/request/${i}`));
    await f.reply(3);
    expect(f.sent.length).toBe(9);
    expect(f.sent[8]!.path).toBe("/request/8");
    await f.reply(3); // Duplicate response must not free another slot.
    expect(f.sent.length).toBe(9);
    await f.reply(0);
    expect(f.sent[9]!.path).toBe("/request/9");
    await f.reply(1);
    expect(f.sent[10]!.path).toBe("/request/10");
    for (const i of [2, 4, 5, 6, 7, 8, 9, 10]) await f.reply(i);
    expect((await Promise.all(responses)).every((r) => r.status === 200)).toBe(true);
  });

  test("timeouts free slots and queued requests receive their own timeout window", async () => {
    const f = fixture("50");
    const responses = Array.from({ length: 9 }, (_, i) => f.request(i));
    expect((await responses[0]!).status).toBe(504);
    expect(f.sent.length).toBe(9);
    await f.reply(8);
    expect((await responses[8]!).status).toBe(200);
    await Promise.all(responses);
  });

  test("disconnect resolves pending and headroom without forwarding queued requests", async () => {
    const f = fixture();
    const responses = Array.from({ length: 10 }, (_, i) => f.request(i));
    await tick();
    await f.device.webSocketClose(f.socket);
    expect((await Promise.all(responses)).every((r) => r.status === 503)).toBe(true);
    expect(f.sent.length).toBe(8);
  });

  test("send failure resolves all remaining requests", async () => {
    const f = fixture();
    const responses = Array.from({ length: 10 }, (_, i) => f.request(i));
    await tick();
    f.fail();
    await f.reply(0);
    expect((await responses[0]!).status).toBe(200);
    expect((await Promise.all(responses.slice(1))).every((r) => r.status === 503)).toBe(true);
  });
});
