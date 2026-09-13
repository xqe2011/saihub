/**
 * @name Route registration
 * @file route.h
 * @author xqe2011
 */
#ifndef ROUTE_H__
#define ROUTE_H__

#include <esp_err.h>
#include <esp_http_server.h>

esp_err_t Route_OpenApiRegister(httpd_handle_t server);
esp_err_t Route_PinRegister(httpd_handle_t server);
esp_err_t Route_LockRegister(httpd_handle_t server);
esp_err_t Route_PowerRegister(httpd_handle_t server);
esp_err_t Route_McpRegister(httpd_handle_t server);

#endif
