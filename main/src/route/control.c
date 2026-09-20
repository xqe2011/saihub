/**
 * @name Human control UI route
 * @file control.c
 * @author xqe2011
 */
#include "route.h"

#include "http_server.h"
#include "tool.h"

#include <esp_log.h>

static const char* tag = "SAIHub-Control";

/* EMBED_FILES "embed/control.html.gz" → control_html_gz (IDF uses the basename) */
extern const uint8_t control_html_gz_start[] asm("_binary_control_html_gz_start");
extern const uint8_t control_html_gz_end[] asm("_binary_control_html_gz_end");

static esp_err_t Control_GetIndexHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  size_t len = (size_t)(control_html_gz_end - control_html_gz_start);
  static const HttpServer_Header headers[] = {
      {"Content-Encoding", "gzip"}, {"Cache-Control", "no-store"}, {NULL, NULL},
  };
  return HttpServer_Send(ctx, 200, "text/html", headers, control_html_gz_start, len);
}

static const HttpServer_Route uris[] = {
    {.uri = "/", .method = HTTP_SERVER_GET, .handler = Control_GetIndexHandler},
};

esp_err_t Route_ControlRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)), "register control uri failed");
  ESP_LOGI(tag, "Control UI registered at /");
  return ESP_OK;
}
