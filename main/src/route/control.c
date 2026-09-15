/**
 * @name Human control UI route
 * @file control.c
 * @author xqe2011
 */
#include "route.h"

#include "http_server.h"
#include "tool.h"

#include <esp_http_server.h>
#include <esp_log.h>

static const char* tag = "SAIHUB-Control";

/* EMBED_FILES "embed/control.html.gz" → control_html_gz (IDF uses the basename) */
extern const uint8_t control_html_gz_start[] asm("_binary_control_html_gz_start");
extern const uint8_t control_html_gz_end[] asm("_binary_control_html_gz_end");

static esp_err_t Control_GetIndexHandler(httpd_req_t* req)
{
  HttpServer_LogCall(req);
  HttpServer_SetCors(req);
  size_t len = (size_t)(control_html_gz_end - control_html_gz_start);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, (const char*)control_html_gz_start, len);
}

static const httpd_uri_t uris[] = {
    {.uri = "/", .method = HTTP_GET, .handler = Control_GetIndexHandler},
};

esp_err_t Route_ControlRegister(httpd_handle_t server)
{
  for (size_t i = 0; i < TOOL_GET_ARRAY_LENGTH(uris); i++) {
    TOOL_CHECK_ESP_OK_OR_LOG_RETURN(httpd_register_uri_handler(server, &uris[i]), "register control uri failed");
  }
  ESP_LOGI(tag, "Control UI registered at /");
  return ESP_OK;
}
