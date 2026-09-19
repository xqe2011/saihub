import {
  AUTH_DOMAIN_PREFIX,
  BASE58_ALPHABET,
  CHALLENGE_BYTES,
  DIGEST_CHECKSUM_BYTES,
  DIGEST_PAYLOAD_BYTES,
  PUBLIC_KEY_BYTES,
  SIGNATURE_BYTES,
  isDigest,
} from "./protocol.ts";

const BASE64URL_RE = /^[A-Za-z0-9_-]+$/;
const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();
const AUTH_DOMAIN_BYTES = TEXT_ENCODER.encode(AUTH_DOMAIN_PREFIX);

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

export async function sha256(bytes: ArrayBuffer | Uint8Array): Promise<Uint8Array> {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  return new Uint8Array(await crypto.subtle.digest("SHA-256", view));
}

export function randomChallenge(): Uint8Array {
  const challenge = new Uint8Array(CHALLENGE_BYTES);
  crypto.getRandomValues(challenge);
  return challenge;
}

export function authSignedMessage(challenge: Uint8Array): Uint8Array {
  const out = new Uint8Array(AUTH_DOMAIN_BYTES.length + challenge.length);
  out.set(AUTH_DOMAIN_BYTES, 0);
  out.set(challenge, AUTH_DOMAIN_BYTES.length);
  return out;
}

/** Base58Check-encode a fixed-length payload (matches firmware Cloud_Base58Check). */
export async function base58CheckEncode(payload: Uint8Array): Promise<string | null> {
  if (payload.length !== DIGEST_PAYLOAD_BYTES) {
    return null;
  }
  const checksumInput = await sha256(payload);
  const checksum = await sha256(checksumInput);
  const encoded = new Uint8Array(DIGEST_PAYLOAD_BYTES + DIGEST_CHECKSUM_BYTES);
  encoded.set(payload, 0);
  encoded.set(checksum.subarray(0, DIGEST_CHECKSUM_BYTES), DIGEST_PAYLOAD_BYTES);

  let leadingZeroes = 0;
  while (leadingZeroes < encoded.length && encoded[leadingZeroes] === 0) {
    leadingZeroes += 1;
  }

  const digits: number[] = [];
  for (let i = leadingZeroes; i < encoded.length; i += 1) {
    let carry = encoded[i]!;
    for (let j = 0; j < digits.length; j += 1) {
      const value = digits[j]! * 256 + carry;
      digits[j] = value % 58;
      carry = (value / 58) | 0;
    }
    while (carry > 0) {
      digits.push(carry % 58);
      carry = (carry / 58) | 0;
    }
  }

  let out = "";
  for (let i = 0; i < leadingZeroes; i += 1) {
    out += "1";
  }
  for (let i = digits.length - 1; i >= 0; i -= 1) {
    out += BASE58_ALPHABET[digits[i]!]!;
  }
  return out;
}

export async function publicKeyDigest(publicKey: Uint8Array): Promise<string | null> {
  if (publicKey.length !== PUBLIC_KEY_BYTES || publicKey[0] !== 0x04) {
    return null;
  }
  const hash = await sha256(publicKey);
  return base58CheckEncode(hash.subarray(0, DIGEST_PAYLOAD_BYTES));
}

export type RoutingTokenPayload = {
  devicePublicKeyDigest: string;
  grantSecret: string;
};

const GCM_IV_BYTES = 12;

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
    if (!isDigest(devicePublicKeyDigest) || grantSecret.length === 0) {
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
  if (!publicKey || publicKey.length !== PUBLIC_KEY_BYTES || publicKey[0] !== 0x04) {
    return { ok: false, reason: "invalid devicePublicKey" };
  }

  const signature = base64UrlToBytes(args.signatureB64);
  if (!signature || signature.length !== SIGNATURE_BYTES) {
    return { ok: false, reason: "invalid response" };
  }

  const digest = await publicKeyDigest(publicKey);
  if (digest === null || digest !== args.expectedDigest) {
    return { ok: false, reason: "devicePublicKey digest mismatch" };
  }

  let key: CryptoKey;
  try {
    key = await crypto.subtle.importKey("raw", publicKey, { name: "ECDSA", namedCurve: "P-256" }, false, ["verify"]);
  } catch {
    return { ok: false, reason: "invalid devicePublicKey" };
  }

  let valid = false;
  try {
    valid = await crypto.subtle.verify(
      { name: "ECDSA", hash: "SHA-256" },
      key,
      signature,
      authSignedMessage(args.challenge),
    );
  } catch {
    return { ok: false, reason: "invalid response" };
  }

  if (!valid) {
    return { ok: false, reason: "authentication failed" };
  }
  return { ok: true };
}
