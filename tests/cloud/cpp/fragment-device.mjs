// Pure Node.js fragmented WebSocket device using node:net (no external ws dependency required).
import { createHash, randomBytes } from "node:crypto";
import net from "node:net";

const { url, digest, publicKeyB64, privateKey } = JSON.parse(process.argv[2]);
const parsedUrl = new URL(url);
const key = await crypto.subtle.importKey("jwk", privateKey, { name: "ECDSA", namedCurve: "P-256" }, false, ["sign"]);

function maskFrame(opcode, payload, fin = true) {
  const buf = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  const mask = randomBytes(4);
  let header;
  const first = (fin ? 0x80 : 0x00) | (opcode & 0x0f);
  if (buf.length < 126) {
    header = Buffer.from([first, 0x80 | buf.length]);
  } else if (buf.length < 65536) {
    header = Buffer.alloc(4);
    header[0] = first;
    header[1] = 0x80 | 126;
    header.writeUInt16BE(buf.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = first;
    header[1] = 0x80 | 127;
    header.writeBigUInt64BE(BigInt(buf.length), 2);
  }
  const masked = Buffer.allocUnsafe(buf.length);
  for (let i = 0; i < buf.length; i++) masked[i] = buf[i] ^ mask[i & 3];
  return Buffer.concat([header, mask, masked]);
}

const socket = net.connect(Number(parsedUrl.port), parsedUrl.hostname, () => {
  const wsKey = randomBytes(16).toString("base64");
  socket.write(
    `GET /cloud/device/${digest} HTTP/1.1\r\nHost: ${parsedUrl.host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${wsKey}\r\nSec-WebSocket-Version: 13\r\n\r\n`,
  );
});

let rx = Buffer.alloc(0);
let upgraded = false;
let fragOp = 0;
let fragBuf = [];

socket.on("data", async (chunk) => {
  rx = Buffer.concat([rx, chunk]);
  if (!upgraded) {
    const idx = rx.indexOf("\r\n\r\n");
    if (idx === -1) return;
    rx = rx.subarray(idx + 4);
    upgraded = true;
  }
  while (rx.length >= 2) {
    const b0 = rx[0];
    const b1 = rx[1];
    const fin = (b0 & 0x80) !== 0;
    const op = b0 & 0x0f;
    let plen = b1 & 0x7f;
    let hdr = 2;
    if (plen === 126) {
      if (rx.length < 4) return;
      plen = rx.readUInt16BE(2);
      hdr = 4;
    } else if (plen === 127) {
      if (rx.length < 10) return;
      plen = Number(rx.readBigUInt64BE(2));
      hdr = 10;
    }
    if (rx.length < hdr + plen) return;
    const body = rx.subarray(hdr, hdr + plen);
    rx = rx.subarray(hdr + plen);

    if (op === 8) {
      socket.end();
      return;
    }
    if (op !== 0) {
      fragOp = op;
      fragBuf = [body];
    } else {
      fragBuf.push(body);
    }
    if (!fin) continue;
    const full = Buffer.concat(fragBuf);
    fragBuf = [];
    if (fragOp !== 1) continue;

    const message = JSON.parse(full.toString("utf8"));
    if (message.type === "authRequest") {
      const signed = Buffer.concat([Buffer.from("saihub/cloud-auth/v1\0"), Buffer.from(message.challenge, "base64url")]);
      const signature = await crypto.subtle.sign({ name: "ECDSA", hash: "SHA-256" }, key, signed);
      const reply = JSON.stringify({
        type: "authResponse",
        devicePublicKey: publicKeyB64,
        devicePublicKeyDigest: digest,
        version: "fragment-test",
        response: Buffer.from(signature).toString("base64url"),
      });
      socket.write(maskFrame(1, reply, true));
    } else if (message.type === "authResult") {
      if (!message.success) process.exit(1);
      console.log("ready");
    } else if (message.type === "request") {
      if (message.body !== null || message.headers["content-type"] !== undefined) process.exit(1);
      const prefix = JSON.stringify({ type: "response", requestId: message.requestId, status: 200, headers: { "content-type": "application/json" } });
      socket.write(maskFrame(1, prefix.slice(0, -1) + ',"body":', false));
      socket.write(maskFrame(0, '{"events":[', false));
      for (let i = 0; i < 2000; i++) {
        socket.write(maskFrame(0, (i ? "," : "") + JSON.stringify({ pin: 1, level: i % 2, time: i, label: "测试" }), false));
      }
      socket.write(maskFrame(0, "]}}", true));
    }
  }
});
socket.on("error", (err) => { console.error(err); process.exit(1); });
