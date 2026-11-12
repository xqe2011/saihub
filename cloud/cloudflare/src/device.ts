import { tryServeCached } from "./cache.ts";
import { DEFAULT_AUTH_TIMEOUT_MS, DEFAULT_REQUEST_TIMEOUT_MS, parseTimeoutMs, type Env } from "./env.ts";
import { base64UrlToBytes, bytesToBase64Url, randomChallenge, sealWebsocketToken, verifyDeviceAuth } from "./crypto.ts";
import { injectWebsocketToken, isJsonContentType, isTcpStreamPath, jsonError, MAX_BODY_BYTES, MAX_PATH_LEN, parseDeviceMessage, pairingErrorStatus, selectForwardHeaders, TCP_STREAM_CLIENT_BUFFER_MAX, unsupportedContentType, type AuthRequestMessage, type AuthResultMessage, type JsonBody, type PairingSessionRequestMessage, type PairingSessionResponseMessage, type PairingSessionTokenResponseMessage, type RequestMessage, type TcpStreamAckMessage, type TcpStreamCloseRequestMessage, type TcpStreamOpenRequestMessage } from "./protocol.ts";

const MAX_PENDING_REQUESTS = 8;
const MAX_PAIRING_WAITERS = 8;
const HEARTBEAT_TIMEOUT_MS = 33_000;
const PAIRING_STORAGE_KEY = "pairing";
const tcpStreamStorageKey = (channelId: number) => `tcpStream:${channelId}`;

type HeadroomRequest = {
  message: RequestMessage;
  resolve: (response: Response) => void;
};

type PendingRequest = {
  resolve: (response: Response) => void;
  timer: ReturnType<typeof setTimeout>;
  grantSecret?: string;
};

type StreamWaiter = {
  resolve: (result: { ok: true; channelId: number; byteCredits: number } | { ok: false; reason: string }) => void;
  timer: ReturnType<typeof setTimeout>;
  service: string;
};

type StreamBuffer = {
  byteCredits: number;
  capacity: number;
  pending: Uint8Array;
};

type DeviceWebsocketAttachment = {
  kind: "deviceWebsocket";
  connectedAt: number;
  digest: string;
  authenticated: boolean;
  challenge?: string;
  version?: string;
};

type TcpStreamWebsocketAttachment = {
  kind: "tcpStreamWebsocket";
  channelId: number;
  grantSecret: string;
  service: string;
  digest: string;
  connectedAt: number;
};

function readWebsocketAttachment(value: unknown): DeviceWebsocketAttachment | TcpStreamWebsocketAttachment | null {
  if (typeof value !== "object" || value === null) return null;
  const rec = value as Record<string, unknown>;
  if (rec.kind === "tcpStreamWebsocket") {
    if (typeof rec.channelId !== "number" || rec.channelId < 0 || rec.channelId > 255 ||
      typeof rec.grantSecret !== "string" || typeof rec.service !== "string" || typeof rec.digest !== "string" ||
      typeof rec.connectedAt !== "number") {
      return null;
    }
    return {
      kind: "tcpStreamWebsocket",
      channelId: rec.channelId,
      grantSecret: rec.grantSecret,
      service: rec.service,
      digest: rec.digest,
      connectedAt: rec.connectedAt,
    };
  }
  if ((rec.kind !== undefined && rec.kind !== "deviceWebsocket") || typeof rec.connectedAt !== "number") return null;
  return {
    kind: "deviceWebsocket",
    connectedAt: rec.connectedAt,
    digest: typeof rec.digest === "string" ? rec.digest : "",
    authenticated: rec.authenticated === true,
    challenge: typeof rec.challenge === "string" ? rec.challenge : undefined,
    version: typeof rec.version === "string" ? rec.version : undefined,
  };
}

function readStoredStream(value: unknown): StreamBuffer | null {
  if (typeof value !== "object" || value === null) return null;
  const rec = value as Record<string, unknown>;
  if (typeof rec.byteCredits !== "number" || !Number.isInteger(rec.byteCredits) || rec.byteCredits < 0) return null;
  if (typeof rec.capacity !== "number" || !Number.isInteger(rec.capacity) || rec.capacity < 1) return null;
  if (rec.byteCredits > rec.capacity) return null;
  let pending: Uint8Array;
  if (rec.pending instanceof ArrayBuffer) pending = new Uint8Array(rec.pending);
  else if (ArrayBuffer.isView(rec.pending)) {
    const view = rec.pending as ArrayBufferView;
    pending = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
  } else return null;
  if (pending.byteLength > TCP_STREAM_CLIENT_BUFFER_MAX) return null;
  return { byteCredits: rec.byteCredits, capacity: rec.capacity, pending };
}

function appendPending(pending: Uint8Array, bytes: Uint8Array): Uint8Array {
  if (bytes.length === 0) return pending;
  if (pending.length === 0) return bytes;
  const out = new Uint8Array(pending.length + bytes.length);
  out.set(pending);
  out.set(bytes, pending.length);
  return out;
}

type PairingState = {
  sessionToken: string;
  expiredAt: number;
  grantSecret?: string;
};

type PairingWaiter = {
  resolve: (response: Response) => void;
  timer: ReturnType<typeof setTimeout>;
};

function isPairingState(value: unknown): value is PairingState {
  if (typeof value !== "object" || value === null) {
    return false;
  }
  const rec = value as Record<string, unknown>;
  return typeof rec.sessionToken === "string" && rec.sessionToken.length > 0 &&
    typeof rec.expiredAt === "number" && Number.isFinite(rec.expiredAt) &&
    (rec.grantSecret === undefined || (typeof rec.grantSecret === "string" && rec.grantSecret.length > 0));
}

type DeviceWebsocketState = {
  authenticated: boolean;
  challenge: Uint8Array | null;
};

export class Device implements DurableObject {
  readonly #ctx: DurableObjectState;
  readonly #env: Env;
  readonly #pending = new Map<string, PendingRequest>();
  readonly #streamPending = new Map<string, StreamWaiter>();
  readonly #streamBuffers = new Map<number, StreamBuffer>();
  readonly #headroom: HeadroomRequest[] = [];
  readonly #discarded = new WeakSet<WebSocket>();
  readonly #pairingTokenWaiters: PairingWaiter[] = [];
  #seq = 0;
  #deviceWebsocket: WebSocket | null = null;
  #deviceWebsocketState: DeviceWebsocketState | null = null;
  #pairing: PairingState | null = null;
  #pairingLoaded = false;
  #pairingSessionWaiter: PendingRequest | null = null;

  constructor(ctx: DurableObjectState, env: Env) {
    this.#ctx = ctx;
    this.#env = env;
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
  }

  async fetch(request: Request): Promise<Response> {
    await this.#validateDeviceWebsocket();
    const url = new URL(request.url);
    if (url.pathname === "/pairing/session" || url.pathname === "/pairing/token") {
      await this.#loadPairing();
    }

    if (url.pathname === "/websocket") {
      const digest = url.searchParams.get("digest") ?? "";
      return this.#handleDeviceWebsocket(request, digest);
    }
    if (url.pathname === "/proxy") {
      return this.#handleProxy(request);
    }
    if (url.pathname === "/pairing/session") {
      return this.#handlePairingSession(request);
    }
    if (url.pathname === "/pairing/token") {
      return this.#handlePairingToken(request);
    }
    if (url.pathname === "/tcp-stream") {
      return this.#handleTcpStreamWebsocket(request);
    }
    if (url.pathname === "/online") {
      return this.#handleOnline();
    }
    if (url.pathname === "/disconnect") {
      return this.#handleDisconnect();
    }
    return jsonError(404, "not found");
  }

  async alarm(): Promise<void> {
    await this.#validateDeviceWebsocket();
    if (this.#deviceWebsocket && this.#deviceWebsocketState && !this.#deviceWebsocketState.authenticated) {
      await this.#failAuth(this.#deviceWebsocket, "authentication timeout");
      return;
    }
    await this.#clearAuthAlarm();
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    await this.#validateDeviceWebsocket();
    const attachment = readWebsocketAttachment(ws.deserializeAttachment());
    if (attachment?.kind === "tcpStreamWebsocket") {
      await this.#onTcpStreamWebsocketMessage(ws, message, attachment);
      return;
    }
    if (attachment?.kind !== "deviceWebsocket" || ws !== this.#deviceWebsocket) {
      try { ws.close(1008, "stale connection"); } catch { /* ignore */ }
      return;
    }
    await this.#onDeviceWebsocketMessage(ws, message);
  }

  async #onDeviceWebsocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    if (typeof message !== "string") {
      if (!this.#deviceWebsocketState?.authenticated) {
        await this.#failAuth(ws, "expected text frame");
        return;
      }
      this.#forwardDeviceBinary(message);
      return;
    }

    const parsed = parseDeviceMessage(message);
    if (!parsed) {
      if (!this.#deviceWebsocketState?.authenticated) {
        await this.#failAuth(ws, "malformed auth response");
      }
      return;
    }

    if (parsed.type === "authResponse") {
      await this.#handleAuthResponse(ws, parsed);
      return;
    }

    if (!this.#deviceWebsocketState?.authenticated) {
      await this.#failAuth(ws, "not authenticated");
      return;
    }

    if (parsed.type === "pairingSessionResponse") {
      await this.#loadPairing();
      await this.#onPairingSessionResponse(parsed);
      return;
    }
    if (parsed.type === "pairingSessionTokenResponse") {
      await this.#loadPairing();
      await this.#onPairingTokenResponse(parsed);
      return;
    }
    if (parsed.type === "tcpStreamOpenResponse") {
      this.#onTcpStreamOpenResponse(parsed);
      return;
    }
    if (parsed.type === "tcpStreamCloseResponse") {
      return;
    }
    if (parsed.type === "tcpStreamAck") {
      await this.#onTcpStreamAck(parsed);
      return;
    }

    if (parsed.type !== "response") {
      return;
    }

    const pending = this.#pending.get(parsed.requestId);
    if (!pending) {
      return;
    }

    await this.#injectWebsocketToken(parsed.requestId, parsed);
    const bodyText = parsed.body === null ? null : JSON.stringify(parsed.body);
    if (bodyText !== null && new TextEncoder().encode(bodyText).byteLength > 1024 * 1024) {
      this.#completePending(parsed.requestId, jsonError(502, "invalid device response body"));
      return;
    }

    const headers = new Headers();
    for (const [name, value] of Object.entries(parsed.headers)) {
      try {
        headers.set(name, value);
      } catch {
        this.#completePending(parsed.requestId, jsonError(502, "invalid device response headers"));
        return;
      }
    }

    if (bodyText !== null && !isJsonContentType(headers.get("content-type"))) {
      this.#completePending(parsed.requestId, unsupportedContentType(headers.get("content-type"), 502));
      return;
    }
    if ([204, 205, 304].includes(parsed.status) && bodyText !== null) {
      this.#completePending(parsed.requestId, jsonError(502, "invalid device response body"));
      return;
    }
    headers.delete("content-length");
    headers.delete("transfer-encoding");
    this.#completePending(parsed.requestId, new Response(bodyText, { status: parsed.status, headers }));
  }

  async webSocketClose(ws: WebSocket): Promise<void> {
    const attachment = readWebsocketAttachment(ws.deserializeAttachment());
    if (attachment?.kind === "tcpStreamWebsocket") {
      this.#discarded.add(ws);
      await this.#deleteStream(attachment.channelId);
      this.#sendTcpStreamClose(attachment);
      return;
    }
    await this.#validateDeviceWebsocket();
    if (this.#deviceWebsocket !== null && ws !== this.#deviceWebsocket) return;
    if (this.#deviceWebsocket === null) this.#restoreDeviceWebsocket();
    if (ws !== this.#deviceWebsocket) return;
    await this.#clearAuthAlarm();
    await this.#clearDeviceWebsocket();
    this.#rejectAllPending("device offline");
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    await this.webSocketClose(ws);
  }

  async #validateDeviceWebsocket(): Promise<void> {
    this.#restoreDeviceWebsocket();
    this.#evictStaleTcpStreamWebsockets();
    const ws = this.#deviceWebsocket;
    if (!ws) return;
    const attachment = readWebsocketAttachment(ws.deserializeAttachment());
    const connectedAt = attachment?.kind === "deviceWebsocket" ? attachment.connectedAt : Date.now();
    const lastSeen = this.#ctx.getWebSocketAutoResponseTimestamp(ws)?.getTime() ?? connectedAt;
    if (Date.now() - lastSeen <= HEARTBEAT_TIMEOUT_MS) return;
    this.#discarded.add(ws);
    this.#deviceWebsocket = null;
    this.#deviceWebsocketState = null;
    this.#rejectAllPending("device offline");
    try {
      ws.close(1001, "heartbeat timeout");
    } catch {
      // The edge may already have closed the socket while the object slept.
    }
    await this.#clearAuthAlarm();
  }

  #restoreDeviceWebsocket(): void {
    if (this.#deviceWebsocket) return;
    const sockets = this.#ctx.getWebSockets().filter((ws) => {
      if (this.#discarded.has(ws) || ws.readyState !== WebSocket.OPEN) return false;
      return readWebsocketAttachment(ws.deserializeAttachment())?.kind === "deviceWebsocket";
    });
    const socket = sockets[sockets.length - 1];
    if (!socket) return;
    const attachment = readWebsocketAttachment(socket.deserializeAttachment());
    if (attachment?.kind !== "deviceWebsocket") return;
    this.#deviceWebsocket = socket;
    this.#deviceWebsocketState = {
      authenticated: attachment?.authenticated === true,
      challenge: attachment?.challenge ? base64UrlToBytes(attachment.challenge) : null,
    };
  }

  #handleOnline(): Response {
    const online = this.#deviceWebsocket !== null && this.#deviceWebsocketState?.authenticated === true;
    return Response.json({ online }, { headers: { "cache-control": "no-store" } });
  }

  async #handleDisconnect(): Promise<Response> {
    await this.#disconnectSocket(1008, "removed from whitelist");
    return new Response(null, { status: 204 });
  }

  #handleDeviceWebsocket(request: Request, digest: string): Response {
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
      return jsonError(426, "websocket upgrade required");
    }
    if (this.#deviceWebsocket !== null) {
      return jsonError(409, "device already connected");
    }

    const pair = new WebSocketPair();
    const client = pair[0];
    const deviceWebsocket = pair[1];
    this.#ctx.acceptWebSocket(deviceWebsocket);

    const challenge = randomChallenge();
    const challengeB64 = bytesToBase64Url(challenge);
    const authTimeout = parseTimeoutMs(this.#env.AUTH_TIMEOUT_MS, DEFAULT_AUTH_TIMEOUT_MS);
    deviceWebsocket.serializeAttachment({
      kind: "deviceWebsocket", digest, connectedAt: Date.now(), authenticated: false, challenge: challengeB64,
    } satisfies DeviceWebsocketAttachment);
    void this.#setAuthAlarm(Date.now() + authTimeout);

    this.#deviceWebsocket = deviceWebsocket;
    this.#deviceWebsocketState = { authenticated: false, challenge };

    const authRequest: AuthRequestMessage = { type: "authRequest", challenge: challengeB64 };
    deviceWebsocket.send(JSON.stringify(authRequest));

    return new Response(null, { status: 101, webSocket: client });
  }

  async #handleAuthResponse(
    ws: WebSocket,
    message: {
      devicePublicKey: string;
      devicePublicKeyDigest: string;
      version: string;
      response: string;
    },
  ): Promise<void> {
    const state = this.#deviceWebsocketState;
    if (!state || ws !== this.#deviceWebsocket) {
      return;
    }
    if (state.authenticated) {
      return;
    }

    const attachment = readWebsocketAttachment(ws.deserializeAttachment());
    if (attachment?.kind !== "deviceWebsocket") {
      await this.#failAuth(ws, "missing digest");
      return;
    }
    const expectedDigest = attachment.digest;
    if (!expectedDigest) {
      await this.#failAuth(ws, "missing digest");
      return;
    }

    if (!state.challenge) {
      state.challenge = attachment?.challenge ? base64UrlToBytes(attachment.challenge) : null;
    }
    if (!state.challenge) {
      await this.#failAuth(ws, "missing challenge");
      return;
    }

    const verified = await verifyDeviceAuth({
      publicKeyB64: message.devicePublicKey,
      signatureB64: message.response,
      challenge: state.challenge,
      expectedDigest,
      claimedDigest: message.devicePublicKeyDigest,
    });

    if (!verified.ok) {
      await this.#failAuth(ws, verified.reason);
      return;
    }

    state.challenge = null;
    state.authenticated = true;
    ws.serializeAttachment({
      kind: "deviceWebsocket",
      digest: expectedDigest,
      connectedAt: attachment.connectedAt,
      authenticated: true,
      version: message.version,
    } satisfies DeviceWebsocketAttachment);
    await this.#clearAuthAlarm();

    const result: AuthResultMessage = { type: "authResult", success: true };
    ws.send(JSON.stringify(result));
  }

  async #handleProxy(request: Request): Promise<Response> {
    if (!this.#deviceWebsocket || !this.#deviceWebsocketState?.authenticated) {
      return jsonError(503, "device offline");
    }

    const url = new URL(request.url);
    const path = url.searchParams.get("path") ?? "/";
    if (path.length === 0 || path.length > MAX_PATH_LEN || !path.startsWith("/")) {
      return jsonError(400, "invalid path");
    }

    const body = new Uint8Array(await request.arrayBuffer());
    if (body.byteLength > MAX_BODY_BYTES) {
      return jsonError(413, "body too large");
    }

    const contentType = request.headers.get("content-type");
    if ((contentType !== null || body.length > 0) && !isJsonContentType(contentType)) {
      return unsupportedContentType(contentType);
    }
    let jsonBody: Record<string, unknown> | null = null;
    if (body.length > 0) {
      try {
        const parsed: unknown = JSON.parse(new TextDecoder().decode(body));
        if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) throw new Error("object required");
        jsonBody = parsed as Record<string, unknown>;
      } catch {
        return jsonError(400, "body must be a JSON object");
      }
    }

    const attachment = readWebsocketAttachment(this.#deviceWebsocket.deserializeAttachment());
    const version = attachment?.kind === "deviceWebsocket" ? attachment.version : undefined;
    const cached = await tryServeCached(this.#env, version, request.method, path, jsonBody);
    if (cached) {
      return cached;
    }

    const requestId = this.#nextRequestId();
    const grantSecret = url.searchParams.get("grantSecret") ?? "";
    const message: RequestMessage = {
      type: "request",
      requestId,
      method: request.method,
      path,
      headers: selectForwardHeaders(request),
      body: jsonBody,
    };
    if (grantSecret) {
      message.grantSecret = grantSecret;
    }

    return new Promise<Response>((resolve) => {
      this.#headroom.push({ message, resolve });
      this.#drainHeadroom();
    });
  }

  async #handlePairingSession(request: Request): Promise<Response> {
    if (!this.#deviceWebsocket || !this.#deviceWebsocketState?.authenticated) {
      return jsonError(503, "device offline");
    }
    if (this.#pairingSessionWaiter) {
      return jsonError(409, "a pairing session is already active");
    }
    let body: Record<string, unknown>;
    try {
      const parsed: unknown = await request.json();
      if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
        return jsonError(400, "invalid body");
      }
      body = parsed as Record<string, unknown>;
    } catch {
      return jsonError(400, "invalid body");
    }

    const message: PairingSessionRequestMessage = {
      type: "pairingSessionRequest",
      name: typeof body.name === "string" ? body.name : "",
    };

    return new Promise<Response>((resolve) => {
      const timeoutMs = parseTimeoutMs(this.#env.REQUEST_TIMEOUT_MS, DEFAULT_REQUEST_TIMEOUT_MS);
      const timer = setTimeout(() => {
        this.#completePairingSession(jsonError(504, "device timeout"));
      }, timeoutMs);
      this.#pairingSessionWaiter = { resolve, timer };
      try {
        this.#deviceWebsocket!.send(JSON.stringify(message));
      } catch {
        clearTimeout(timer);
        this.#pairingSessionWaiter = null;
        resolve(jsonError(503, "device offline"));
      }
    });
  }

  async #handlePairingToken(request: Request): Promise<Response> {
    let body: Record<string, unknown>;
    try {
      const parsed: unknown = await request.json();
      if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
        return jsonError(400, "invalid body");
      }
      body = parsed as Record<string, unknown>;
    } catch {
      return jsonError(400, "invalid body");
    }

    const sessionToken = typeof body.sessionToken === "string" ? body.sessionToken : "";
    if (!sessionToken) {
      return jsonError(400, "invalid sessionToken");
    }

    const pairing = await this.#livePairing();
    if (!pairing || pairing.sessionToken !== sessionToken) {
      return jsonError(401, "session token is invalid or expired");
    }
    if (pairing.grantSecret) {
      return Response.json({ grantSecret: pairing.grantSecret });
    }
    if (!this.#deviceWebsocket || !this.#deviceWebsocketState?.authenticated) {
      return jsonError(503, "device offline");
    }
    if (this.#pairingTokenWaiters.length >= MAX_PAIRING_WAITERS) {
      return jsonError(409, "a pairing session is already active");
    }

    return new Promise<Response>((resolve) => {
      const waitMs = Math.max(0, pairing.expiredAt * 1000 - Date.now());
      const timer = setTimeout(() => {
        const index = this.#pairingTokenWaiters.findIndex((waiter) => waiter.resolve === resolve);
        if (index >= 0) {
          this.#pairingTokenWaiters.splice(index, 1);
        }
        resolve(jsonError(401, "session token is invalid or expired"));
      }, waitMs);
      this.#pairingTokenWaiters.push({ resolve, timer });
    });
  }

  async #onPairingSessionResponse(parsed: PairingSessionResponseMessage): Promise<void> {
    if (parsed.success) {
      const sameToken = this.#pairing?.sessionToken === parsed.sessionToken;
      await this.#savePairing({
        sessionToken: parsed.sessionToken,
        expiredAt: parsed.expiredAt,
        grantSecret: sameToken ? this.#pairing?.grantSecret : undefined,
      });
      if (!sameToken) {
        this.#finishPairingTokenWaiters(jsonError(401, "session token is invalid or expired"));
      }
      this.#completePairingSession(Response.json({
        sessionToken: parsed.sessionToken, expiredAt: parsed.expiredAt,
      }));
      return;
    }
    this.#completePairingSession(jsonError(pairingErrorStatus(parsed.reason), parsed.reason));
  }

  async #onPairingTokenResponse(parsed: PairingSessionTokenResponseMessage): Promise<void> {
    const pairing = await this.#livePairing();
    if (!pairing) {
      return;
    }
    if (parsed.success) {
      await this.#savePairing({ ...pairing, grantSecret: parsed.grantSecret });
      this.#finishPairingTokenWaiters(Response.json({ grantSecret: parsed.grantSecret }));
      return;
    }
    this.#finishPairingTokenWaiters(jsonError(pairingErrorStatus(parsed.reason), parsed.reason));
  }

  async #loadPairing(): Promise<void> {
    if (this.#pairingLoaded) {
      return;
    }
    this.#pairingLoaded = true;
    const stored = await this.#ctx.storage.get(PAIRING_STORAGE_KEY);
    this.#pairing = isPairingState(stored) ? stored : null;
  }

  async #savePairing(state: PairingState | null): Promise<void> {
    this.#pairing = state;
    if (state) {
      await this.#ctx.storage.put(PAIRING_STORAGE_KEY, state);
    } else {
      await this.#ctx.storage.delete(PAIRING_STORAGE_KEY);
    }
  }

  async #livePairing(): Promise<PairingState | null> {
    const pairing = this.#pairing;
    if (!pairing) {
      return null;
    }
    if (Date.now() / 1000 >= pairing.expiredAt) {
      await this.#savePairing(null);
      this.#finishPairingTokenWaiters(jsonError(401, "session token is invalid or expired"));
      return null;
    }
    return pairing;
  }

  #completePairingSession(response: Response): void {
    const pending = this.#pairingSessionWaiter;
    if (!pending) {
      return;
    }
    clearTimeout(pending.timer);
    this.#pairingSessionWaiter = null;
    pending.resolve(response);
  }

  #finishPairingTokenWaiters(response: Response): void {
    const waiters = this.#pairingTokenWaiters.splice(0);
    for (const waiter of waiters) {
      clearTimeout(waiter.timer);
      waiter.resolve(response.clone());
    }
  }

  #drainHeadroom(): void {
    if (!this.#deviceWebsocket || !this.#deviceWebsocketState?.authenticated) {
      this.#rejectAllPending("device offline");
      return;
    }
    while (this.#pending.size < MAX_PENDING_REQUESTS && this.#headroom.length > 0) {
      const { message, resolve } = this.#headroom.shift()!;
      const { requestId } = message;
      // Start the device timeout only when admitted; queued fetches keep the DO awake.
      const timeoutMs = parseTimeoutMs(this.#env.REQUEST_TIMEOUT_MS, DEFAULT_REQUEST_TIMEOUT_MS);
      const timer = setTimeout(() => {
        this.#completePending(requestId, jsonError(504, "device timeout"));
      }, timeoutMs);
      this.#pending.set(requestId, { resolve, timer, grantSecret: message.grantSecret });
      try {
        this.#deviceWebsocket.send(JSON.stringify(message));
      } catch {
        clearTimeout(timer);
        this.#pending.delete(requestId);
        resolve(jsonError(503, "device offline"));
        this.#rejectAllPending("device offline");
        return;
      }
    }
  }

  async #handleTcpStreamWebsocket(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const service = url.searchParams.get("service") ?? "";
    const grantSecret = url.searchParams.get("grantSecret") ?? "";
    if (service !== "dap" && service !== "rfc2217") return jsonError(400, "invalid service");
    if (!grantSecret) return jsonError(400, "invalid grantSecret");
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
      return jsonError(426, "websocket upgrade required");
    }
    if (!this.#deviceWebsocket || !this.#deviceWebsocketState?.authenticated) return jsonError(503, "device offline");
    if (this.#findTcpStreamWebsocket(service) || this.#tcpStreamOpenInFlight(service)) {
      return jsonError(409, "stream already open");
    }

    const deviceAttachment = readWebsocketAttachment(this.#deviceWebsocket.deserializeAttachment());
    const digest = deviceAttachment?.kind === "deviceWebsocket" ? deviceAttachment.digest : "";
    if (!digest) return jsonError(503, "device offline");

    const requestId = this.#nextRequestId();
    const open: TcpStreamOpenRequestMessage = { type: "tcpStreamOpenRequest", requestId, service, grantSecret };
    const result = await new Promise<{ ok: true; channelId: number; byteCredits: number } | { ok: false; reason: string }>((resolve) => {
      const timeoutMs = parseTimeoutMs(this.#env.REQUEST_TIMEOUT_MS, DEFAULT_REQUEST_TIMEOUT_MS);
      const timer = setTimeout(() => {
        this.#streamPending.delete(requestId);
        resolve({ ok: false, reason: "device timeout" });
      }, timeoutMs);
      this.#streamPending.set(requestId, { resolve, timer, service });
      try {
        this.#deviceWebsocket!.send(JSON.stringify(open));
      } catch {
        clearTimeout(timer);
        this.#streamPending.delete(requestId);
        resolve({ ok: false, reason: "device offline" });
      }
    });

    if (!result.ok) {
      const status = result.reason === "device offline" ? 503 : result.reason === "device timeout" ? 504 : 502;
      return jsonError(status, result.reason);
    }

    const pair = new WebSocketPair();
    const client = pair[0];
    const tcpStreamWebsocket = pair[1];
    this.#ctx.acceptWebSocket(tcpStreamWebsocket);
    await this.#saveStream(result.channelId, {
      byteCredits: result.byteCredits,
      capacity: result.byteCredits,
      pending: new Uint8Array(),
    });
    tcpStreamWebsocket.serializeAttachment({
      kind: "tcpStreamWebsocket",
      channelId: result.channelId,
      grantSecret,
      service,
      digest,
      connectedAt: Date.now(),
    } satisfies TcpStreamWebsocketAttachment);
    return new Response(null, { status: 101, webSocket: client });
  }

  #tcpStreamOpenInFlight(service: string): boolean {
    for (const waiter of this.#streamPending.values()) {
      if (waiter.service === service) return true;
    }
    return false;
  }

  #onTcpStreamOpenResponse(parsed: { requestId: string; success: boolean; channelId?: number; byteCredits?: number; reason?: string }): void {
    const waiter = this.#streamPending.get(parsed.requestId);
    if (!waiter) return;
    clearTimeout(waiter.timer);
    this.#streamPending.delete(parsed.requestId);
    if (parsed.success && typeof parsed.channelId === "number" && typeof parsed.byteCredits === "number") {
      waiter.resolve({ ok: true, channelId: parsed.channelId, byteCredits: parsed.byteCredits });
      return;
    }
    waiter.resolve({ ok: false, reason: parsed.reason ?? "open failed" });
  }

  async #onTcpStreamAck(parsed: TcpStreamAckMessage): Promise<void> {
    const ws = this.#findTcpStreamWebsocketByChannel(parsed.channelId);
    if (!ws) return;
    const attachment = readWebsocketAttachment(ws.deserializeAttachment());
    if (attachment?.kind !== "tcpStreamWebsocket") return;
    const buf = await this.#loadStream(parsed.channelId);
    if (!buf) return;
    if (buf.byteCredits + parsed.length > buf.capacity) {
      await this.#dropTcpStream(ws, parsed.channelId, 1009, "stream credit exceeded");
      return;
    }
    buf.byteCredits += parsed.length;
    if (!this.#flushStream(ws, attachment, buf)) {
      await this.#deleteStream(parsed.channelId);
      return;
    }
    await this.#saveStream(parsed.channelId, buf);
  }

  async #loadStream(channelId: number): Promise<StreamBuffer | null> {
    const existing = this.#streamBuffers.get(channelId);
    if (existing) return existing;
    const stored = readStoredStream(await this.#ctx.storage.get(tcpStreamStorageKey(channelId)));
    if (!stored) return null;
    this.#streamBuffers.set(channelId, stored);
    return stored;
  }

  async #saveStream(channelId: number, buf: StreamBuffer): Promise<void> {
    this.#streamBuffers.set(channelId, buf);
    await this.#ctx.storage.put(tcpStreamStorageKey(channelId), {
      byteCredits: buf.byteCredits,
      capacity: buf.capacity,
      pending: buf.pending.byteLength === 0
        ? new ArrayBuffer(0)
        : buf.pending.buffer.slice(buf.pending.byteOffset, buf.pending.byteOffset + buf.pending.byteLength),
    });
  }

  async #deleteStream(channelId: number): Promise<void> {
    this.#streamBuffers.delete(channelId);
    await this.#ctx.storage.delete(tcpStreamStorageKey(channelId));
  }

  async #dropTcpStream(ws: WebSocket, channelId: number, code: number, reason: string): Promise<void> {
    this.#discarded.add(ws);
    await this.#deleteStream(channelId);
    try { ws.close(code, reason); } catch { /* ignore */ }
  }

  #flushStream(ws: WebSocket, attachment: TcpStreamWebsocketAttachment, buf: StreamBuffer): boolean {
    const device = this.#deviceWebsocket;
    if (!device || !this.#deviceWebsocketState?.authenticated) {
      try { ws.close(1011, "device offline"); } catch { /* ignore */ }
      return false;
    }
    while (buf.byteCredits > 0 && buf.pending.length > 0) {
      const n = Math.min(buf.byteCredits, buf.pending.length);
      const framed = new Uint8Array(n + 1);
      framed[0] = attachment.channelId;
      framed.set(buf.pending.subarray(0, n), 1);
      try { device.send(framed); } catch {
        try { ws.close(1011, "device offline"); } catch { /* ignore */ }
        return false;
      }
      buf.byteCredits -= n;
      buf.pending = buf.pending.subarray(n);
    }
    return true;
  }

  async #onTcpStreamWebsocketMessage(
    ws: WebSocket, message: string | ArrayBuffer, attachment: TcpStreamWebsocketAttachment,
  ): Promise<void> {
    if (typeof message === "string") {
      if (message === "ping" || message === "pong") return;
      try { ws.close(1003, "binary frames only"); } catch { /* ignore */ }
      return;
    }
    if (!this.#deviceWebsocket || !this.#deviceWebsocketState?.authenticated) {
      try { ws.close(1011, "device offline"); } catch { /* ignore */ }
      return;
    }
    const bytes = new Uint8Array(message);
    const buf = await this.#loadStream(attachment.channelId);
    if (!buf) {
      await this.#dropTcpStream(ws, attachment.channelId, 1011, "stream closed");
      return;
    }
    if (buf.pending.length + bytes.length > TCP_STREAM_CLIENT_BUFFER_MAX) {
      await this.#dropTcpStream(ws, attachment.channelId, 1009, "stream buffer full");
      return;
    }
    buf.pending = appendPending(buf.pending, bytes);
    if (!this.#flushStream(ws, attachment, buf)) {
      await this.#deleteStream(attachment.channelId);
      return;
    }
    await this.#saveStream(attachment.channelId, buf);
  }

  #forwardDeviceBinary(message: ArrayBuffer): void {
    const bytes = new Uint8Array(message);
    if (bytes.length < 1) return;
    const channelId = bytes[0]!;
    const target = this.#findTcpStreamWebsocketByChannel(channelId);
    if (!target) return;
    try {
      target.send(bytes.subarray(1));
    } catch {
      try { target.close(1011, "forward failed"); } catch { /* ignore */ }
    }
  }

  #sendTcpStreamClose(attachment: TcpStreamWebsocketAttachment): void {
    if (!this.#deviceWebsocket || !this.#deviceWebsocketState?.authenticated) return;
    const close: TcpStreamCloseRequestMessage = {
      type: "tcpStreamCloseRequest",
      requestId: this.#nextRequestId(),
      channelId: attachment.channelId,
      grantSecret: attachment.grantSecret,
    };
    try { this.#deviceWebsocket.send(JSON.stringify(close)); } catch { /* device already gone */ }
  }

  #openTcpStreamWebsockets(): Array<{ ws: WebSocket; attachment: TcpStreamWebsocketAttachment }> {
    const found: Array<{ ws: WebSocket; attachment: TcpStreamWebsocketAttachment }> = [];
    for (const ws of this.#ctx.getWebSockets()) {
      if (this.#discarded.has(ws) || ws.readyState !== WebSocket.OPEN) continue;
      const attachment = readWebsocketAttachment(ws.deserializeAttachment());
      if (attachment?.kind === "tcpStreamWebsocket") found.push({ ws, attachment });
    }
    return found;
  }

  #findTcpStreamWebsocket(service: string): WebSocket | null {
    return this.#openTcpStreamWebsockets().find((item) => item.attachment.service === service)?.ws ?? null;
  }

  #findTcpStreamWebsocketByChannel(channelId: number): WebSocket | null {
    return this.#openTcpStreamWebsockets().find((item) => item.attachment.channelId === channelId)?.ws ?? null;
  }

  #closeTcpStreamWebsockets(code: number, reason: string): void {
    for (const { ws } of this.#openTcpStreamWebsockets()) {
      this.#discarded.add(ws);
      try { ws.close(code, reason); } catch { /* ignore */ }
    }
  }

  #evictStaleTcpStreamWebsockets(): void {
    const now = Date.now();
    for (const { ws, attachment } of this.#openTcpStreamWebsockets()) {
      const lastSeen = this.#ctx.getWebSocketAutoResponseTimestamp(ws)?.getTime() ?? attachment.connectedAt;
      if (now - lastSeen <= HEARTBEAT_TIMEOUT_MS) continue;
      this.#discarded.add(ws);
      try { ws.close(1001, "heartbeat timeout"); } catch { /* ignore */ }
    }
  }

  async #injectWebsocketToken(
    requestId: string,
    parsed: { body: JsonBody; cloud?: { addWebsocketToken: { jsonPathes: string[]; websocketPath: string } } },
  ): Promise<void> {
    const spec = parsed.cloud?.addWebsocketToken;
    if (!spec || !isTcpStreamPath(spec.websocketPath) || parsed.body === null) return;
    const pending = this.#pending.get(requestId);
    const grantSecret = pending?.grantSecret;
    const raw = this.#deviceWebsocket?.deserializeAttachment();
    const deviceAttachment = raw === undefined ? null : readWebsocketAttachment(raw);
    const digest = deviceAttachment?.kind === "deviceWebsocket" ? deviceAttachment.digest : "";
    const ttlSec = parseTimeoutMs(this.#env.WEBSOCKET_TOKEN_TTL_S, 0);
    if (!grantSecret || !digest || !this.#env.WEBSOCKET_TOKEN_SECRET || ttlSec === 0) return;
    try {
      const token = await sealWebsocketToken(this.#env.WEBSOCKET_TOKEN_SECRET, {
        expireAt: Math.floor(Date.now() / 1000) + ttlSec,
        devicePublicKeyDigest: digest,
        path: spec.websocketPath,
        grantSecret,
      });
      injectWebsocketToken(parsed.body, spec.jsonPathes, token);
    } catch {
      // Leave the placeholder in place when the token cannot be sealed.
    }
  }

  #nextRequestId(): string {
    this.#seq = (this.#seq + 1) >>> 0;
    const random = crypto.getRandomValues(new Uint8Array(8));
    return `${this.#seq.toString(16)}-${bytesToBase64Url(random)}`;
  }

  async #failAuth(ws: WebSocket, reason: string): Promise<void> {
    if (ws !== this.#deviceWebsocket) {
      try {
        ws.close(1008, reason);
      } catch {
        // ignore
      }
      return;
    }

    const result: AuthResultMessage = { type: "authResult", success: false, reason };
    try {
      ws.send(JSON.stringify(result));
    } catch {
      // ignore
    }
    try {
      ws.close(1008, reason);
    } catch {
      // ignore
    }
    await this.#clearAuthAlarm();
    await this.#clearDeviceWebsocket();
    this.#rejectAllPending("device offline");
  }

  async #clearDeviceWebsocket(): Promise<void> {
    this.#deviceWebsocket = null;
    this.#deviceWebsocketState = null;
  }

  async #disconnectSocket(code: number, reason: string): Promise<void> {
    for (const ws of this.#ctx.getWebSockets()) {
      this.#discarded.add(ws);
      try {
        ws.close(code, reason);
      } catch {
        // ignore
      }
    }
    this.#deviceWebsocket = null;
    this.#deviceWebsocketState = null;
    this.#rejectAllPending("device offline");
    await this.#clearAuthAlarm();
  }

  #completePending(requestId: string, response: Response): void {
    const pending = this.#pending.get(requestId);
    if (!pending) {
      return;
    }
    clearTimeout(pending.timer);
    this.#pending.delete(requestId);
    pending.resolve(response);
    this.#drainHeadroom();
  }

  #rejectAllPending(reason: string): void {
    for (const [id, pending] of this.#pending) {
      clearTimeout(pending.timer);
      pending.resolve(jsonError(503, reason));
      this.#pending.delete(id);
    }
    for (const { resolve } of this.#headroom.splice(0)) {
      resolve(jsonError(503, reason));
    }
    for (const [id, waiter] of this.#streamPending) {
      clearTimeout(waiter.timer);
      waiter.resolve({ ok: false, reason });
      this.#streamPending.delete(id);
    }
    this.#closeTcpStreamWebsockets(1001, reason);
    this.#completePairingSession(jsonError(503, reason));
    this.#finishPairingTokenWaiters(jsonError(503, reason));
  }

  async #setAuthAlarm(deadline: number): Promise<void> {
    await this.#ctx.storage.setAlarm(Math.max(deadline, Date.now()));
  }

  async #clearAuthAlarm(): Promise<void> {
    await this.#ctx.storage.deleteAlarm();
  }
}
