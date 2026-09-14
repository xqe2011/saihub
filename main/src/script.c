/**
 * @name On-device Lua script runner
 * @file script.c
 * @author xqe2011
 */
#include "script.h"

#include "config.h"
#include "http_server.h"
#include "tool_call.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* tag = "SAIHUB-Script";

#define SCRIPT_HOOK_INTERVAL 1000
#define SCRIPT_SLEEP_CHUNK_US 10000ULL
#define SCRIPT_LOCK_ID_MAX 64

typedef enum {
  SCRIPT_ABORT_NONE = 0,
  SCRIPT_ABORT_TIMEOUT,
  SCRIPT_ABORT_MAX_CALLS,
  SCRIPT_ABORT_OOM,
} Script_Abort;

typedef struct Script_Block {
  size_t size; /* payload bytes */
  int free;
  struct Script_Block* next; /* freelist when free */
} Script_Block;

typedef struct {
  uint8_t* base;
  size_t capacity;
  Script_Block* freeList;
  size_t usedBytes; /* allocated payload+hdr total for OOM diagnostics */
} Script_Arena;

static uint8_t scriptLuaHeap[CONFIG_SCRIPT_LUA_HEAP_BYTES];
static Script_Arena scriptArena;

static StackType_t scriptTaskStack[CONFIG_SCRIPT_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t scriptTaskTcb;
static TaskHandle_t scriptTaskHandle = NULL;

static SemaphoreHandle_t scriptBusyMutex = NULL;
static QueueHandle_t scriptJobQueue = NULL;
static bool scriptBusy = false;

static Script_Abort scriptAbort = SCRIPT_ABORT_NONE;
static uint32_t scriptCalls = 0;
static uint32_t scriptMaxCalls = 0;
static int64_t scriptDeadlineUs = 0;
static char scriptDefaultLockId[SCRIPT_LOCK_ID_MAX];
static char scriptOutput[CONFIG_SCRIPT_MAX_OUTPUT_BYTES];
static size_t scriptOutputLen = 0;

static void Script_ArenaReset(void)
{
  scriptArena.base = scriptLuaHeap;
  scriptArena.capacity = CONFIG_SCRIPT_LUA_HEAP_BYTES;
  scriptArena.usedBytes = 0;
  memset(scriptLuaHeap, 0, sizeof(scriptLuaHeap));
  Script_Block* first = (Script_Block*)scriptArena.base;
  first->size = scriptArena.capacity - sizeof(Script_Block);
  first->free = 1;
  first->next = NULL;
  scriptArena.freeList = first;
}

static void Script_ArenaCoalesce(void)
{
  /* Linear coalesce adjacent free blocks in address order. */
  Script_Block* b = (Script_Block*)scriptArena.base;
  uint8_t* end = scriptArena.base + scriptArena.capacity;
  while ((uint8_t*)b + sizeof(Script_Block) <= end) {
    uint8_t* nextAddr = (uint8_t*)b + sizeof(Script_Block) + b->size;
    if (nextAddr >= end) break;
    Script_Block* n = (Script_Block*)nextAddr;
    if (b->free && n->free) {
      b->size += sizeof(Script_Block) + n->size;
      continue;
    }
    b = n;
  }
  /* Rebuild freelist */
  scriptArena.freeList = NULL;
  b = (Script_Block*)scriptArena.base;
  while ((uint8_t*)b + sizeof(Script_Block) <= end) {
    if (b->free) {
      b->next = scriptArena.freeList;
      scriptArena.freeList = b;
    }
    uint8_t* nextAddr = (uint8_t*)b + sizeof(Script_Block) + b->size;
    if (nextAddr >= end) break;
    b = (Script_Block*)nextAddr;
  }
}

static void* Script_ArenaAlloc(size_t nsize)
{
  if (nsize == 0) return NULL;
  nsize = (nsize + 7u) & ~7u;
  Script_Block* prev = NULL;
  Script_Block* cur = scriptArena.freeList;
  while (cur != NULL) {
    if (cur->free && cur->size >= nsize) {
      size_t remain = cur->size - nsize;
      if (remain >= sizeof(Script_Block) + 8) {
        Script_Block* split = (Script_Block*)((uint8_t*)(cur + 1) + nsize);
        split->size = remain - sizeof(Script_Block);
        split->free = 1;
        split->next = cur->next;
        cur->size = nsize;
        if (prev) prev->next = split;
        else scriptArena.freeList = split;
      } else {
        if (prev) prev->next = cur->next;
        else scriptArena.freeList = cur->next;
      }
      cur->free = 0;
      cur->next = NULL;
      scriptArena.usedBytes += sizeof(Script_Block) + cur->size;
      return (void*)(cur + 1);
    }
    prev = cur;
    cur = cur->next;
  }
  Script_ArenaCoalesce();
  prev = NULL;
  cur = scriptArena.freeList;
  while (cur != NULL) {
    if (cur->free && cur->size >= nsize) {
      size_t remain = cur->size - nsize;
      if (remain >= sizeof(Script_Block) + 8) {
        Script_Block* split = (Script_Block*)((uint8_t*)(cur + 1) + nsize);
        split->size = remain - sizeof(Script_Block);
        split->free = 1;
        split->next = cur->next;
        cur->size = nsize;
        if (prev) prev->next = split;
        else scriptArena.freeList = split;
      } else {
        if (prev) prev->next = cur->next;
        else scriptArena.freeList = cur->next;
      }
      cur->free = 0;
      cur->next = NULL;
      scriptArena.usedBytes += sizeof(Script_Block) + cur->size;
      return (void*)(cur + 1);
    }
    prev = cur;
    cur = cur->next;
  }
  scriptAbort = SCRIPT_ABORT_OOM;
  return NULL;
}

static void Script_ArenaFree(void* ptr)
{
  if (ptr == NULL) return;
  Script_Block* blk = ((Script_Block*)ptr) - 1;
  if (blk->free) return;
  if (scriptArena.usedBytes >= sizeof(Script_Block) + blk->size) {
    scriptArena.usedBytes -= sizeof(Script_Block) + blk->size;
  }
  blk->free = 1;
  blk->next = scriptArena.freeList;
  scriptArena.freeList = blk;
}

static void* Script_LuaAlloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
  (void)ud;
  (void)osize;
  if (nsize == 0) {
    Script_ArenaFree(ptr);
    return NULL;
  }
  if (ptr == NULL) {
    return Script_ArenaAlloc(nsize);
  }
  Script_Block* old = ((Script_Block*)ptr) - 1;
  void* neu = Script_ArenaAlloc(nsize);
  if (neu == NULL) return NULL;
  size_t copy = old->size < nsize ? old->size : nsize;
  memcpy(neu, ptr, copy);
  Script_ArenaFree(ptr);
  return neu;
}

typedef struct {
  const char* script;
  uint32_t maxCalls;
  uint64_t timeoutUs;
  char defaultLockId[SCRIPT_LOCK_ID_MAX];
  Script_Result* out;
  Script_Status status;
  SemaphoreHandle_t done;
} Script_Job;

static int64_t Script_NowUs(void)
{
  return HttpServer_NowUs();
}

static bool Script_TimedOut(void)
{
  return Script_NowUs() >= scriptDeadlineUs;
}

static void Script_RaiseAbort(lua_State* L)
{
  lua_newtable(L);
  int status = 422;
  if (scriptAbort == SCRIPT_ABORT_TIMEOUT) {
    lua_pushstring(L, "Script exceeded timeout.");
  } else if (scriptAbort == SCRIPT_ABORT_MAX_CALLS) {
    lua_pushstring(L, "Script exceeded maxCalls.");
  } else if (scriptAbort == SCRIPT_ABORT_OOM) {
    lua_pushstring(L, "Script ran out of memory.");
  } else {
    lua_pushstring(L, "Script aborted.");
  }
  lua_setfield(L, -2, "reason");
  lua_pushinteger(L, status);
  lua_setfield(L, -2, "status");
  lua_error(L);
}

static void Script_Hook(lua_State* L, lua_Debug* ar)
{
  (void)ar;
  if (scriptAbort == SCRIPT_ABORT_NONE) {
    if (Script_TimedOut()) scriptAbort = SCRIPT_ABORT_TIMEOUT;
  }
  if (scriptAbort != SCRIPT_ABORT_NONE) {
    Script_RaiseAbort(L);
  }
}

static bool Script_IsReasonTable(lua_State* L, int idx)
{
  if (!lua_istable(L, idx)) return false;
  lua_getfield(L, idx, "reason");
  bool ok = lua_isstring(L, -1);
  lua_pop(L, 1);
  return ok;
}

static int Script_SanitizeFailStatus(int status)
{
  switch (status) {
    case 400:
    case 409:
    case 412:
    case 415:
    case 422:
    case 423:
    case 500:
      return status;
    default:
      return 422;
  }
}

static void Script_PushErr(lua_State* L, const char* reason, int status)
{
  lua_newtable(L);
  lua_pushstring(L, reason ? reason : "error");
  lua_setfield(L, -2, "reason");
  lua_pushinteger(L, Script_SanitizeFailStatus(status));
  lua_setfield(L, -2, "status");
}

static int Script_LuaError(lua_State* L)
{
  if (lua_gettop(L) >= 1 && Script_IsReasonTable(L, 1)) {
    lua_settop(L, 1);
    return lua_error(L);
  }

  const char* msg = NULL;
  if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
    msg = NULL;
  } else if (lua_isstring(L, 1)) {
    msg = lua_tostring(L, 1);
  } else {
    msg = luaL_tolstring(L, 1, NULL);
  }

  char reason[256];
  if (msg == NULL || msg[0] == '\0') {
    snprintf(reason, sizeof(reason),
             "The script called error with no message. Catch it with pcall or fix the script.");
  } else {
    snprintf(reason, sizeof(reason),
             "The script reported an error: %s. Catch it with pcall or fix the script.", msg);
  }
  Script_PushErr(L, reason, 422);
  return lua_error(L);
}

static int Script_LuaPrint(lua_State* L)
{
  int n = lua_gettop(L);
  for (int i = 1; i <= n; i++) {
    size_t len = 0;
    const char* s = luaL_tolstring(L, i, &len);
    if (s == NULL) s = "";
    if (i > 1 && scriptOutputLen + 1 < CONFIG_SCRIPT_MAX_OUTPUT_BYTES) {
      scriptOutput[scriptOutputLen++] = '\t';
    }
    size_t copy = len;
    if (scriptOutputLen + copy >= CONFIG_SCRIPT_MAX_OUTPUT_BYTES) {
      copy = CONFIG_SCRIPT_MAX_OUTPUT_BYTES - 1 - scriptOutputLen;
    }
    if (copy > 0) {
      memcpy(scriptOutput + scriptOutputLen, s, copy);
      scriptOutputLen += copy;
    }
    lua_pop(L, 1);
  }
  if (scriptOutputLen + 1 < CONFIG_SCRIPT_MAX_OUTPUT_BYTES) {
    scriptOutput[scriptOutputLen++] = '\n';
  }
  scriptOutput[scriptOutputLen] = '\0';
  return 0;
}

static int Script_LuaSleep(lua_State* L)
{
  lua_Number us = luaL_checknumber(L, 1);
  if (us < 0) us = 0;
  uint64_t remain = (uint64_t)us;
  while (remain > 0) {
    if (Script_TimedOut()) {
      scriptAbort = SCRIPT_ABORT_TIMEOUT;
      Script_RaiseAbort(L);
    }
    uint64_t chunk = remain > SCRIPT_SLEEP_CHUNK_US ? SCRIPT_SLEEP_CHUNK_US : remain;
    uint32_t ms = (uint32_t)((chunk + 999ULL) / 1000ULL);
    if (ms == 0) ms = 1;
    vTaskDelay(pdMS_TO_TICKS(ms));
    remain -= chunk;
  }
  return 0;
}

static void Script_JsonToLua(lua_State* L, const cJSON* node)
{
  if (node == NULL || cJSON_IsNull(node)) {
    lua_pushnil(L);
    return;
  }
  if (cJSON_IsBool(node)) {
    lua_pushboolean(L, cJSON_IsTrue(node));
    return;
  }
  if (cJSON_IsNumber(node)) {
    lua_pushnumber(L, node->valuedouble);
    return;
  }
  if (cJSON_IsString(node)) {
    lua_pushstring(L, node->valuestring ? node->valuestring : "");
    return;
  }
  if (cJSON_IsArray(node)) {
    lua_newtable(L);
    int i = 1;
    const cJSON* child = NULL;
    cJSON_ArrayForEach(child, node) {
      Script_JsonToLua(L, child);
      lua_rawseti(L, -2, i++);
    }
    return;
  }
  if (cJSON_IsObject(node)) {
    lua_newtable(L);
    const cJSON* child = NULL;
    cJSON_ArrayForEach(child, node) {
      Script_JsonToLua(L, child);
      lua_setfield(L, -2, child->string ? child->string : "");
    }
    return;
  }
  lua_pushnil(L);
}

static cJSON* Script_LuaToJson(lua_State* L, int idx);

static cJSON* Script_LuaTableToJson(lua_State* L, int idx)
{
  idx = lua_absindex(L, idx);
  bool isArray = true;
  int maxIndex = 0;
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    if (!lua_isinteger(L, -2)) {
      isArray = false;
    } else {
      lua_Integer k = lua_tointeger(L, -2);
      if (k < 1) isArray = false;
      if ((int)k > maxIndex) maxIndex = (int)k;
    }
    lua_pop(L, 1);
  }

  if (isArray && maxIndex > 0) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 1; i <= maxIndex; i++) {
      lua_rawgeti(L, idx, i);
      cJSON* item = Script_LuaToJson(L, -1);
      lua_pop(L, 1);
      if (item == NULL) item = cJSON_CreateNull();
      cJSON_AddItemToArray(arr, item);
    }
    return arr;
  }

  cJSON* obj = cJSON_CreateObject();
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    const char* key = NULL;
    char keyBuf[64];
    if (lua_isstring(L, -2)) {
      key = lua_tostring(L, -2);
    } else if (lua_isinteger(L, -2)) {
      snprintf(keyBuf, sizeof(keyBuf), "%lld", (long long)lua_tointeger(L, -2));
      key = keyBuf;
    } else {
      lua_pop(L, 1);
      continue;
    }
    cJSON* val = Script_LuaToJson(L, -1);
    if (val == NULL) val = cJSON_CreateNull();
    cJSON_AddItemToObject(obj, key, val);
    lua_pop(L, 1);
  }
  return obj;
}

static cJSON* Script_LuaToJson(lua_State* L, int idx)
{
  switch (lua_type(L, idx)) {
    case LUA_TNIL:
      return cJSON_CreateNull();
    case LUA_TBOOLEAN:
      return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER:
      return cJSON_CreateNumber(lua_tonumber(L, idx));
    case LUA_TSTRING:
      return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE:
      return Script_LuaTableToJson(L, idx);
    default:
      return cJSON_CreateString(lua_typename(L, lua_type(L, idx)));
  }
}

static cJSON* Script_LuaArgsToJson(lua_State* L, int idx)
{
  if (lua_isnoneornil(L, idx)) {
    return cJSON_CreateObject();
  }
  if (!lua_istable(L, idx)) {
    return NULL;
  }
  return Script_LuaTableToJson(L, idx);
}

static bool Script_PayloadIsEmptyOk(const cJSON* payload)
{
  if (!cJSON_IsObject(payload)) return false;
  cJSON* ok = cJSON_GetObjectItem((cJSON*)payload, "ok");
  if (!cJSON_IsTrue(ok)) return false;
  return cJSON_GetArraySize((cJSON*)payload) == 1;
}

static int Script_LuaTool(lua_State* L)
{
  if (scriptAbort != SCRIPT_ABORT_NONE) Script_RaiseAbort(L);
  if (Script_TimedOut()) {
    scriptAbort = SCRIPT_ABORT_TIMEOUT;
    Script_RaiseAbort(L);
  }

  const char* name = lua_tostring(L, lua_upvalueindex(1));
  cJSON* args = Script_LuaArgsToJson(L, 1);
  if (args == NULL) {
    Script_PushErr(L, "tool arguments must be a table.", 400);
    return lua_error(L);
  }

  if (scriptDefaultLockId[0] != '\0') {
    cJSON* existing = cJSON_GetObjectItem(args, "lockId");
    if (existing == NULL) {
      cJSON_AddStringToObject(args, "lockId", scriptDefaultLockId);
    }
  }

  if (scriptCalls >= scriptMaxCalls) {
    cJSON_Delete(args);
    scriptAbort = SCRIPT_ABORT_MAX_CALLS;
    Script_RaiseAbort(L);
  }
  scriptCalls++;

  /* pulse/trace duration must fit remaining timeout budget */
  int64_t remainUs = scriptDeadlineUs - Script_NowUs();
  if (strcmp(name, "pulse_pins") == 0) {
    cJSON* width = cJSON_GetObjectItem(args, "width");
    if (cJSON_IsNumber(width) && width->valuedouble > (double)remainUs) {
      cJSON_Delete(args);
      Script_PushErr(L, "pulse width exceeds remaining script timeout.", 422);
      return lua_error(L);
    }
  } else if (strcmp(name, "trace_pins") == 0) {
    cJSON* duration = cJSON_GetObjectItem(args, "duration");
    double dur = cJSON_IsNumber(duration) ? duration->valuedouble : 1000000.0;
    if (dur > (double)remainUs) {
      cJSON_Delete(args);
      Script_PushErr(L, "trace duration exceeds remaining script timeout.", 422);
      return lua_error(L);
    }
  }

  ToolCall_Result tr = ToolCall_Invoke("lua", name, args);
  cJSON_Delete(args);
  if (!tr.ok) {
    Script_PushErr(L, tr.reason, tr.httpStatus);
    return lua_error(L);
  }

  if (tr.payload == NULL || Script_PayloadIsEmptyOk(tr.payload)) {
    cJSON_Delete(tr.payload);
    return 0;
  }
  Script_JsonToLua(L, tr.payload);
  cJSON_Delete(tr.payload);
  return 1;
}

static void Script_RegisterTool(lua_State* L, const char* name)
{
  lua_pushstring(L, name);
  lua_pushcclosure(L, Script_LuaTool, 1);
  lua_setglobal(L, name);
}

static void Script_OpenSandbox(lua_State* L)
{
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_pop(L, 1);

  lua_pushnil(L);
  lua_setglobal(L, "dofile");
  lua_pushnil(L);
  lua_setglobal(L, "loadfile");
  lua_pushnil(L);
  lua_setglobal(L, "load");
  lua_pushnil(L);
  lua_setglobal(L, "require");
  lua_pushnil(L);
  lua_setglobal(L, "collectgarbage");

  lua_pushcfunction(L, Script_LuaPrint);
  lua_setglobal(L, "print");
  lua_pushcfunction(L, Script_LuaError);
  lua_setglobal(L, "error");
  lua_pushcfunction(L, Script_LuaSleep);
  lua_setglobal(L, "sleep");

  static const char* tools[] = {
      "list_pins",
      "configure_pins",
      "get_pin_levels",
      "set_pin_levels",
      "pulse_pins",
      "get_pin_pwms",
      "set_pin_pwms",
      "trace_pins",
      "get_output_power_state",
      "set_output_power_state",
      "create_lock",
      "renew_lock",
      "delete_lock",
  };
  for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
    Script_RegisterTool(L, tools[i]);
  }
}

static void Script_FormatScriptFail(char* out, size_t outLen, const char* msg)
{
  if (msg == NULL || msg[0] == '\0') {
    snprintf(out, outLen, "The script called error with no message. Catch it with pcall or fix the script.");
  } else {
    snprintf(out, outLen, "The script failed: %s. Catch it with pcall or fix the script.", msg);
  }
}

static int Script_ExtractErr(lua_State* L, char* out, size_t outLen)
{
  if (Script_IsReasonTable(L, -1)) {
    lua_getfield(L, -1, "reason");
    const char* r = lua_tostring(L, -1);
    if (r != NULL && strncmp(r, "The script ", 11) == 0) {
      snprintf(out, outLen, "%s", r);
    } else {
      Script_FormatScriptFail(out, outLen, r);
    }
    lua_pop(L, 1);
    return 422;
  }
  if (lua_isstring(L, -1)) {
    Script_FormatScriptFail(out, outLen, lua_tostring(L, -1));
    return 422;
  }
  Script_FormatScriptFail(out, outLen, luaL_tolstring(L, -1, NULL));
  lua_pop(L, 1);
  return 422;
}

static void Script_ExecuteJob(Script_Job* job)
{
  Script_Result* out = job->out;
  memset(out, 0, sizeof(*out));
  out->httpStatus = 400;

  size_t scriptLen = strlen(job->script);
  if (scriptLen == 0) {
    snprintf(out->reason, sizeof(out->reason), "script must be a non-empty string.");
    job->status = SCRIPT_ERR_BAD_ARG;
    return;
  }
  if (scriptLen > CONFIG_SCRIPT_MAX_SOURCE_BYTES) {
    snprintf(out->reason, sizeof(out->reason), "script exceeds the maximum source size.");
    job->status = SCRIPT_ERR_BAD_ARG;
    return;
  }

  uint32_t maxCalls = job->maxCalls ? job->maxCalls : CONFIG_SCRIPT_MAX_CALLS;
  uint64_t timeoutUs = job->timeoutUs ? job->timeoutUs : CONFIG_SCRIPT_MAX_TIMEOUT_US;
  if (maxCalls < 1 || maxCalls > CONFIG_SCRIPT_MAX_CALLS) {
    snprintf(out->reason, sizeof(out->reason), "maxCalls must be an integer from 1 to the configured maximum.");
    job->status = SCRIPT_ERR_BAD_ARG;
    return;
  }
  if (timeoutUs < 1 || timeoutUs > CONFIG_SCRIPT_MAX_TIMEOUT_US) {
    snprintf(out->reason, sizeof(out->reason),
             "timeout must be an integer from 1 to the configured maximum microseconds.");
    job->status = SCRIPT_ERR_BAD_ARG;
    return;
  }

  Script_ArenaReset();
  scriptAbort = SCRIPT_ABORT_NONE;
  scriptCalls = 0;
  scriptMaxCalls = maxCalls;
  scriptDeadlineUs = Script_NowUs() + (int64_t)timeoutUs;
  scriptOutputLen = 0;
  scriptOutput[0] = '\0';
  snprintf(scriptDefaultLockId, sizeof(scriptDefaultLockId), "%s", job->defaultLockId);

  int64_t started = Script_NowUs();
  lua_State* L = lua_newstate(Script_LuaAlloc, NULL);
  if (L == NULL) {
    snprintf(out->reason, sizeof(out->reason), "Script ran out of memory.");
    job->status = SCRIPT_ERR_RUNTIME;
    out->httpStatus = 422;
    return;
  }

  Script_OpenSandbox(L);
  lua_sethook(L, Script_Hook, LUA_MASKCOUNT, SCRIPT_HOOK_INTERVAL);

  int loadStatus = luaL_loadbuffer(L, job->script, scriptLen, "script");
  if (loadStatus != LUA_OK) {
    (void)Script_ExtractErr(L, out->reason, sizeof(out->reason));
    out->elapsedUs = (uint64_t)(Script_NowUs() - started);
    out->calls = scriptCalls;
    lua_close(L);
    job->status = SCRIPT_ERR_RUNTIME;
    out->httpStatus = 400;
    return;
  }

  int runStatus = lua_pcall(L, 0, 1, 0);
  out->elapsedUs = (uint64_t)(Script_NowUs() - started);
  out->calls = scriptCalls;

  if (scriptAbort == SCRIPT_ABORT_TIMEOUT || (runStatus != LUA_OK && scriptAbort == SCRIPT_ABORT_TIMEOUT)) {
    snprintf(out->reason, sizeof(out->reason), "Script exceeded timeout.");
    out->httpStatus = 422;
    lua_close(L);
    job->status = SCRIPT_ERR_TIMEOUT;
    return;
  }
  if (scriptAbort == SCRIPT_ABORT_MAX_CALLS) {
    snprintf(out->reason, sizeof(out->reason), "Script exceeded maxCalls.");
    out->httpStatus = 422;
    lua_close(L);
    job->status = SCRIPT_ERR_RUNTIME;
    return;
  }
  if (scriptAbort == SCRIPT_ABORT_OOM) {
    snprintf(out->reason, sizeof(out->reason), "Script ran out of memory.");
    out->httpStatus = 422;
    lua_close(L);
    job->status = SCRIPT_ERR_RUNTIME;
    return;
  }

  if (runStatus != LUA_OK) {
    out->httpStatus = Script_ExtractErr(L, out->reason, sizeof(out->reason));
    lua_close(L);
    job->status = SCRIPT_ERR_RUNTIME;
    return;
  }

  if (!lua_isnoneornil(L, -1)) {
    out->result = Script_LuaToJson(L, -1);
  } else {
    out->result = NULL;
  }
  out->output = strdup(scriptOutput);
  out->reason[0] = '\0';
  out->httpStatus = 200;
  lua_close(L);
  job->status = SCRIPT_OK;
}

static void Script_TaskMain(void* arg)
{
  (void)arg;
  for (;;) {
    Script_Job* job = NULL;
    if (xQueueReceive(scriptJobQueue, &job, portMAX_DELAY) != pdTRUE || job == NULL) {
      continue;
    }
    Script_ExecuteJob(job);
    xSemaphoreGive(job->done);
  }
}

esp_err_t Script_Init(void)
{
  if (scriptTaskHandle != NULL) return ESP_OK;

  scriptBusyMutex = xSemaphoreCreateMutex();
  if (scriptBusyMutex == NULL) return ESP_ERR_NO_MEM;

  scriptJobQueue = xQueueCreate(1, sizeof(Script_Job*));
  if (scriptJobQueue == NULL) return ESP_ERR_NO_MEM;

  scriptTaskHandle =
      xTaskCreateStatic(Script_TaskMain, "saihub_script", CONFIG_SCRIPT_STACK_BYTES / sizeof(StackType_t), NULL, 5,
                        scriptTaskStack, &scriptTaskTcb);
  if (scriptTaskHandle == NULL) {
    ESP_LOGE(tag, "script task create failed");
    return ESP_FAIL;
  }
  Script_ArenaReset();
  ESP_LOGI(tag, "script runner ready heap=%u stack=%u", (unsigned)CONFIG_SCRIPT_LUA_HEAP_BYTES,
           (unsigned)CONFIG_SCRIPT_STACK_BYTES);
  return ESP_OK;
}

bool Script_IsBusy(void)
{
  if (scriptBusyMutex == NULL) return false;
  if (xSemaphoreTake(scriptBusyMutex, portMAX_DELAY) != pdTRUE) return true;
  bool busy = scriptBusy;
  xSemaphoreGive(scriptBusyMutex);
  return busy;
}

static void Script_FillBusy(Script_Result* out)
{
  memset(out, 0, sizeof(*out));
  snprintf(out->reason, sizeof(out->reason), "A script is already running. Wait for it to finish.");
  out->httpStatus = 409;
}

static void Script_FillInternal(Script_Result* out)
{
  memset(out, 0, sizeof(*out));
  snprintf(out->reason, sizeof(out->reason), "internal");
  out->httpStatus = 500;
}

typedef struct {
  httpd_req_t* req;
  char* script;
  uint32_t maxCalls;
  uint64_t timeoutUs;
  char lockId[SCRIPT_LOCK_ID_MAX];
  bool hasLockId;
  Script_HttpRespondFn respond;
  void* userCtx;
} Script_WaiterJob;

static void Script_WaiterTask(void* arg)
{
  Script_WaiterJob* job = (Script_WaiterJob*)arg;
  Script_Result sr;
  Script_Status st = Script_Run(job->script, job->maxCalls, job->timeoutUs, job->hasLockId ? job->lockId : NULL, &sr);
  job->respond(job->req, st, &sr, job->userCtx);
  httpd_req_async_handler_complete(job->req);
  free(job->script);
  free(job);
  vTaskDelete(NULL);
}

esp_err_t Script_RunAsync(httpd_req_t* req, const char* script, uint32_t maxCalls, uint64_t timeoutUs,
                          const char* defaultLockId, Script_HttpRespondFn respond, void* userCtx)
{
  if (req == NULL || script == NULL || respond == NULL) return ESP_ERR_INVALID_ARG;

  if (Script_IsBusy()) {
    Script_Result sr;
    Script_FillBusy(&sr);
    respond(req, SCRIPT_ERR_BUSY, &sr, userCtx);
    return ESP_OK;
  }

  Script_WaiterJob* job = (Script_WaiterJob*)calloc(1, sizeof(*job));
  if (job == NULL) {
    Script_Result sr;
    Script_FillInternal(&sr);
    respond(req, SCRIPT_ERR_INTERNAL, &sr, userCtx);
    return ESP_OK;
  }

  size_t scriptLen = strlen(script);
  job->script = (char*)malloc(scriptLen + 1);
  if (job->script == NULL) {
    free(job);
    Script_Result sr;
    Script_FillInternal(&sr);
    respond(req, SCRIPT_ERR_INTERNAL, &sr, userCtx);
    return ESP_OK;
  }
  memcpy(job->script, script, scriptLen + 1);
  job->maxCalls = maxCalls;
  job->timeoutUs = timeoutUs;
  if (defaultLockId != NULL && defaultLockId[0] != '\0') {
    snprintf(job->lockId, sizeof(job->lockId), "%s", defaultLockId);
    job->hasLockId = true;
  }
  job->respond = respond;
  job->userCtx = userCtx;

  httpd_req_t* copy = NULL;
  if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
    free(job->script);
    free(job);
    Script_Result sr;
    Script_FillInternal(&sr);
    respond(req, SCRIPT_ERR_INTERNAL, &sr, userCtx);
    return ESP_OK;
  }
  job->req = copy;

  BaseType_t created =
      xTaskCreate(Script_WaiterTask, "saihub_scwait", CONFIG_SCRIPT_WAITER_STACK_BYTES / sizeof(StackType_t), job, 5,
                  NULL);
  if (created != pdPASS) {
    Script_Result sr;
    Script_FillInternal(&sr);
    respond(copy, SCRIPT_ERR_INTERNAL, &sr, userCtx);
    httpd_req_async_handler_complete(copy);
    free(job->script);
    free(job);
    return ESP_OK;
  }
  return ESP_OK;
}

Script_Status Script_Run(const char* script, uint32_t maxCalls, uint64_t timeoutUs, const char* defaultLockId,
                         Script_Result* out)
{
  if (out == NULL || script == NULL) return SCRIPT_ERR_BAD_ARG;
  memset(out, 0, sizeof(*out));
  out->httpStatus = 400;

  if (scriptTaskHandle == NULL || scriptJobQueue == NULL) {
    snprintf(out->reason, sizeof(out->reason), "internal");
    out->httpStatus = 500;
    return SCRIPT_ERR_INTERNAL;
  }

  if (xSemaphoreTake(scriptBusyMutex, 0) != pdTRUE) {
    snprintf(out->reason, sizeof(out->reason), "A script is already running. Wait for it to finish.");
    out->httpStatus = 409;
    return SCRIPT_ERR_BUSY;
  }
  if (scriptBusy) {
    xSemaphoreGive(scriptBusyMutex);
    snprintf(out->reason, sizeof(out->reason), "A script is already running. Wait for it to finish.");
    out->httpStatus = 409;
    return SCRIPT_ERR_BUSY;
  }
  scriptBusy = true;
  xSemaphoreGive(scriptBusyMutex);

  Script_Job job;
  memset(&job, 0, sizeof(job));
  job.script = script;
  job.maxCalls = maxCalls;
  job.timeoutUs = timeoutUs;
  if (defaultLockId != NULL) {
    snprintf(job.defaultLockId, sizeof(job.defaultLockId), "%s", defaultLockId);
  }
  job.out = out;
  job.status = SCRIPT_ERR_INTERNAL;
  job.done = xSemaphoreCreateBinary();
  if (job.done == NULL) {
    xSemaphoreTake(scriptBusyMutex, portMAX_DELAY);
    scriptBusy = false;
    xSemaphoreGive(scriptBusyMutex);
    snprintf(out->reason, sizeof(out->reason), "internal");
    out->httpStatus = 500;
    return SCRIPT_ERR_INTERNAL;
  }

  Script_Job* jobPtr = &job;
  if (xQueueSend(scriptJobQueue, &jobPtr, 0) != pdTRUE) {
    vSemaphoreDelete(job.done);
    xSemaphoreTake(scriptBusyMutex, portMAX_DELAY);
    scriptBusy = false;
    xSemaphoreGive(scriptBusyMutex);
    snprintf(out->reason, sizeof(out->reason), "A script is already running. Wait for it to finish.");
    out->httpStatus = 409;
    return SCRIPT_ERR_BUSY;
  }

  xSemaphoreTake(job.done, portMAX_DELAY);
  vSemaphoreDelete(job.done);

  xSemaphoreTake(scriptBusyMutex, portMAX_DELAY);
  scriptBusy = false;
  xSemaphoreGive(scriptBusyMutex);
  return job.status;
}

void Script_ResultFree(Script_Result* out)
{
  if (out == NULL) return;
  cJSON_Delete(out->result);
  out->result = NULL;
  free(out->output);
  out->output = NULL;
}
