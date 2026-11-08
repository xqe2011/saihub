/**
 * @name TCP services
 * @file tcp.h
 * @author xqe2011
 */
#ifndef TCP_H__
#define TCP_H__

#include <esp_err.h>
#include <stdbool.h>

esp_err_t Tcp_DapStart(void); /* ESP_ERR_INVALID_STATE if already running */
esp_err_t Tcp_DapStop(void);  /* drop client, stop listen task, wait until gone */
bool Tcp_DapIsRunning(void);

#endif
