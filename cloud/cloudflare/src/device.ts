import { DEFAULT_AUTH_TIMEOUT_MS, DEFAULT_REQUEST_TIMEOUT_MS, parseTimeoutMs, type Env } from "./env.ts";
import { base64UrlToBytes, bytesToBase64Url, randomChallenge, verifyDeviceAuth } from "./crypto.ts";
import { isJsonContentType, unsupportedContentType, jsonError, MAX_BODY_BYTES, MAX_PATH_LEN, parseDeviceMessage, selectForwardHeaders, type AuthRequestMessage, type AuthResultMessage, type RequestMessage } from "./protocol.ts";

const MAX_PENDING_REQUESTS = 8;
const HEARTBEAT_TIMEOUT_MS = 33_000;

type HeadroomRequest = {
  message: RequestMessage;
  resolve: (response: Response) => void;
};

type PendingRequest = {
  resolve: (response: Response) => void;
  timer: ReturnType<typeof setTimeout>;
};

type SocketState = {
  authenticated: boolean;
  challenge: Uint8Array | null;
};

type SocketAttachment = {
  connectedAt: number;
  digest: string;
  authenticated: boolean;
  challenge?: string;
};

export class Device implements DurableObject {
  readonly #ctx: DurableObjectState;
  readonly #env: Env;
  readonly #pending = new Map<string, PendingRequest>();
  readonly #headroom: HeadroomRequest[] = [];
  readonly #discarded = new WeakSet<WebSocket>();
  #seq = 0;
  #socket: WebSocket | null = null;
  #socketState: SocketState | null = null;

  constructor(ctx: DurableObjectState, env: Env) {
    this.#ctx = ctx;
    this.#env = env;
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
  }

  async fetch(request: Request): Promise<Response> {
    await this.#validateSocket();
    const url = new URL(request.url);

    if (url.pathname === "/websocket") {
      const digest = url.searchParams.get("digest") ?? "";
      return this.#handleWebSocket(request, digest);
    }
    if (url.pathname === "/proxy") {
      return this.#handleProxy(request);
    }
    if (url.pathname === "/online") {
      return this.#handleOnline();
    }
    return jsonError(404, "not found");
  }

  async alarm(): Promise<void> {
    await this.#validateSocket();
    if (this.#socket && this.#socketState && !this.#socketState.authenticated) {
      await this.#failAuth(this.#socket, "authentication timeout");
      return;
    }
    await this.#clearAuthAlarm();
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    await this.#validateSocket();
    if (ws !== this.#socket) {
      try {
        ws.close(1008, "stale connection");
      } catch {
        // ignore
      }
      return;
    }
    if (typeof message !== "string") {
      await this.#failAuth(ws, "expected text frame");
      return;
    }

    const parsed = parseDeviceMessage(message);
    if (!parsed) {
      if (!this.#socketState?.authenticated) {
        await this.#failAuth(ws, "malformed auth response");
      }
      return;
    }

    if (parsed.type === "authResponse") {
      await this.#handleAuthResponse(ws, parsed);
      return;
    }

    if (!this.#socketState?.authenticated) {
      await this.#failAuth(ws, "not authenticated");
      return;
    }

    const pending = this.#pending.get(parsed.requestId);
    if (!pending) {
      return;
    }

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
    await this.#validateSocket();
    if (this.#socket !== null && ws !== this.#socket) {
      return;
    }
    if (this.#socket === null) {
      this.#restoreSocket();
    }
    if (ws !== this.#socket) {
      return;
    }
    await this.#clearAuthAlarm();
    await this.#clearSocket();
    this.#rejectAllPending("device offline");
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    await this.webSocketClose(ws);
  }

  async #validateSocket(): Promise<void> {
    this.#restoreSocket();
    const ws = this.#socket;
    if (!ws) return;
    const attachment = ws.deserializeAttachment() as SocketAttachment;
    const lastSeen = this.#ctx.getWebSocketAutoResponseTimestamp(ws)?.getTime() ?? attachment.connectedAt;
    if (Date.now() - lastSeen <= HEARTBEAT_TIMEOUT_MS) return;
    this.#discarded.add(ws);
    this.#socket = null;
    this.#socketState = null;
    this.#rejectAllPending("device offline");
    try {
      ws.close(1001, "heartbeat timeout");
    } catch {
      // The edge may already have closed the socket while the object slept.
    }
    await this.#clearAuthAlarm();
  }

  #restoreSocket(): void {
    if (this.#socket) {
      return;
    }
    const sockets = this.#ctx.getWebSockets().filter((ws) => !this.#discarded.has(ws) && ws.readyState === WebSocket.OPEN);
    if (sockets.length === 0) {
      return;
    }
    const socket = sockets[sockets.length - 1]!;
    const attachment = socket.deserializeAttachment() as SocketAttachment | null;
    this.#socket = socket;
    this.#socketState = {
      authenticated: attachment?.authenticated === true,
      challenge: attachment?.challenge ? base64UrlToBytes(attachment.challenge) : null,
    };
  }

  #handleOnline(): Response {
    const online = this.#socket !== null && this.#socketState?.authenticated === true;
    return Response.json({ online }, { headers: { "cache-control": "no-store" } });
  }

  #handleWebSocket(request: Request, digest: string): Response {
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
      return jsonError(426, "websocket upgrade required");
    }
    if (this.#socket !== null) {
      return jsonError(409, "device already connected");
    }

    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    this.#ctx.acceptWebSocket(server);

    const challenge = randomChallenge();
    const challengeB64 = bytesToBase64Url(challenge);
    const authTimeout = parseTimeoutMs(this.#env.AUTH_TIMEOUT_MS, DEFAULT_AUTH_TIMEOUT_MS);
    server.serializeAttachment({ digest, connectedAt: Date.now(), authenticated: false, challenge: challengeB64 } satisfies SocketAttachment);
    void this.#setAuthAlarm(Date.now() + authTimeout);

    this.#socket = server;
    this.#socketState = { authenticated: false, challenge };

    const authRequest: AuthRequestMessage = { type: "authRequest", challenge: challengeB64 };
    server.send(JSON.stringify(authRequest));

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
    const state = this.#socketState;
    if (!state || ws !== this.#socket) {
      return;
    }
    if (state.authenticated) {
      return;
    }

    const attachment = ws.deserializeAttachment() as SocketAttachment;
    const expectedDigest = attachment?.digest ?? "";
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
    ws.serializeAttachment({ digest: expectedDigest, connectedAt: attachment.connectedAt, authenticated: true } satisfies SocketAttachment);
    await this.#clearAuthAlarm();

    const result: AuthResultMessage = { type: "authResult", success: true };
    ws.send(JSON.stringify(result));
  }

  async #handleProxy(request: Request): Promise<Response> {
    if (!this.#socket || !this.#socketState?.authenticated) {
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

    const requestId = this.#nextRequestId();
    const message: RequestMessage = {
      type: "request",
      requestId,
      method: request.method,
      path,
      headers: selectForwardHeaders(request),
      body: jsonBody,
    };

    return new Promise<Response>((resolve) => {
      this.#headroom.push({ message, resolve });
      this.#drainHeadroom();
    });
  }

  #drainHeadroom(): void {
    if (!this.#socket || !this.#socketState?.authenticated) {
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
      this.#pending.set(requestId, { resolve, timer });
      try {
        this.#socket.send(JSON.stringify(message));
      } catch {
        clearTimeout(timer);
        this.#pending.delete(requestId);
        resolve(jsonError(503, "device offline"));
        this.#rejectAllPending("device offline");
        return;
      }
    }
  }

  #nextRequestId(): string {
    this.#seq = (this.#seq + 1) >>> 0;
    const random = crypto.getRandomValues(new Uint8Array(8));
    return `${this.#seq.toString(16)}-${bytesToBase64Url(random)}`;
  }

  async #failAuth(ws: WebSocket, reason: string): Promise<void> {
    if (ws !== this.#socket) {
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
    await this.#clearSocket();
    this.#rejectAllPending("device offline");
  }

  async #clearSocket(): Promise<void> {
    this.#socket = null;
    this.#socketState = null;
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
  }

  async #setAuthAlarm(deadline: number): Promise<void> {
    await this.#ctx.storage.setAlarm(Math.max(deadline, Date.now()));
  }

  async #clearAuthAlarm(): Promise<void> {
    await this.#ctx.storage.deleteAlarm();
  }
}
