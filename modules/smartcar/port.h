// SPDX-License-Identifier: MIT
#ifndef SMARTCAR_PORT_H
#define SMARTCAR_PORT_H
#include "py/runtime.h"

// Port drivers allocate only in init/add/configure, never in capture.
// Capture returns 0 on success, or a positive errno. No Python exceptions in IRQ.
void *sc_adc_init(int id);
void sc_adc_add(void *ctx, mp_obj_t pin);
void sc_adc_configure(void *ctx, int sample_mode, int average);
int sc_adc_capture(void *ctx, uint16_t *values);
void sc_adc_deinit(void *ctx);
// Soft reset only, after stopping ticker IRQs; no allocations or exceptions.
// False means hardware stop failed; resource roots have still been released.
bool sc_adc_shutdown(void *ctx);

void *sc_encoder_init(mp_obj_t a, mp_obj_t b, bool invert);
int sc_encoder_capture(void *ctx, int32_t *value);
void sc_encoder_deinit(void *ctx);

// Claims shared with ticker and encoder. Non-NULL owners must match on release.
void sc_timer_claim(unsigned id, void *owner);
void sc_timer_release(unsigned id, void *owner);
void sc_pin_claim(mp_obj_t pin, void *owner);
void sc_pin_release_all(void *owner);
void sc_adc_claim(unsigned id, void *owner);
void sc_adc_release(unsigned id, void *owner);

#ifdef UNIX
void sc_sim_adc_set(void *ctx, size_t n, const mp_obj_t *values);
void sc_sim_encoder_step(void *ctx, int32_t step);
#endif
#endif
