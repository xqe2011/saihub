/**
 * @name OpenAPI route
 * @file openapi.c
 * @author xqe2011
 */
#include "route.h"

#include "http_server.h"
#include "tool.h"

#include <esp_log.h>

static const char* tag = "SAIHub-Http";

extern const uint8_t openapi_json_start[] asm("_binary_openapi_json_start");
extern const uint8_t openapi_json_end[] asm("_binary_openapi_json_end");

static esp_err_t Route_OpenApiGetHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);
  size_t len = (size_t)(openapi_json_end - openapi_json_start);
  return HttpServer_Send(ctx, 200, "application/json", NULL, openapi_json_start, len);
}

static const HttpServer_Route uris[] = {
    {.uri = "/openapi.json", .method = HTTP_SERVER_GET, .handler = Route_OpenApiGetHandler},
};

esp_err_t Route_OpenApiRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)), "register openapi uri failed");
  return ESP_OK;
}
