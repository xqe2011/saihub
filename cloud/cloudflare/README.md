# SAIHub cloud relay

Cloudflare Worker that proxies MCP and REST to a SAIHub board.

[![Deploy to Cloudflare](https://deploy.workers.cloudflare.com/button)](https://deploy.workers.cloudflare.com/?url=https://github.com/xqe2011/saihub/tree/main/cloud/cloudflare)

Deploy provisions D1 (`DB`), the `DEVICE` Durable Object, `AUTH_TIMEOUT_MS` / `REQUEST_TIMEOUT_MS`, and the `ROUTING_TOKEN_SECRET` / `ADMIN_TOKEN` secrets from `.dev.vars.example`. Generate each secret with `openssl rand -hex 32`. The deploy script applies D1 migrations, then publishes the worker.

Local development:

```bash
cp .dev.vars.example .dev.vars
npm install
npm run dev
```

Then open `/admin` on the worker origin, enter `ADMIN_TOKEN`, and whitelist each device digest.
