/**
 * @name Protobuf / domain enum conversions
 * @file api_conv.c
 */
#include "api_conv.h"

#include "config.h"
#include "http_server.h"

#include <stdio.h>
#include <string.h>

bool ApiConv_ModeFromPb(saihub_api_Mode in, GpioCtrl_Mode* out)
{
  switch (in) {
    case saihub_api_Mode_disable:
      *out = GPIO_CTRL_MODE_DISABLE;
      return true;
    case saihub_api_Mode_digitalInput:
      *out = GPIO_CTRL_MODE_DIGITAL_INPUT;
      return true;
    case saihub_api_Mode_digitalOutput:
      *out = GPIO_CTRL_MODE_DIGITAL_OUTPUT;
      return true;
    case saihub_api_Mode_digitalInputOutput:
      *out = GPIO_CTRL_MODE_DIGITAL_INPUT_OUTPUT;
      return true;
    case saihub_api_Mode_pwmOutput:
      *out = GPIO_CTRL_MODE_PWM_OUTPUT;
      return true;
    default:
      return false;
  }
}

saihub_api_Mode ApiConv_ModeToPb(GpioCtrl_Mode in)
{
  switch (in) {
    case GPIO_CTRL_MODE_DISABLE:
      return saihub_api_Mode_disable;
    case GPIO_CTRL_MODE_DIGITAL_INPUT:
      return saihub_api_Mode_digitalInput;
    case GPIO_CTRL_MODE_DIGITAL_OUTPUT:
      return saihub_api_Mode_digitalOutput;
    case GPIO_CTRL_MODE_DIGITAL_INPUT_OUTPUT:
      return saihub_api_Mode_digitalInputOutput;
    case GPIO_CTRL_MODE_PWM_OUTPUT:
      return saihub_api_Mode_pwmOutput;
    default:
      return saihub_api_Mode_MODE_UNSPECIFIED;
  }
}

bool ApiConv_EdgeFromPb(saihub_api_Edge in, GpioCtrl_Edge* out)
{
  switch (in) {
    case saihub_api_Edge_raising:
      *out = GPIO_CTRL_EDGE_RAISING;
      return true;
    case saihub_api_Edge_falling:
      *out = GPIO_CTRL_EDGE_FALLING;
      return true;
    case saihub_api_Edge_both:
      *out = GPIO_CTRL_EDGE_BOTH;
      return true;
    default:
      return false;
  }
}

saihub_api_Edge ApiConv_EdgeToPb(GpioCtrl_Edge in)
{
  switch (in) {
    case GPIO_CTRL_EDGE_RAISING:
      return saihub_api_Edge_raising;
    case GPIO_CTRL_EDGE_FALLING:
      return saihub_api_Edge_falling;
    case GPIO_CTRL_EDGE_BOTH:
      return saihub_api_Edge_both;
    default:
      return saihub_api_Edge_EDGE_UNSPECIFIED;
  }
}

saihub_api_Edge ApiConv_TraceEdgeToPb(const char* edge)
{
  if (edge && strcmp(edge, "raising") == 0) return saihub_api_Edge_raising;
  if (edge && strcmp(edge, "falling") == 0) return saihub_api_Edge_falling;
  return saihub_api_Edge_EDGE_UNSPECIFIED;
}

bool ApiConv_ParityFromPb(saihub_api_UartParity in, UartCtrl_Parity* out)
{
  switch (in) {
    case saihub_api_UartParity_none:
      *out = UART_CTRL_PARITY_NONE;
      return true;
    case saihub_api_UartParity_even:
      *out = UART_CTRL_PARITY_EVEN;
      return true;
    case saihub_api_UartParity_odd:
      *out = UART_CTRL_PARITY_ODD;
      return true;
    default:
      return false;
  }
}

saihub_api_UartParity ApiConv_ParityToPb(UartCtrl_Parity in)
{
  switch (in) {
    case UART_CTRL_PARITY_NONE:
      return saihub_api_UartParity_none;
    case UART_CTRL_PARITY_EVEN:
      return saihub_api_UartParity_even;
    case UART_CTRL_PARITY_ODD:
      return saihub_api_UartParity_odd;
    default:
      return saihub_api_UartParity_UART_PARITY_UNSPECIFIED;
  }
}

bool ApiConv_EncodingFromPb(saihub_api_UartEncoding in, UartCtrl_Encoding* out)
{
  switch (in) {
    case saihub_api_UartEncoding_utf8:
      *out = UART_CTRL_ENCODING_UTF8;
      return true;
    case saihub_api_UartEncoding_byte:
      *out = UART_CTRL_ENCODING_BYTE;
      return true;
    default:
      return false;
  }
}

saihub_api_UartEncoding ApiConv_EncodingToPb(UartCtrl_Encoding in)
{
  switch (in) {
    case UART_CTRL_ENCODING_UTF8:
      return saihub_api_UartEncoding_utf8;
    case UART_CTRL_ENCODING_BYTE:
      return saihub_api_UartEncoding_byte;
    default:
      return saihub_api_UartEncoding_UART_ENCODING_UNSPECIFIED;
  }
}

bool ApiConv_PowerRailFromPb(saihub_api_PowerRail in, GpioCtrl_PowerRail* out)
{
  switch (in) {
    case saihub_api_PowerRail_POWER_RAIL_3V3:
      *out = GPIO_CTRL_POWER_3V3;
      return true;
    case saihub_api_PowerRail_POWER_RAIL_5V:
      *out = GPIO_CTRL_POWER_5V;
      return true;
    default:
      return false;
  }
}

saihub_api_PowerRail ApiConv_PowerRailToPb(GpioCtrl_PowerRail in)
{
  switch (in) {
    case GPIO_CTRL_POWER_3V3:
      return saihub_api_PowerRail_POWER_RAIL_3V3;
    case GPIO_CTRL_POWER_5V:
      return saihub_api_PowerRail_POWER_RAIL_5V;
    default:
      return saihub_api_PowerRail_POWER_RAIL_UNSPECIFIED;
  }
}

static bool ApiConv_MethodsFromPb(const saihub_api_LockMethod* methods, pb_size_t count, uint8_t* out, char* reason,
                                  size_t reasonLen)
{
  *out = 0;
  if (count == 0) {
    snprintf(reason, reasonLen, "method must be a non-empty array of read and/or write.");
    return false;
  }
  for (pb_size_t i = 0; i < count; i++) {
    if (methods[i] == saihub_api_LockMethod_read) {
      *out |= LOCK_METHOD_READ;
    } else if (methods[i] == saihub_api_LockMethod_write) {
      *out |= LOCK_METHOD_WRITE;
    } else {
      snprintf(reason, reasonLen, "method must contain only read and/or write.");
      return false;
    }
  }
  return true;
}

esp_err_t ApiConv_ParseLockResources(const saihub_api_LockResource* resources, pb_size_t count, Lock_Resource* out,
                                     size_t maxOut, size_t* countOut, char* reason, size_t reasonLen)
{
  *countOut = 0;
  if (resources == NULL || count == 0) {
    snprintf(reason, reasonLen, "resources must be a non-empty array.");
    return ESP_ERR_INVALID_ARG;
  }
  for (pb_size_t i = 0; i < count; i++) {
    const saihub_api_LockResource* r = &resources[i];
    uint8_t methods = 0;
    if (!ApiConv_MethodsFromPb(r->method, r->method_count, &methods, reason, reasonLen)) {
      return ESP_ERR_INVALID_ARG;
    }
    if (r->type == saihub_api_LockResourceType_pin) {
      if (r->pins_count == 0) {
        snprintf(reason, reasonLen, "resources[%u].pins must be a non-empty array of pin numbers.", (unsigned)i);
        return ESP_ERR_INVALID_ARG;
      }
      for (pb_size_t j = 0; j < r->pins_count; j++) {
        if (*countOut >= maxOut) {
          snprintf(reason, reasonLen, "resources expand to too many entries (max %u).", (unsigned)maxOut);
          return ESP_ERR_INVALID_ARG;
        }
        int pin = r->pins[j];
        if (!GpioCtrl_IsValidLogicalPin(pin)) {
          char range[32];
          HttpServer_FormatPinRange(range, sizeof(range));
          snprintf(reason, reasonLen, "resources[%u].pins[%u] must be an integer from %s.", (unsigned)i, (unsigned)j,
                   range);
          return ESP_ERR_INVALID_ARG;
        }
        out[*countOut].kind = LOCK_KIND_GPIO;
        out[*countOut].pin = pin;
        out[*countOut].methods = methods;
        (*countOut)++;
      }
    } else if (r->type == saihub_api_LockResourceType_power) {
      if (r->rails_count == 0) {
        snprintf(reason, reasonLen, "resources[%u].rails must be a non-empty array of 3v3 and/or 5v.", (unsigned)i);
        return ESP_ERR_INVALID_ARG;
      }
      for (pb_size_t j = 0; j < r->rails_count; j++) {
        if (*countOut >= maxOut) {
          snprintf(reason, reasonLen, "resources expand to too many entries (max %u).", (unsigned)maxOut);
          return ESP_ERR_INVALID_ARG;
        }
        GpioCtrl_PowerRail rail;
        if (!ApiConv_PowerRailFromPb(r->rails[j], &rail)) {
          snprintf(reason, reasonLen, "resources[%u].rails[%u] must be 3v3 or 5v.", (unsigned)i, (unsigned)j);
          return ESP_ERR_INVALID_ARG;
        }
        out[*countOut].kind = (rail == GPIO_CTRL_POWER_3V3) ? LOCK_KIND_POWER_3V3 : LOCK_KIND_POWER_5V;
        out[*countOut].pin = 0;
        out[*countOut].methods = methods;
        (*countOut)++;
      }
    } else if (r->type == saihub_api_LockResourceType_uart) {
      if (r->ids_count == 0) {
        snprintf(reason, reasonLen, "resources[%u].ids must be a non-empty array of UART ids.", (unsigned)i);
        return ESP_ERR_INVALID_ARG;
      }
      for (pb_size_t j = 0; j < r->ids_count; j++) {
        if (*countOut >= maxOut) {
          snprintf(reason, reasonLen, "resources expand to too many entries (max %u).", (unsigned)maxOut);
          return ESP_ERR_INVALID_ARG;
        }
        int id = r->ids[j];
        if (!Lock_IsValidUartId(id)) {
          snprintf(reason, reasonLen, "resources[%u].ids[%u] must be a UART id from GET /uart/.", (unsigned)i,
                   (unsigned)j);
          return ESP_ERR_INVALID_ARG;
        }
        out[*countOut].kind = LOCK_KIND_UART;
        out[*countOut].pin = id;
        out[*countOut].methods = methods;
        (*countOut)++;
      }
    } else {
      snprintf(reason, reasonLen, "resources[%u].type must be pin, power, or uart.", (unsigned)i);
      return ESP_ERR_INVALID_ARG;
    }
  }
  return ESP_OK;
}

bool ApiConv_SerializeLockResources(const Lock_Resource* resources, size_t count, saihub_api_Lock* lockOut)
{
  lockOut->resources_count = 0;
  bool used[CONFIG_LOCK_MAX_RESOURCES];
  memset(used, 0, sizeof(used));
  for (size_t i = 0; i < count; i++) {
    if (used[i]) continue;
    if (lockOut->resources_count >= 16) return false;
    saihub_api_LockResource* r = &lockOut->resources[lockOut->resources_count];
    memset(r, 0, sizeof(*r));
    uint8_t methods = resources[i].methods;
    r->method_count = 0;
    if (methods & LOCK_METHOD_READ) r->method[r->method_count++] = saihub_api_LockMethod_read;
    if (methods & LOCK_METHOD_WRITE) r->method[r->method_count++] = saihub_api_LockMethod_write;

    if (resources[i].kind == LOCK_KIND_GPIO) {
      r->type = saihub_api_LockResourceType_pin;
      for (size_t j = i; j < count; j++) {
        if (used[j]) continue;
        if (resources[j].kind != LOCK_KIND_GPIO || resources[j].methods != methods) continue;
        if (r->pins_count >= 8) break;
        r->pins[r->pins_count++] = resources[j].pin;
        used[j] = true;
      }
    } else if (resources[i].kind == LOCK_KIND_UART) {
      r->type = saihub_api_LockResourceType_uart;
      for (size_t j = i; j < count; j++) {
        if (used[j]) continue;
        if (resources[j].kind != LOCK_KIND_UART || resources[j].methods != methods) continue;
        if (r->ids_count >= 4) break;
        r->ids[r->ids_count++] = resources[j].pin;
        used[j] = true;
      }
    } else {
      r->type = saihub_api_LockResourceType_power;
      for (size_t j = i; j < count; j++) {
        if (used[j]) continue;
        if (resources[j].kind == LOCK_KIND_GPIO || resources[j].kind == LOCK_KIND_UART ||
            resources[j].methods != methods)
          continue;
        if (r->rails_count >= 2) break;
        GpioCtrl_PowerRail rail =
            (resources[j].kind == LOCK_KIND_POWER_3V3) ? GPIO_CTRL_POWER_3V3 : GPIO_CTRL_POWER_5V;
        r->rails[r->rails_count++] = ApiConv_PowerRailToPb(rail);
        used[j] = true;
      }
    }
    lockOut->resources_count++;
  }
  return true;
}

void ApiConv_UartConfigFromPb(const saihub_api_UartConfigBody* in, UartCtrl_Config* out)
{
  memset(out, 0, sizeof(*out));
  out->enable = in->enable;
  out->baudRate = in->baudRate;
  out->dataBits = in->dataBits;
  ApiConv_ParityFromPb(in->parity, &out->parity);
  out->stopBits = in->stopBits;
  ApiConv_EncodingFromPb(in->encoding, &out->encoding);
  out->rxPin = in->pins.has_rx ? in->pins.rx : -1;
  out->txPin = in->pins.has_tx ? in->pins.tx : -1;
}

void ApiConv_UartStateToPb(int id, const UartCtrl_Config* cfg, saihub_api_UartState* out)
{
  memset(out, 0, sizeof(*out));
  out->id = id;
  out->enable = cfg->enable;
  out->baudRate = cfg->baudRate;
  out->dataBits = cfg->dataBits;
  out->parity = ApiConv_ParityToPb(cfg->parity);
  out->stopBits = cfg->stopBits;
  out->encoding = ApiConv_EncodingToPb(cfg->encoding);
  out->has_pins = true;
  if (cfg->rxPin >= 0) {
    out->pins.has_rx = true;
    out->pins.rx = cfg->rxPin;
  }
  if (cfg->txPin >= 0) {
    out->pins.has_tx = true;
    out->pins.tx = cfg->txPin;
  }
}
