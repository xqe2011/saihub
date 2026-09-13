/**
 * @name GPIO control module
 * @file gpio_ctrl.h
 * @author xqe2011
 */
#ifndef GPIO_CTRL_H__
#define GPIO_CTRL_H__

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  GPIO_CTRL_MODE_DISABLE = 0,
  GPIO_CTRL_MODE_INPUT,
  GPIO_CTRL_MODE_OUTPUT,
  GPIO_CTRL_MODE_OUTPUT_OPEN_DRAIN,
  GPIO_CTRL_MODE_INPUT_OUTPUT,
  GPIO_CTRL_MODE_INPUT_OUTPUT_OPEN_DRAIN,
} GpioCtrl_Mode;

typedef enum {
  GPIO_CTRL_EDGE_RAISING = 0,
  GPIO_CTRL_EDGE_FALLING,
  GPIO_CTRL_EDGE_BOTH,
} GpioCtrl_Edge;

typedef enum {
  GPIO_CTRL_POWER_3V3 = 0,
  GPIO_CTRL_POWER_5V,
} GpioCtrl_PowerRail;

typedef struct {
  GpioCtrl_Mode mode;
  bool pullUp;
  bool pullDown;
  int level;
} GpioCtrl_State;

typedef struct {
  int pin; /* logical pin; -1 for single-pin response without pin field usage */
  const char* edge;
  int level;
  int64_t time;
} GpioCtrl_TraceEvent;

bool GpioCtrl_IsValidLogicalPin(int pin);
int GpioCtrl_GetLogicalCount(void);
esp_err_t GpioCtrl_Init(void);
esp_err_t GpioCtrl_GetState(int logicalPin, GpioCtrl_State* out);
esp_err_t GpioCtrl_SetConfig(int logicalPin, GpioCtrl_Mode mode, bool pullUp, bool pullDown);
esp_err_t GpioCtrl_GetLevel(int logicalPin, int* level);
esp_err_t GpioCtrl_SetLevel(int logicalPin, int level);
esp_err_t GpioCtrl_Pulse(int logicalPin, int level, uint64_t widthUs);
bool GpioCtrl_IsOutputCapable(int logicalPin);
const char* GpioCtrl_ModeToString(GpioCtrl_Mode mode);
bool GpioCtrl_ModeFromString(const char* s, GpioCtrl_Mode* out);
bool GpioCtrl_EdgeFromString(const char* s, GpioCtrl_Edge* out);
const char* GpioCtrl_EdgeToString(GpioCtrl_Edge edge);

esp_err_t GpioCtrl_GetPowerEnable(GpioCtrl_PowerRail rail, bool* enable);
esp_err_t GpioCtrl_SetPowerEnable(GpioCtrl_PowerRail rail, bool enable);
bool GpioCtrl_PowerRailFromString(const char* s, GpioCtrl_PowerRail* out);
const char* GpioCtrl_PowerRailToString(GpioCtrl_PowerRail rail);

/**
 * Blocking capture. eventsOut capacity is maxEvents.
 * includePin: if true, fill event.pin; else leave pin as -1.
 */
esp_err_t GpioCtrl_Trace(const int* logicalPins, size_t pinCount, GpioCtrl_Edge edge, uint64_t durationUs,
                         GpioCtrl_TraceEvent* eventsOut, size_t maxEvents, size_t* eventCountOut, bool includePin);

#endif
