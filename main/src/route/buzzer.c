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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHub-Http";

typedef struct {
  HttpServer_Context* ctx;
  char* sequence;
} BuzzerPlayJob;

static void Route_BuzzerPlayTask(void* arg)
{
  BuzzerPlayJob* j = arg;
  char reason[128];
  esp_err_t ret = Buzzer_Play(j->sequence, reason, sizeof(reason));
  if (ret == ESP_ERR_INVALID_ARG) HttpServer_SendError(j->ctx, 400, reason);
  else if (ret == ESP_ERR_INVALID_STATE) HttpServer_SendError(j->ctx, 409, reason);
  else if (ret != ESP_OK) HttpServer_SendError(j->ctx, 500, reason[0] ? reason : "internal");
  else HttpServer_SendEmpty(j->ctx, 204);
  HttpServer_AsyncComplete(j->ctx);
  free(j->sequence);
  free(j);
  vTaskDelete(NULL);
}

static esp_err_t Route_BuzzerStartPlay(HttpServer_Context* ctx, const char* sequence)
{
  BuzzerPlayJob* j = calloc(1, sizeof(*j));
  if (!j) return HttpServer_SendError(ctx, 500, "internal");
  size_t n = strlen(sequence);
  j->sequence = malloc(n + 1);
  if (!j->sequence) {
    free(j);
    return HttpServer_SendError(ctx, 500, "internal");
  }
  memcpy(j->sequence, sequence, n + 1);
  if (HttpServer_AsyncBegin(ctx, &j->ctx) != ESP_OK) {
    free(j->sequence);
    free(j);
    return HttpServer_SendError(ctx, 500, "internal");
  }
  if (xTaskCreate(Route_BuzzerPlayTask, "buzzer_play", 4096, j, 5, NULL) != pdPASS) {
    HttpServer_SendError(j->ctx, 500, "internal");
    HttpServer_AsyncComplete(j->ctx);
    free(j->sequence);
    free(j);
    return ESP_OK;
  }
  return ESP_OK;
}

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

  esp_err_t ret = Route_BuzzerStartPlay(ctx, sequenceItem->valuestring);
  cJSON_Delete(body);
  return ret;
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
