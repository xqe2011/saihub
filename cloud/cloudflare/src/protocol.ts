export const BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
/** Base58Check digest length range for SHA-256(publicKey)[0..19] + 4-byte checksum. */
export const DIGEST_RE = /^[123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz]{26,33}$/;

export const MAX_VERSION_LEN = 64;
export const MAX_HEADER_NAME_LEN = 64;
export const MAX_HEADER_VALUE_LEN = 512;
export const MAX_HEADERS = 16;
export const MAX_PATH_LEN = 1024;
export const MAX_BODY_BYTES = 64 * 1024;
export const PUBLIC_KEY_BYTES = 65;
export const SIGNATURE_BYTES = 64;
export const CHALLENGE_BYTES = 32;
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
};

export type ResponseMessage = {
  type: "response";
  requestId: string;
  status: number;
  headers: Record<string, string>;
  body: JsonBody;
};

export type ServerMessage = AuthRequestMessage | AuthResultMessage | RequestMessage;

export type DeviceMessage = AuthResponseMessage | ResponseMessage;

export type JsonError = {
  reason: string;
};

export function jsonError(status: number, reason: string): Response {
  return Response.json({ reason } satisfies JsonError, { status });
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
    return { type, requestId, status, headers, body };
  }
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
