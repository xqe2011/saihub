/**
 * @name Buzzer routes
 * @file buzzer.c
 * @author xqe2011
 */
#include "route.h"

#include "buzzer.h"
#include "http_server.h"
#include "tool.h"

#include <cJSON.h>
#include <esp_log.h>

static const char* tag = "SAIHub-Http";

static esp_err_t Route_BuzzerPostHandler(HttpServer_Context* ctx)
{
  HttpServer_LogCall(ctx);

  if (!HttpServer_HasJsonContentType(ctx)) {
    return HttpServer_SendError(ctx, 415, "Content-Type must be application/json.");
  }

  esp_err_t perr = ESP_OK;
  cJSON* body = HttpServer_ParseBody(ctx, &perr);
  if (body == NULL) return HttpServer_SendError(ctx, 400, "invalid_json");

  cJSON* sequenceItem = cJSON_GetObjectItem(body, "sequence");
  if (!cJSON_IsString(sequenceItem) || sequenceItem->valuestring == NULL) {
    cJSON_Delete(body);
    return HttpServer_SendError(ctx, 400, "sequence must be a non-empty string of . and -.");
  }

  TOOL_CALL_LOG("rest/%s POST /buzzer sequence=%s", HttpServer_GetFrom(ctx), sequenceItem->valuestring);

  char reason[128];
  esp_err_t ret = Buzzer_Play(sequenceItem->valuestring, reason, sizeof(reason));
  cJSON_Delete(body);
  if (ret == ESP_ERR_INVALID_ARG) return HttpServer_SendError(ctx, 400, reason);
  if (ret == ESP_ERR_INVALID_STATE) return HttpServer_SendError(ctx, 409, reason);
  if (ret != ESP_OK) return HttpServer_SendError(ctx, 500, reason[0] ? reason : "internal");
  return HttpServer_SendEmpty(ctx, 204);
}

static const HttpServer_Route uris[] = {
    {.uri = "/buzzer", .method = HTTP_SERVER_POST, .handler = Route_BuzzerPostHandler},
};

esp_err_t Route_BuzzerRegister(void)
{
  TOOL_CHECK_ESP_OK_OR_LOG_RETURN(HttpServer_RegisterRoutes(uris, TOOL_GET_ARRAY_LENGTH(uris)),
                                  "register buzzer uri failed");
  (void)tag;
  return ESP_OK;
}
