import { CHALLENGE_BYTES, MAX_PUBLIC_KEY_BYTES, MAX_SIGNATURE_BYTES, PSS_SALT_LENGTH, RSA_MODULUS_BITS } from "./protocol.ts";

const BASE64URL_RE = /^[A-Za-z0-9_-]+$/;

export function bytesToBase64Url(bytes: ArrayBuffer | Uint8Array): string {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let binary = "";
  for (let i = 0; i < view.length; i += 1) {
    binary += String.fromCharCode(view[i]!);
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

export function base64UrlToBytes(value: string): Uint8Array | null {
  if (value.length === 0 || !BASE64URL_RE.test(value)) {
    return null;
  }
  const padded = value + "=".repeat((4 - (value.length % 4)) % 4);
  const base64 = padded.replace(/-/g, "+").replace(/_/g, "/");
  try {
    const binary = atob(base64);
    const out = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i += 1) {
      out[i] = binary.charCodeAt(i);
    }
    return out;
  } catch {
    return null;
  }
}

export function bytesToHex(bytes: ArrayBuffer | Uint8Array): string {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let out = "";
  for (let i = 0; i < view.length; i += 1) {
    out += view[i]!.toString(16).padStart(2, "0");
  }
  return out;
}

export async function sha256Hex(bytes: ArrayBuffer | Uint8Array): Promise<string> {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const digest = await crypto.subtle.digest("SHA-256", view);
  return bytesToHex(digest);
}

export function randomChallenge(): Uint8Array {
  const challenge = new Uint8Array(CHALLENGE_BYTES);
  crypto.getRandomValues(challenge);
  return challenge;
}

function readAsn1Length(bytes: Uint8Array, offset: number): { length: number; next: number } | null {
  if (offset >= bytes.length) {
    return null;
  }
  const first = bytes[offset]!;
  if (first < 0x80) {
    return { length: first, next: offset + 1 };
  }
  const size = first & 0x7f;
  if (size === 0 || size > 3 || offset + size >= bytes.length) {
    return null;
  }
  let length = 0;
  for (let i = 1; i <= size; i += 1) {
    length = (length << 8) | bytes[offset + i]!;
  }
  return { length, next: offset + 1 + size };
}

function skipAsn1Value(bytes: Uint8Array, offset: number): number | null {
  if (offset >= bytes.length) {
    return null;
  }
  const lengthInfo = readAsn1Length(bytes, offset + 1);
  if (!lengthInfo) {
    return null;
  }
  const end = lengthInfo.next + lengthInfo.length;
  if (end > bytes.length) {
    return null;
  }
  return end;
}

/** Returns RSA modulus bit length from a DER-encoded SubjectPublicKeyInfo. */
export function rsaModulusBitsFromSpki(spki: Uint8Array): number | null {
  if (spki.length < 20 || spki[0] !== 0x30) {
    return null;
  }
  const seqLen = readAsn1Length(spki, 1);
  if (!seqLen) {
    return null;
  }
  let offset = seqLen.next;
  // AlgorithmIdentifier SEQUENCE
  if (spki[offset] !== 0x30) {
    return null;
  }
  const afterAlg = skipAsn1Value(spki, offset);
  if (afterAlg === null || afterAlg >= spki.length || spki[afterAlg] !== 0x03) {
    return null;
  }
  offset = afterAlg;
  const bitStringLen = readAsn1Length(spki, offset + 1);
  if (!bitStringLen) {
    return null;
  }
  offset = bitStringLen.next;
  if (offset >= spki.length) {
    return null;
  }
  // unused bits byte
  offset += 1;
  if (offset >= spki.length || spki[offset] !== 0x30) {
    return null;
  }
  const rsaSeqLen = readAsn1Length(spki, offset + 1);
  if (!rsaSeqLen) {
    return null;
  }
  offset = rsaSeqLen.next;
  if (offset >= spki.length || spki[offset] !== 0x02) {
    return null;
  }
  const modulusLen = readAsn1Length(spki, offset + 1);
  if (!modulusLen) {
    return null;
  }
  offset = modulusLen.next;
  let modulusBytes = modulusLen.length;
  if (modulusBytes === 0 || offset + modulusBytes > spki.length) {
    return null;
  }
  // Strip leading zero used for positive INTEGER encoding.
  if (spki[offset] === 0x00) {
    modulusBytes -= 1;
  }
  return modulusBytes * 8;
}

export type RoutingTokenPayload = {
  devicePublicKeyDigest: string;
  grantSecret: string;
};

const GCM_IV_BYTES = 12;
const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();

async function routingAesKey(secret: string): Promise<CryptoKey> {
  const digest = await crypto.subtle.digest("SHA-256", TEXT_ENCODER.encode(secret));
  return crypto.subtle.importKey("raw", digest, { name: "AES-GCM" }, false, ["encrypt", "decrypt"]);
}

export async function sealRoutingToken(secret: string, payload: RoutingTokenPayload): Promise<string> {
  const key = await routingAesKey(secret);
  const iv = crypto.getRandomValues(new Uint8Array(GCM_IV_BYTES));
  const plaintext = TEXT_ENCODER.encode(JSON.stringify(payload));
  const ciphertext = new Uint8Array(await crypto.subtle.encrypt({ name: "AES-GCM", iv }, key, plaintext));
  const packed = new Uint8Array(iv.length + ciphertext.length);
  packed.set(iv, 0);
  packed.set(ciphertext, iv.length);
  return bytesToBase64Url(packed);
}

export async function openRoutingToken(secret: string, token: string): Promise<RoutingTokenPayload | null> {
  const packed = base64UrlToBytes(token);
  if (!packed || packed.length <= GCM_IV_BYTES) {
    return null;
  }
  const iv = packed.subarray(0, GCM_IV_BYTES);
  const ciphertext = packed.subarray(GCM_IV_BYTES);
  try {
    const key = await routingAesKey(secret);
    const plaintext = await crypto.subtle.decrypt({ name: "AES-GCM", iv }, key, ciphertext);
    const parsed = JSON.parse(TEXT_DECODER.decode(plaintext)) as unknown;
    if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
      return null;
    }
    const record = parsed as Record<string, unknown>;
    const devicePublicKeyDigest = record.devicePublicKeyDigest;
    const grantSecret = record.grantSecret;
    if (typeof devicePublicKeyDigest !== "string" || typeof grantSecret !== "string") {
      return null;
    }
    if (!/^[0-9a-f]{64}$/.test(devicePublicKeyDigest) || grantSecret.length === 0) {
      return null;
    }
    return { devicePublicKeyDigest, grantSecret };
  } catch {
    return null;
  }
}

export async function verifyDeviceAuth(args: {
  publicKeyB64: string;
  signatureB64: string;
  challenge: Uint8Array;
  expectedDigest: string;
  claimedDigest: string;
}): Promise<{ ok: true } | { ok: false; reason: string }> {
  if (args.claimedDigest !== args.expectedDigest) {
    return { ok: false, reason: "devicePublicKeyDigest mismatch" };
  }

  const publicKey = base64UrlToBytes(args.publicKeyB64);
  if (!publicKey || publicKey.length === 0 || publicKey.length > MAX_PUBLIC_KEY_BYTES) {
    return { ok: false, reason: "invalid devicePublicKey" };
  }

  const signature = base64UrlToBytes(args.signatureB64);
  if (!signature || signature.length === 0 || signature.length > MAX_SIGNATURE_BYTES) {
    return { ok: false, reason: "invalid response" };
  }

  const digest = await sha256Hex(publicKey);
  if (digest !== args.expectedDigest) {
    return { ok: false, reason: "devicePublicKey digest mismatch" };
  }

  const modulusBits = rsaModulusBitsFromSpki(publicKey);
  if (modulusBits !== RSA_MODULUS_BITS) {
    return { ok: false, reason: "unsupported public key size" };
  }

  let key: CryptoKey;
  try {
    key = await crypto.subtle.importKey("spki", publicKey, { name: "RSA-PSS", hash: "SHA-256" }, false, ["verify"]);
  } catch {
    return { ok: false, reason: "invalid devicePublicKey" };
  }

  let valid = false;
  try {
    valid = await crypto.subtle.verify(
      { name: "RSA-PSS", saltLength: PSS_SALT_LENGTH },
      key,
      signature,
      args.challenge,
    );
  } catch {
    return { ok: false, reason: "invalid response" };
  }

  if (!valid) {
    return { ok: false, reason: "authentication failed" };
  }
  return { ok: true };
}
