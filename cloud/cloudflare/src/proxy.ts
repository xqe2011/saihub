import type { Env } from "./env.ts";

export function proxyToDevice(
  env: Env,
  digest: string,
  method: string,
  path: string,
  body?: string,
  headers?: Record<string, string>,
): Promise<Response> {
  const id = env.DEVICE.idFromName(digest);
  const stub = env.DEVICE.get(id);
  const forwardUrl = new URL("https://device/proxy");
  forwardUrl.searchParams.set("path", path);

  const requestHeaders = new Headers(headers);
  return stub.fetch(
    new Request(forwardUrl, {
      method,
      headers: requestHeaders,
      // An empty string makes Request synthesize text/plain even for bodyless requests.
      body: method === "GET" || method === "HEAD" || body === "" ? undefined : body,
    }),
  );
}
