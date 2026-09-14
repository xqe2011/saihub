/**
 * @name Shared tool invoke (MCP + Lua)
 * @file tool_call.h
 * @author xqe2011
 */
#ifndef TOOL_CALL_H__
#define TOOL_CALL_H__

#include <cJSON.h>
#include <stdbool.h>

typedef struct {
  bool ok;
  cJSON* payload; /* caller-owned on success; NULL on error */
  char reason[256];
} ToolCall_Result;

/**
 * Invoke a pin/power/lock tool by MCP name.
 * Does not handle run_script. Caller owns payload and must cJSON_Delete it.
 * Empty-success tools return payload {"ok":true}.
 */
ToolCall_Result ToolCall_Invoke(const char* name, cJSON* args);

#endif
