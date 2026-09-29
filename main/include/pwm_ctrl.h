/**
 * @name PWM control module
 * @file pwm_ctrl.h
 * @author xqe2011
 */
#ifndef PWM_CTRL_H__
#define PWM_CTRL_H__

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

esp_err_t PwmCtrl_Init(void);
bool PwmCtrl_IsAttached(int hwPin);
esp_err_t PwmCtrl_Set(int hwPin, uint32_t frequencyHz, double dutyPercent);
esp_err_t PwmCtrl_Clear(int hwPin);

#endif
