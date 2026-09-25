import type { Env } from "./env.ts";
import { MAX_VERSION_LEN } from "./protocol.ts";

export const FILE_CACHE_MCP = "mcp.json";
export const FILE_CACHE_OPENAPI = "openapi.json";
export const FILE_CACHE_PAGE_SIZE = 50;
export const MAX_FILE_CACHE_BYTES = 256 * 1024;

export type FileCacheRow = {
  version: string;
  filename: string;
  bytes: number;
};

export type FileCachePage = {
  page: number;
  pageSize: number;
  total: number;
  items: FileCacheRow[];
};

export function isCachedFilename(value: string): boolean {
  return value === FILE_CACHE_MCP || value === FILE_CACHE_OPENAPI;
}

export function isCachedVersion(value: string): boolean {
  return value.length > 0 && value.length <= MAX_VERSION_LEN && !value.includes("/");
}

export function serializeCacheContent(value: unknown): string | null {
  let parsed: unknown = value;
  if (typeof value === "string") {
    try {
      parsed = JSON.parse(value);
    } catch {
      return null;
    }
  }
  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
    return null;
  }
  return JSON.stringify(parsed);
}

export async function getCachedFile(env: Env, version: string, filename: string): Promise<string | null> {
  const row = await env.DB.prepare("SELECT content FROM file_cache WHERE version = ? AND filename = ?")
    .bind(version, filename)
    .first<{ content: string }>();
  return typeof row?.content === "string" ? row.content : null;
}

export async function putCachedFile(
  env: Env,
  version: string,
  filename: string,
  content: string,
): Promise<{ created: boolean }> {
  const existing = await getCachedFile(env, version, filename);
  await env.DB.prepare("INSERT OR REPLACE INTO file_cache (version, filename, content) VALUES (?, ?, ?)")
    .bind(version, filename, content)
    .run();
  return { created: existing === null };
}

export async function deleteCachedFile(env: Env, version: string, filename: string): Promise<boolean> {
  const result = await env.DB.prepare("DELETE FROM file_cache WHERE version = ? AND filename = ?")
    .bind(version, filename)
    .run();
  return (result.meta.changes ?? 0) > 0;
}

export async function listCachedFiles(env: Env, page: number): Promise<FileCachePage> {
  const offset = (page - 1) * FILE_CACHE_PAGE_SIZE;
  const countRow = await env.DB.prepare("SELECT COUNT(*) AS total FROM file_cache").first<{ total: number }>();
  const total = Number(countRow?.total ?? 0);
  const result = await env.DB.prepare(
    "SELECT version, filename, length(content) AS bytes FROM file_cache ORDER BY version DESC, filename ASC LIMIT ? OFFSET ?",
  )
    .bind(FILE_CACHE_PAGE_SIZE, offset)
    .all<FileCacheRow>();
  return {
    page,
    pageSize: FILE_CACHE_PAGE_SIZE,
    total,
    items: result.results ?? [],
  };
}

function pathName(path: string): string {
  const query = path.indexOf("?");
  return query === -1 ? path : path.slice(0, query);
}

function isToolsListRequest(
  body: Record<string, unknown> | null,
): body is Record<string, unknown> & { id: string | number } {
  if (body === null || body.jsonrpc !== "2.0" || body.method !== "tools/list") {
    return false;
  }
  return typeof body.id === "string" || typeof body.id === "number";
}

export async function tryServeCached(
  env: Env,
  version: string | undefined,
  method: string,
  path: string,
  jsonBody: Record<string, unknown> | null,
): Promise<Response | null> {
  if (!version) {
    return null;
  }
  const name = pathName(path);
  if (method === "GET" && name === "/openapi.json") {
    const content = await getCachedFile(env, version, FILE_CACHE_OPENAPI);
    if (content === null) {
      return null;
    }
    return new Response(content, { status: 200, headers: { "content-type": "application/json" } });
  }
  if (method === "POST" && name === "/mcp" && isToolsListRequest(jsonBody)) {
    const content = await getCachedFile(env, version, FILE_CACHE_MCP);
    if (content === null) {
      return null;
    }
    try {
      return Response.json({ jsonrpc: "2.0", result: JSON.parse(content), id: jsonBody.id });
    } catch {
      return null;
    }
  }
  return null;
}
