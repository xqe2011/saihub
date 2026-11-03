/**
 * @name Route registration
 * @file route.h
 * @author xqe2011
 */
#ifndef ROUTE_H__
#define ROUTE_H__

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct HttpServer_Context;

esp_err_t Route_OpenApiRegister(void);
esp_err_t Route_PinRegister(void);
esp_err_t Route_UartRegister(void);
esp_err_t Route_SpiRegister(void);
esp_err_t Route_LockRegister(void);
esp_err_t Route_PowerRegister(void);
esp_err_t Route_McpRegister(void);
esp_err_t Route_ScriptRegister(void);
esp_err_t Route_BuzzerRegister(void);
esp_err_t Route_PortalRegister(void);
esp_err_t Route_ControlRegister(void);
esp_err_t Route_CloudRegister(void);
esp_err_t Route_SystemRegister(void);

/** Append one SPI JSON object to an open chunk stream. escape quotes when the object is inside a JSON string. */
esp_err_t Route_SpiAppendMasterJson(struct HttpServer_Context* ctx, const uint8_t* data, size_t len, bool escape);
esp_err_t Route_SpiAppendSlaveJson(struct HttpServer_Context* ctx, const uint8_t* storage, size_t len, bool escape);

#endif
