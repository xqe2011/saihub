import type { Env } from "./env.ts";
import { jsonError } from "./protocol.ts";

export const WHITELIST_PAGE_SIZE = 50;

export type WhitelistRow = {
  digest: string;
  created_at: number;
};

export type WhitelistPage = {
  page: number;
  pageSize: number;
  total: number;
  items: WhitelistRow[];
};

export async function isWhitelisted(env: Env, digest: string): Promise<boolean> {
  const row = await env.DB.prepare("SELECT 1 AS ok FROM device_whitelist WHERE digest = ?").bind(digest).first();
  return row !== null;
}

export async function requireWhitelisted(env: Env, digest: string): Promise<Response | null> {
  if (await isWhitelisted(env, digest)) {
    return null;
  }
  return jsonError(403, "digest not in whitelist");
}

export async function listWhitelist(env: Env, page: number): Promise<WhitelistPage> {
  const offset = (page - 1) * WHITELIST_PAGE_SIZE;
  const countRow = await env.DB.prepare("SELECT COUNT(*) AS total FROM device_whitelist").first<{ total: number }>();
  const total = Number(countRow?.total ?? 0);
  const result = await env.DB.prepare(
    "SELECT digest, created_at FROM device_whitelist ORDER BY created_at DESC LIMIT ? OFFSET ?",
  )
    .bind(WHITELIST_PAGE_SIZE, offset)
    .all<WhitelistRow>();
  return {
    page,
    pageSize: WHITELIST_PAGE_SIZE,
    total,
    items: result.results ?? [],
  };
}

export async function insertWhitelist(env: Env, digest: string): Promise<{ created: boolean; created_at: number }> {
  const created_at = Date.now();
  const result = await env.DB.prepare("INSERT OR IGNORE INTO device_whitelist (digest, created_at) VALUES (?, ?)")
    .bind(digest, created_at)
    .run();
  if ((result.meta.changes ?? 0) === 0) {
    const existing = await env.DB.prepare("SELECT created_at FROM device_whitelist WHERE digest = ?")
      .bind(digest)
      .first<{ created_at: number }>();
    return { created: false, created_at: Number(existing?.created_at ?? created_at) };
  }
  return { created: true, created_at };
}

export async function deleteWhitelist(env: Env, digest: string): Promise<boolean> {
  const result = await env.DB.prepare("DELETE FROM device_whitelist WHERE digest = ?").bind(digest).run();
  return (result.meta.changes ?? 0) > 0;
}
