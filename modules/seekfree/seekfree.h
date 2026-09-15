#ifndef SEEKFREE_H
#define SEEKFREE_H
#include "py/runtime.h"
bool seekfree_sensor_is(mp_obj_t obj);
int seekfree_sensor_capture(mp_obj_t obj);
mp_obj_t seekfree_sensor_get(mp_obj_t obj);
bool seekfree_sensor_attached(mp_obj_t obj);
void seekfree_sensor_attach(mp_obj_t obj);
void seekfree_sensor_detach(mp_obj_t obj);
void seekfree_deinit(void);
#endif
