// SPDX-License-Identifier: MIT
#ifndef SMARTCAR_H
#define SMARTCAR_H
#include <stdbool.h>
#include "py/obj.h"
bool sc_sensor_is_running(mp_obj_t obj);
bool sc_timer_is_claimed(unsigned id);
bool sc_adc_is_claimed(unsigned id);
void smartcar_irq(void);
void smartcar_deinit(void);
#endif
