/**
 * @name Pairing DNS hijack
 * @file dns.h
 * @author xqe2011
 */
#ifndef DNS_H__
#define DNS_H__

#include <esp_err.h>

esp_err_t Dns_Start(void);
void Dns_Stop(void);

#endif
