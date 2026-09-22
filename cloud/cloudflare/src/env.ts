export interface Env {
  DEVICE: DurableObjectNamespace;
  DB: D1Database;
  AUTH_TIMEOUT_MS: string;
  REQUEST_TIMEOUT_MS: string;
  ROUTING_TOKEN_SECRET: string;
  ADMIN_TOKEN: string;
}

export const DEFAULT_AUTH_TIMEOUT_MS = 10_000;
export const DEFAULT_REQUEST_TIMEOUT_MS = 55_000;

export function parseTimeoutMs(value: string | undefined, fallback: number): number {
  if (!value) {
    return fallback;
  }
  const parsed = Number.parseInt(value, 10);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    return fallback;
  }
  return parsed;
}
