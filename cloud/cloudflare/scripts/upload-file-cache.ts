import { readFile } from "node:fs/promises";
import { join } from "node:path";

const FILES = ["mcp.json", "openapi.json"] as const;

export async function upload(adminUrl: string, token: string, version: string): Promise<void> {
  const base = adminUrl.replace(/\/+$/, "");
  const repoRoot = join(import.meta.dirname, "../../..");
  for (const filename of FILES) {
    const content: unknown = JSON.parse(await readFile(join(repoRoot, filename), "utf8"));
    const res = await fetch(`${base}/file-cache`, {
      method: "POST",
      headers: { authorization: `Bearer ${token}`, "content-type": "application/json" },
      body: JSON.stringify({ version, filename, content }),
    });
    if (res.status !== 200 && res.status !== 201) {
      throw new Error(`upload ${filename} failed: HTTP ${res.status} ${await res.text()}`);
    }
    console.log(`uploaded ${filename} for version ${version} (HTTP ${res.status})`);
  }
}

const adminUrl = process.argv[2];
const token = process.argv[3];
const version = process.argv[4];
if (import.meta.main) {
  if (!adminUrl || !token || !version) {
    console.error("usage: bun run upload-file-cache -- <adminUrl> <token> <version>");
    process.exit(1);
  }
  await upload(adminUrl, token, version);
}
