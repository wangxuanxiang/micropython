// SPDX-License-Identifier: MIT
// Python bindings and ticker integration. Hardware transactions live in imu_port.c.
#include <string.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/objlist.h"
#include "py/mperrno.h"
#include "seekfree.h"
#include "imu_port.h"
#include "../smartcar/smartcar.h"

typedef struct _imu660_t {
    mp_obj_base_t base;
    void *ctx;
    mp_obj_t data;
    mp_obj_t cs;
    uint16_t capture_div, count;
    unsigned spi_id;
    bool attached, closed;
    int error;
} imu660_t;
static const mp_obj_type_t imu660_type;
MP_REGISTER_ROOT_POINTER(mp_obj_t seekfree_imu_root);

bool seekfree_sensor_is(mp_obj_t obj) { return mp_obj_is_type(obj, &imu660_type); }
static imu660_t *imu_check(mp_obj_t obj) {
    if (!seekfree_sensor_is(obj)) { mp_raise_TypeError(MP_ERROR_TEXT("expected IMU660RX")); }
    imu660_t *imu = MP_OBJ_TO_PTR(obj);
    if (imu->closed) { mp_raise_ValueError(MP_ERROR_TEXT("IMU660 deinitialised")); }
    return imu;
}
bool seekfree_sensor_attached(mp_obj_t obj) { return imu_check(obj)->attached; }
void seekfree_sensor_attach(mp_obj_t obj) { imu_check(obj)->attached = true; }
void seekfree_sensor_detach(mp_obj_t obj) { ((imu660_t *)MP_OBJ_TO_PTR(obj))->attached = false; }

// Called only with a checked, rooted native object. No allocation or exception.
static int imu_sample(imu660_t *imu) {
    int16_t values[6];
    int error = imu_port_capture(imu->ctx, values);
    if (!error) {
        // The original demo retains get()'s list. Refresh that same list with
        // immediate small integers; users must not resize it.
        mp_obj_list_t *list = MP_OBJ_TO_PTR(imu->data);
        mp_uint_t atomic = MICROPY_BEGIN_ATOMIC_SECTION();
        if (list->len != 6) {
            error = MP_EINVAL;
        } else {
            for (unsigned i = 0; i < 6; ++i) { list->items[i] = MP_OBJ_NEW_SMALL_INT(values[i]); }
        }
        MICROPY_END_ATOMIC_SECTION(atomic);
    }
    imu->error = error;
    return error;
}
int seekfree_sensor_capture(mp_obj_t obj) {
    imu660_t *imu = MP_OBJ_TO_PTR(obj);
    if (imu->closed) { return MP_ENODEV; }
    if (++imu->count < imu->capture_div) { return imu->error; }
    imu->count = 0;
    return imu_sample(imu);
}
mp_obj_t seekfree_sensor_get(mp_obj_t obj) {
    imu660_t *imu = imu_check(obj);
    if (imu->error) { mp_raise_OSError(imu->error); }
    return imu->data;
}
static void imu_foreground_check(imu660_t *imu) {
    if (sc_sensor_is_running(MP_OBJ_FROM_PTR(imu))) { mp_raise_OSError(MP_EBUSY); }
}
static mp_obj_t imu_capture(mp_obj_t obj) {
    imu660_t *imu = imu_check(obj);
    imu_foreground_check(imu);
    int error = seekfree_sensor_capture(obj);
    if (error) { mp_raise_OSError(error); }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(imu_capture_obj, imu_capture);
static MP_DEFINE_CONST_FUN_OBJ_1(imu_get_obj, seekfree_sensor_get);
static mp_obj_t imu_read(mp_obj_t obj) {
    imu660_t *imu = imu_check(obj);
    imu_foreground_check(imu);
    // read() forces one physical read but does not consume a divided capture.
    int error = imu_sample(imu);
    if (error) { mp_raise_OSError(error); }
    return imu->data;
}
static MP_DEFINE_CONST_FUN_OBJ_1(imu_read_obj, imu_read);
static mp_obj_t imu_info(mp_obj_t obj) {
    imu660_t *imu = imu_check(obj);
    mp_printf(&mp_plat_print, "IMU660RX model=%s SPI%u CS=", imu_port_model(imu->ctx), imu->spi_id);
    mp_obj_print_helper(&mp_plat_print, imu->cs, PRINT_STR);
    mp_printf(&mp_plat_print, " capture_div=%u range=+/-8g,+/-2000dps error=%d\n", imu->capture_div, imu->error);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(imu_info_obj, imu_info);
static mp_obj_t imu_help(size_t n, const mp_obj_t *args) {
    (void)n; (void)args;
    mp_print_str(&mp_plat_print,
        "IMU660RX(capture_div=1, *, spi=2, cs='B12')\n"
        "Models: IMU660RA (BMI270), IMU660RB, IMU660RC; +/-8g, +/-2000dps\n"
        "RA output: 50/200Hz; RB output: 52/208Hz (acc/gyro)\n"
        "capture(): divided sampling; read(): immediate; get(): live six-int list\n"
        "data=[acc_x,acc_y,acc_z,gyro_x,gyro_y,gyro_z]; scales(): LSB/g, LSB/dps\n"
        "Default SPI2: B13 SCK, B14 MISO, B15 MOSI, B12 CS; 3.3V\n"
        "Do not resize the live list; copy with list(imu.get()) for history.\n");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(imu_help_obj, 0, 1, imu_help);
static mp_obj_t imu_scales(mp_obj_t obj) {
    imu660_t *imu = imu_check(obj);
    mp_obj_t values[2] = {mp_obj_new_float(imu_port_acc_scale(imu->ctx)), mp_obj_new_float(imu_port_gyro_scale(imu->ctx))};
    return mp_obj_new_tuple(2, values);
}
static MP_DEFINE_CONST_FUN_OBJ_1(imu_scales_obj, imu_scales);
static mp_obj_t imu_deinit(mp_obj_t obj) {
    imu660_t *imu = MP_OBJ_TO_PTR(obj);
    if (imu->closed) { return mp_const_none; }
    if (imu->attached) { mp_raise_OSError(MP_EBUSY); }
    imu_port_deinit(imu->ctx);
    imu->closed = true;
    if (MP_STATE_VM(seekfree_imu_root) == obj) { MP_STATE_VM(seekfree_imu_root) = MP_OBJ_NULL; }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(imu_deinit_obj, imu_deinit);
static mp_obj_t imu_make_new(const mp_obj_type_t *type, size_t n, size_t nkw, const mp_obj_t *args) {
    static const mp_arg_t allowed[] = {
        {MP_QSTR_capture_div, MP_ARG_INT, {.u_int = 1}},
        {MP_QSTR_spi, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 2}},
        {MP_QSTR_cs, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_B12)}},
    };
    mp_arg_val_t v[3];
    mp_arg_parse_all_kw_array(n, nkw, args, 3, allowed, v);
    if (v[0].u_int < 1 || v[0].u_int > 65535) { mp_raise_ValueError(MP_ERROR_TEXT("capture_div must be 1..65535")); }
    if (v[1].u_int != 2 && v[1].u_int != 4) { mp_raise_ValueError(MP_ERROR_TEXT("IMU requires SPI2 or SPI4")); }
    if (MP_STATE_VM(seekfree_imu_root) != MP_OBJ_NULL) { mp_raise_OSError(MP_EBUSY); }
    imu660_t *imu = mp_obj_malloc(imu660_t, type);
    memset((char *)imu + sizeof(imu->base), 0, sizeof(*imu) - sizeof(imu->base));
    imu->capture_div = v[0].u_int;
    imu->spi_id = v[1].u_int;
    imu->cs = v[2].u_obj;
    mp_obj_t zero[6] = {MP_OBJ_NEW_SMALL_INT(0),MP_OBJ_NEW_SMALL_INT(0),MP_OBJ_NEW_SMALL_INT(0),MP_OBJ_NEW_SMALL_INT(0),MP_OBJ_NEW_SMALL_INT(0),MP_OBJ_NEW_SMALL_INT(0)};
    imu->data = mp_obj_new_list(6, zero);
    // Root before init's delays can run scheduled work, preventing reentrancy.
    MP_STATE_VM(seekfree_imu_root) = MP_OBJ_FROM_PTR(imu);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        imu->ctx = imu_port_init(imu->spi_id, imu->cs);
        nlr_pop();
    } else {
        MP_STATE_VM(seekfree_imu_root) = MP_OBJ_NULL;
        nlr_jump(nlr.ret_val);
    }
    return MP_OBJ_FROM_PTR(imu);
}
#ifdef UNIX
static mp_obj_t imu_sim_set(mp_obj_t obj, mp_obj_t seq) {
    imu660_t *imu = imu_check(obj);
    size_t n; mp_obj_t *items;
    mp_obj_get_array(seq, &n, &items);
    imu_port_set(imu->ctx, n, items);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(imu_sim_set_obj, imu_sim_set);
static mp_obj_t imu_sim_fault(mp_obj_t obj, mp_obj_t error_obj) {
    imu660_t *imu = imu_check(obj);
    mp_int_t error = mp_obj_get_int(error_obj);
    if (error < 0 || error > 255) { mp_raise_ValueError(MP_ERROR_TEXT("invalid errno")); }
    imu_port_fault(imu->ctx, error);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(imu_sim_fault_obj, imu_sim_fault);
static mp_obj_t imu_sim_config(mp_obj_t model_obj, mp_obj_t fault_obj) {
    mp_int_t model = mp_obj_get_int(model_obj), fault = mp_obj_get_int(fault_obj);
    if (model < 0 || model > 3 || fault < 0 || fault > 255) { mp_raise_ValueError(MP_ERROR_TEXT("invalid simulated device")); }
    imu_port_sim_config(model, fault);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(imu_sim_config_obj, imu_sim_config);
#endif
static const mp_rom_map_elem_t imu_locals[] = {
    {MP_ROM_QSTR(MP_QSTR_capture), MP_ROM_PTR(&imu_capture_obj)},
    {MP_ROM_QSTR(MP_QSTR_get), MP_ROM_PTR(&imu_get_obj)},
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&imu_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&imu_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_scales), MP_ROM_PTR(&imu_scales_obj)},
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&imu_deinit_obj)},
    {MP_ROM_QSTR(MP_QSTR_help), MP_ROM_PTR(&imu_help_obj)},
};
static MP_DEFINE_CONST_DICT(imu_locals_dict, imu_locals);
static MP_DEFINE_CONST_OBJ_TYPE(imu660_type, MP_QSTR_IMU660RX, MP_TYPE_FLAG_NONE, make_new, imu_make_new, locals_dict, &imu_locals_dict);
static const mp_rom_map_elem_t seekfree_globals[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_seekfree)},
    {MP_ROM_QSTR(MP_QSTR_IMU660RX), MP_ROM_PTR(&imu660_type)},
    #ifdef UNIX
    {MP_ROM_QSTR(MP_QSTR__imu660_set), MP_ROM_PTR(&imu_sim_set_obj)},
    {MP_ROM_QSTR(MP_QSTR__imu660_fault), MP_ROM_PTR(&imu_sim_fault_obj)},
    {MP_ROM_QSTR(MP_QSTR__imu660_config), MP_ROM_PTR(&imu_sim_config_obj)},
    #endif
};
static MP_DEFINE_CONST_DICT(seekfree_globals_dict, seekfree_globals);
const mp_obj_module_t seekfree_module = {.base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&seekfree_globals_dict};
MP_REGISTER_MODULE(MP_QSTR_seekfree, seekfree_module);
void seekfree_deinit(void) {
    mp_obj_t obj = MP_STATE_VM(seekfree_imu_root);
    if (obj != MP_OBJ_NULL) {
        imu660_t *imu = MP_OBJ_TO_PTR(obj);
        imu->attached = false;
        if (imu->ctx) { imu_deinit(obj); }
    }
}
