// SPDX-License-Identifier: MIT
#ifndef SMARTCAR_H
#define SMARTCAR_H
#include <stdbool.h>
bool sc_timer_is_claimed(unsigned id);
bool sc_adc_is_claimed(unsigned id);
void smartcar_irq(void);
void smartcar_deinit(void);
#endif
