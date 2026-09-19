/**
 * @name Route registration
 * @file route.h
 * @author xqe2011
 */
#ifndef ROUTE_H__
#define ROUTE_H__

#include <esp_err.h>

esp_err_t Route_OpenApiRegister(void);
esp_err_t Route_PinRegister(void);
esp_err_t Route_UartRegister(void);
esp_err_t Route_LockRegister(void);
esp_err_t Route_PowerRegister(void);
esp_err_t Route_McpRegister(void);
esp_err_t Route_ScriptRegister(void);
esp_err_t Route_PortalRegister(void);
esp_err_t Route_ControlRegister(void);

#endif
