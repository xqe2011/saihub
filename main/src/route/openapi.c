/**
 * @name OpenAPI route
 * @file openapi.c
 * @author xqe2011
 */
#include "route.h"

#include "http_server.h"
#include "tool.h"

#include <esp_http_server.h>
#include <esp_log.h>

static const char* tag = "SAIHUB-Http";

extern const uint8_t openapi_json_start[] asm("_binary_openapi_json_start");
extern const uint8_t openapi_json_end[] asm("_binary_openapi_json_end");

static esp_err_t Route_OpenApiGetHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  size_t len = (size_t)(openapi_json_end - openapi_json_start);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, (const char*)openapi_json_start, len);
}

static const httpd_uri_t uris[] = {
    {.uri = "/openapi.json", .method = HTTP_GET, .handler = Route_OpenApiGetHandler},
};

esp_err_t Route_OpenApiRegister(httpd_handle_t server)
{
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(uris); i++) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(server, &uris[i]), "register openapi uri failed");
  }
  return ESP_OK;
}
