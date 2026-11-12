export const BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
/** Base58Check digest length range for SHA-256(publicKey)[0..19] + 4-byte checksum. */
export const DIGEST_RE = /^[123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]{26,33}$/;

export const MAX_VERSION_LEN = 64;
export const MAX_HEADER_NAME_LEN = 64;
export const MAX_HEADER_VALUE_LEN = 512;
export const MAX_HEADERS = 16;
export const MAX_PATH_LEN = 1024;
export const MAX_BODY_BYTES = 64 * 1024;
export const MAX_PAIRING_NAME_LEN = 32;
export const PUBLIC_KEY_BYTES = 65;
export const SIGNATURE_BYTES = 64;
export const CHALLENGE_BYTES = 32;
/** Client bytes held by the relay while the device has no send credit. Also the max open `byteCredits` / ack `length`. */
export const TCP_STREAM_CLIENT_BUFFER_MAX = 1024 * 1024;
export const DIGEST_PAYLOAD_BYTES = 20;
export const DIGEST_CHECKSUM_BYTES = 4;
/** Domain separation prefix including trailing NUL: UTF8("saihub/cloud-auth/v1") || 0x00 */
export const AUTH_DOMAIN_PREFIX = "saihub/cloud-auth/v1\0";

export const FORWARDED_HEADERS = ["content-type", "accept", "x-lock-id", "mcp-protocol-version", "mcp-session-id", "origin"] as const;

export type JsonBody = Record<string, unknown> | unknown[] | null;

export function isJsonContentType(value: string | null): boolean {
  return value?.split(";", 1)[0]?.trim().toLowerCase() === "application/json";
}

export function unsupportedContentType(value: string | null, status = 415): Response {
  return jsonError(status, `cloud relay not support ${value || "missing content-type"} currently, use application/json instead`);
}

export type AuthRequestMessage = {
  type: "authRequest";
  challenge: string;
};

export type AuthResponseMessage = {
  type: "authResponse";
  devicePublicKey: string;
  devicePublicKeyDigest: string;
  version: string;
  response: string;
};

export type AuthResultMessage = {
  type: "authResult";
  success: boolean;
  reason?: string;
};

export type RequestMessage = {
  type: "request";
  requestId: string;
  method: string;
  path: string;
  headers: Record<string, string>;
  body: Record<string, unknown> | null;
  grantSecret?: string;
};

export type PairingSessionRequestMessage = {
  type: "pairingSessionRequest";
  name: string;
};

export type PairingSessionResponseMessage =
  | { type: "pairingSessionResponse"; success: true; sessionToken: string; expiredAt: number }
  | { type: "pairingSessionResponse"; success: false; reason: string };

export type PairingSessionTokenResponseMessage =
  | { type: "pairingSessionTokenResponse"; success: true; grantSecret: string }
  | { type: "pairingSessionTokenResponse"; success: false; reason: string };

export const WEBSOCKET_TOKEN_PLACEHOLDER = "$WEBSOCKET_TOKEN$";
export const TCP_STREAM_DAP_PATH = "/tcp-stream/dap";
export const TCP_STREAM_RFC2217_PATH = "/tcp-stream/rfc2217";

export type AddWebsocketToken = {
  jsonPathes: string[];
  websocketPath: string;
};

export type ResponseMessage = {
  type: "response";
  requestId: string;
  status: number;
  headers: Record<string, string>;
  body: JsonBody;
  cloud?: { addWebsocketToken: AddWebsocketToken };
};

export type TcpStreamOpenRequestMessage = {
  type: "tcpStreamOpenRequest";
  requestId: string;
  service: string;
  grantSecret: string;
};

export type TcpStreamOpenResponseMessage =
  | { type: "tcpStreamOpenResponse"; requestId: string; success: true; channelId: number; byteCredits: number }
  | { type: "tcpStreamOpenResponse"; requestId: string; success: false; reason: string };

export type TcpStreamAckMessage = {
  type: "tcpStreamAck";
  channelId: number;
  length: number;
};

export type TcpStreamCloseRequestMessage = {
  type: "tcpStreamCloseRequest";
  requestId: string;
  channelId: number;
  grantSecret: string;
};

export type TcpStreamCloseResponseMessage =
  | { type: "tcpStreamCloseResponse"; requestId: string; success: true }
  | { type: "tcpStreamCloseResponse"; requestId: string; success: false; reason: string };

export type ServerMessage =
  | AuthRequestMessage
  | AuthResultMessage
  | RequestMessage
  | PairingSessionRequestMessage
  | TcpStreamOpenRequestMessage
  | TcpStreamCloseRequestMessage;

export type DeviceMessage =
  | AuthResponseMessage
  | ResponseMessage
  | PairingSessionResponseMessage
  | PairingSessionTokenResponseMessage
  | TcpStreamOpenResponseMessage
  | TcpStreamCloseResponseMessage
  | TcpStreamAckMessage;

export function isTcpStreamPath(path: string): boolean {
  return path === TCP_STREAM_DAP_PATH || path === TCP_STREAM_RFC2217_PATH;
}

export function tcpStreamService(path: string): "dap" | "rfc2217" | null {
  if (path === TCP_STREAM_DAP_PATH) return "dap";
  if (path === TCP_STREAM_RFC2217_PATH) return "rfc2217";
  return null;
}

export type JsonError = {
  reason: string;
};

export function jsonError(status: number, reason: string): Response {
  return Response.json({ reason } satisfies JsonError, { status });
}

export function pairingErrorStatus(reason: string): number {
  if (reason === "a pairing session is already active") return 409;
  if (reason === "grant secret limit reached (16)") return 422;
  if (reason === "session token is invalid or expired") return 401;
  return 400;
}

export function isDigest(value: string): boolean {
  return DIGEST_RE.test(value);
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function asString(value: unknown): string | null {
  return typeof value === "string" ? value : null;
}

function parseHeaders(value: unknown): Record<string, string> | null {
  if (!isRecord(value)) {
    return null;
  }
  const entries = Object.entries(value);
  if (entries.length > MAX_HEADERS) {
    return null;
  }
  const headers: Record<string, string> = {};
  for (const [name, headerValue] of entries) {
    if (
      typeof name !== "string" ||
      name.length === 0 ||
      name.length > MAX_HEADER_NAME_LEN ||
      typeof headerValue !== "string" ||
      headerValue.length > MAX_HEADER_VALUE_LEN
    ) {
      return null;
    }
    headers[name] = headerValue;
  }
  return headers;
}

export function parseDeviceMessage(raw: string): DeviceMessage | null {
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return null;
  }
  if (!isRecord(parsed)) {
    return null;
  }
  const type = asString(parsed.type);
  if (type === "authResponse") {
    const devicePublicKey = asString(parsed.devicePublicKey);
    const devicePublicKeyDigest = asString(parsed.devicePublicKeyDigest);
    const version = asString(parsed.version);
    const response = asString(parsed.response);
    if (
      devicePublicKey === null ||
      devicePublicKeyDigest === null ||
      version === null ||
      response === null ||
      version.length === 0 ||
      version.length > MAX_VERSION_LEN ||
      !isDigest(devicePublicKeyDigest)
    ) {
      return null;
    }
    return { type, devicePublicKey, devicePublicKeyDigest, version, response };
  }
  if (type === "response") {
    const requestId = asString(parsed.requestId);
    const status = parsed.status;
    const headers = parseHeaders(parsed.headers);
    const body = parsed.body;
    if (
      requestId === null ||
      requestId.length === 0 ||
      typeof status !== "number" ||
      !Number.isInteger(status) ||
      status < 200 ||
      status > 599 ||
      headers === null ||
      !(body === null || isRecord(body) || Array.isArray(body))
    ) {
      return null;
    }
    const cloud = parseAddWebsocketToken(parsed.cloud);
    return cloud ? { type, requestId, status, headers, body, cloud } : { type, requestId, status, headers, body };
  }
  if (type === "tcpStreamOpenResponse" || type === "tcpStreamCloseResponse") {
    return parseTcpStreamResponse(type, parsed);
  }
  if (type === "tcpStreamAck") return parseTcpStreamAck(parsed);
  if (type === "pairingSessionResponse" || type === "pairingSessionTokenResponse") {
    const success = parsed.success;
    if (typeof success !== "boolean") {
      return null;
    }
    if (success) {
      if (type === "pairingSessionResponse") {
        const sessionToken = asString(parsed.sessionToken);
        const expiredAt = parsed.expiredAt;
        if (sessionToken === null || sessionToken.length === 0 || typeof expiredAt !== "number" || !Number.isFinite(expiredAt)) {
          return null;
        }
        return { type, success, sessionToken, expiredAt };
      }
      const grantSecret = asString(parsed.grantSecret);
      if (grantSecret === null || grantSecret.length === 0) {
        return null;
      }
      return { type, success, grantSecret };
    }
    const reason = asString(parsed.reason);
    if (reason === null || reason.length === 0) {
      return null;
    }
    return { type, success, reason };
  }
  return null;
}

function parseAddWebsocketToken(value: unknown): { addWebsocketToken: AddWebsocketToken } | undefined {
  if (!isRecord(value)) return undefined;
  const spec = value.addWebsocketToken;
  if (!isRecord(spec)) return undefined;
  const websocketPath = asString(spec.websocketPath);
  if (!websocketPath || websocketPath.length > MAX_PATH_LEN || !Array.isArray(spec.jsonPathes)) return undefined;
  const jsonPathes: string[] = [];
  for (const item of spec.jsonPathes) {
    if (typeof item !== "string" || !item.startsWith("/") || item.length > MAX_PATH_LEN) return undefined;
    jsonPathes.push(item);
    if (jsonPathes.length >= 8) break;
  }
  if (jsonPathes.length === 0) return undefined;
  return { addWebsocketToken: { jsonPathes, websocketPath } };
}

function parseChannelId(value: unknown): number | null {
  if (typeof value !== "number" || !Number.isInteger(value) || value < 0 || value > 255) return null;
  return value;
}

function parseByteCredits(value: unknown): number | null {
  if (typeof value !== "number" || !Number.isInteger(value) || value < 1 || value > TCP_STREAM_CLIENT_BUFFER_MAX) return null;
  return value;
}

function parseTcpStreamAck(parsed: Record<string, unknown>): TcpStreamAckMessage | null {
  const channelId = parseChannelId(parsed.channelId);
  const length = parsed.length;
  if (channelId === null || typeof length !== "number" || !Number.isInteger(length) || length < 1 ||
    length > TCP_STREAM_CLIENT_BUFFER_MAX) {
    return null;
  }
  return { type: "tcpStreamAck", channelId, length };
}

function parseTcpStreamResponse(
  type: "tcpStreamOpenResponse" | "tcpStreamCloseResponse",
  parsed: Record<string, unknown>,
): TcpStreamOpenResponseMessage | TcpStreamCloseResponseMessage | null {
  const requestId = asString(parsed.requestId);
  if (requestId === null || requestId.length === 0 || typeof parsed.success !== "boolean") return null;
  if (parsed.success) {
    if (type === "tcpStreamCloseResponse") return { type, requestId, success: true };
    const channelId = parseChannelId(parsed.channelId);
    const byteCredits = parseByteCredits(parsed.byteCredits);
    if (channelId === null || byteCredits === null) return null;
    return { type, requestId, success: true, channelId, byteCredits };
  }
  const reason = asString(parsed.reason);
  if (reason === null || reason.length === 0) return null;
  return { type, requestId, success: false, reason };
}

function unescapeJsonPointer(token: string): string {
  return token.replaceAll("~1", "/").replaceAll("~0", "~");
}

/** Replace the first `$WEBSOCKET_TOKEN$` in each listed JSON pointer. One replacement per string. */
export function injectWebsocketToken(body: JsonBody, pointers: readonly string[], token: string): void {
  if (body === null || typeof body !== "object") return;
  for (const pointer of pointers) {
    if (!pointer.startsWith("/")) continue;
    const parts = pointer.split("/").slice(1).map(unescapeJsonPointer);
    let parent: unknown = body;
    let ok = true;
    for (let i = 0; i < parts.length - 1; i += 1) {
      parent = childAt(parent, parts[i]!);
      if (parent === null || typeof parent !== "object") {
        ok = false;
        break;
      }
    }
    if (!ok) continue;
    const last = parts[parts.length - 1];
    if (last === undefined || parent === null || typeof parent !== "object") continue;
    if (Array.isArray(parent)) {
      const index = Number(last);
      if (!Number.isInteger(index) || index < 0 || index >= parent.length) continue;
      const value = parent[index];
      if (typeof value === "string") parent[index] = value.replace(WEBSOCKET_TOKEN_PLACEHOLDER, token);
    } else if (isRecord(parent) && Object.prototype.hasOwnProperty.call(parent, last)) {
      const record = parent;
      const value = record[last];
      if (typeof value === "string") record[last] = value.replace(WEBSOCKET_TOKEN_PLACEHOLDER, token);
    }
  }
}

function childAt(parent: unknown, key: string): unknown {
  if (Array.isArray(parent)) {
    const index = Number(key);
    if (!Number.isInteger(index) || index < 0 || index >= parent.length) return null;
    return parent[index];
  }
  if (isRecord(parent) && Object.prototype.hasOwnProperty.call(parent, key)) return parent[key];
  return null;
}

export function selectForwardHeaders(request: Request): Record<string, string> {
  const headers: Record<string, string> = {};
  for (const name of FORWARDED_HEADERS) {
    const value = request.headers.get(name);
    if (value !== null && value.length <= MAX_HEADER_VALUE_LEN) {
      headers[name] = value;
    }
  }
  return headers;
}
