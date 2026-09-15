// SPDX-License-Identifier: MIT
#ifndef MICROPY_INCLUDED_SEEKFREE_IMU_PORT_H
#define MICROPY_INCLUDED_SEEKFREE_IMU_PORT_H
#include "py/obj.h"
void *imu_port_init(int spi_id, mp_obj_t cs);
int imu_port_capture(void *ctx, int16_t out[6]);
void imu_port_deinit(void *ctx);
const char *imu_port_model(void *ctx);
// Divisors converting raw readings to g and degrees/second.
float imu_port_acc_scale(void *ctx);
float imu_port_gyro_scale(void *ctx);
bool seekfree_spi_claimed(unsigned id);
#ifdef UNIX
void imu_port_set(void *ctx, size_t count, const mp_obj_t *values);
void imu_port_fault(void *ctx, int error);
void imu_port_sim_config(int model, int error);
#endif
#endif
