/**
 * @name Protobuf / domain enum conversions
 * @file api_conv.h
 */
#ifndef API_CONV_H__
#define API_CONV_H__

#include "api.pb.h"
#include "gpio_ctrl.h"
#include "lock.h"
#include "uart_ctrl.h"

#include <stdbool.h>
#include <stddef.h>

bool ApiConv_ModeFromPb(saihub_api_Mode in, GpioCtrl_Mode* out);
saihub_api_Mode ApiConv_ModeToPb(GpioCtrl_Mode in);

bool ApiConv_EdgeFromPb(saihub_api_Edge in, GpioCtrl_Edge* out);
saihub_api_Edge ApiConv_EdgeToPb(GpioCtrl_Edge in);
/** Trace event edges are raising|falling only. */
saihub_api_Edge ApiConv_TraceEdgeToPb(const char* edge);

bool ApiConv_ParityFromPb(saihub_api_UartParity in, UartCtrl_Parity* out);
saihub_api_UartParity ApiConv_ParityToPb(UartCtrl_Parity in);

bool ApiConv_EncodingFromPb(saihub_api_UartEncoding in, UartCtrl_Encoding* out);
saihub_api_UartEncoding ApiConv_EncodingToPb(UartCtrl_Encoding in);

bool ApiConv_PowerRailFromPb(saihub_api_PowerRail in, GpioCtrl_PowerRail* out);
saihub_api_PowerRail ApiConv_PowerRailToPb(GpioCtrl_PowerRail in);

/** Expand LockResource protobuf into Lock_Resource entries. */
esp_err_t ApiConv_ParseLockResources(const saihub_api_LockResource* resources, pb_size_t count, Lock_Resource* out,
                                     size_t maxOut, size_t* countOut, char* reason, size_t reasonLen);

/** Fill Lock protobuf resources from Lock_Resource entries (grouped like JSON). */
bool ApiConv_SerializeLockResources(const Lock_Resource* resources, size_t count, saihub_api_Lock* lockOut);

void ApiConv_UartConfigFromPb(const saihub_api_UartConfigBody* in, UartCtrl_Config* out);
void ApiConv_UartStateToPb(int id, const UartCtrl_Config* cfg, saihub_api_UartState* out);

#endif
