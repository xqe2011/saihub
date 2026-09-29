/**
 * @name Onboard buzzer
 * @file buzzer.h
 * @author xqe2011
 */
#ifndef BUZZER_H__
#define BUZZER_H__

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t Buzzer_Init(void);

/**
 * Play a Morse-style sequence and block until it finishes.
 * `.` is a short beep, `-` is a long beep, space is an extra gap.
 * Returns ESP_ERR_INVALID_STATE when another sequence is already playing.
 */
esp_err_t Buzzer_Play(const char* sequence, char* reason, size_t reasonLen);

/** Duration of a valid sequence, or 0 if sequence is invalid. */
uint64_t Buzzer_SequenceDurationUs(const char* sequence);

#endif
