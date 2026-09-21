# Agent guidelines

## No chip details in responses

Never name the MCU, vendor, or SDK in anything a user or API client can see.

Forbidden in chat replies, HTTP/MCP error `reason` strings, OpenAPI descriptions, and other client-facing copy: `ESP32`, `ESP32-S3`, Espressif, ESP-IDF, LEDC, APB, and similar silicon/SDK terms.

Speak in product terms instead: pin, PWM, frequency, duty, resources.

Firmware logs, identifiers, and SDK API names in `.c`/`.h` may use those terms. Do not leak them into responses.

```
# ❌ BAD (OpenAPI / HTTP / chat)
"ESP32-S3 LEDC cannot generate very low frequencies"
"LEDC timer exhausted"

# ✅ GOOD
"Hz. Very low frequencies may be rejected."
"PWM resources exhausted. Free another pwmOutput pin or reuse an existing frequency."
```

## Code format (TypeScript / JavaScript)

Applies to `**/*.{ts,tsx,js,jsx,mjs,cjs}`.

- Line width: **120** characters. Prefer keeping args, signatures, and object literals on one line when they fit.
- Unwrap premature wraps: if a call, parameter list, or short object fits in 120 cols, keep it on one line.
- Imports: **always one line**, never multi-line `import { ... }`. Long import lists stay single-line even past 120.

```typescript
// ❌ BAD
import {
  foo,
  bar,
  type Baz,
} from "./mod.ts";

function handle(
  request: Request,
  env: Env,
): Response {
  return stub.fetch(
    forwardUrl,
    request,
  );
}

// ✅ GOOD
import { foo, bar, type Baz } from "./mod.ts";

function handle(request: Request, env: Env): Response {
  return stub.fetch(forwardUrl, request);
}
```

Still wrap when a single expression cannot fit in 120 characters (except imports).
