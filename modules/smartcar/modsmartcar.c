// SPDX-License-Identifier: MIT
// SeekFree-style interfaces; native acquisition precedes deferred Python callbacks.
#include <string.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/objlist.h"
#include "py/objstr.h"
#include "py/mperrno.h"
#include "port.h"
#include "smartcar.h"
#if MICROPY_PY_SEEKFREE
#include "../seekfree/seekfree.h"
#endif
#ifndef UNIX
#include "pin.h"
#include "timer.h"
#include "irq.h"
#endif

typedef struct _sc_sensor_t {
    mp_obj_base_t base;
    void *ctx;
    int id, count, attached, error;
    bool closed;
    uint16_t adc[16];
    int32_t encoder;
} sc_sensor_t;
typedef struct _sc_ticker_t sc_ticker_t;
typedef struct {
    mp_obj_base_t base;
    sc_ticker_t *ticker;
    mp_obj_t callback;
    volatile bool pending;
} sc_dispatch_t;
struct _sc_ticker_t {
    mp_obj_base_t base;
    int id;
    volatile bool running;
    uint32_t period, remaining, ticks;
    int error;
    sc_dispatch_t *dispatch;
    mp_obj_t callback;
    size_t count;
    mp_obj_t sensors[8];
};
static qstr sc_pin_keys[64];
MP_REGISTER_ROOT_POINTER(void *sc_timer_owners[18]);
MP_REGISTER_ROOT_POINTER(void *sc_adc_owners[3]);
MP_REGISTER_ROOT_POINTER(void *sc_pin_owners[64]);
MP_REGISTER_ROOT_POINTER(struct _sc_ticker_t *sc_tickers[4]);
MP_REGISTER_ROOT_POINTER(mp_obj_t sc_sensor_roots[16]);
static const mp_obj_type_t sc_ticker_type, sc_adc_type, sc_encoder_type, sc_dispatch_type;

bool sc_timer_is_claimed(unsigned id) {
    return id < 18 && MP_STATE_VM(sc_timer_owners)[id] != NULL;
}
bool sc_adc_is_claimed(unsigned id) {
    return id < 3 && MP_STATE_VM(sc_adc_owners)[id] != NULL;
}
void sc_timer_claim(unsigned id, void *owner) {
    if (id == 0 || id >= 18 || id == 5 || id == 6) {
        mp_raise_ValueError(MP_ERROR_TEXT("timer reserved"));
    }
    if (sc_timer_is_claimed(id)) {
        mp_raise_OSError(MP_EBUSY);
    }
    #ifndef UNIX
    TIM_TypeDef *tim = timer_id_to_reg(id);
    if (tim == NULL || (tim->CR1 & TIM_CR1_CEN)) {
        mp_raise_OSError(MP_EBUSY);
    }
    #endif
    MP_STATE_VM(sc_timer_owners)[id] = owner;
}
void sc_timer_release(unsigned id, void *owner) {
    if (id < 18 && MP_STATE_VM(sc_timer_owners)[id] == owner) {
        MP_STATE_VM(sc_timer_owners)[id] = NULL;
    }
}
void sc_adc_claim(unsigned id, void *owner) {
    if (id < 1 || id > 2) { mp_raise_ValueError(MP_ERROR_TEXT("ADC id must be 1 or 2")); }
    if (sc_adc_is_claimed(id)) { mp_raise_OSError(MP_EBUSY); }
    MP_STATE_VM(sc_adc_owners)[id] = owner;
}
void sc_adc_release(unsigned id, void *owner) {
    if (id < 3 && MP_STATE_VM(sc_adc_owners)[id] == owner) { MP_STATE_VM(sc_adc_owners)[id] = NULL; }
}
void sc_pin_claim(mp_obj_t pin, void *owner) {
    #ifdef UNIX
    const char *name = mp_obj_str_get_str(pin);
    if (name[0] == 'P') { ++name; }
    qstr key = qstr_from_str(name);
    #else
    qstr key = pin_find(pin)->name;
    const char *name = qstr_str(key);
    #endif
    // Board storage, debug, oscillator and USB pins are never general-purpose here.
    const char *reserved[] = {"B2", "B3", "B4", "B6", "D6", "D7", "D11", "D12", "D13", "E2", "A11", "A12", "A13", "A14", "C14", "C15", "H0", "H1"};
    for (size_t i = 0; i < MP_ARRAY_SIZE(reserved); ++i) {
        if (!strcmp(name, reserved[i])) { mp_raise_ValueError(MP_ERROR_TEXT("pin reserved by board")); }
    }
    int free_slot = -1;
    for (size_t i = 0; i < 64; ++i) {
        void *owner_slot = MP_STATE_VM(sc_pin_owners)[i];
        if (owner_slot && sc_pin_keys[i] == key) { mp_raise_OSError(MP_EBUSY); }
        if (!owner_slot) { free_slot = i; }
    }
    if (free_slot < 0) { mp_raise_OSError(MP_ENOSPC); }
    sc_pin_keys[free_slot] = key;
    MP_STATE_VM(sc_pin_owners)[free_slot] = owner;
}
void sc_pin_release_all(void *owner) {
    for (size_t i = 0; i < 64; ++i) {
        if (MP_STATE_VM(sc_pin_owners)[i] == owner) { MP_STATE_VM(sc_pin_owners)[i] = NULL; }
    }
}
static int sensor_root_slot(void) {
    for (int i = 0; i < 16; ++i) {
        if (MP_STATE_VM(sc_sensor_roots)[i] == MP_OBJ_NULL) { return i; }
    }
    mp_raise_OSError(MP_ENOSPC);
}
static sc_sensor_t *sensor_check(mp_obj_t obj) {
    if (!mp_obj_is_type(obj, &sc_adc_type) && !mp_obj_is_type(obj, &sc_encoder_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected native ADC_Group or encoder"));
    }
    sc_sensor_t *s = MP_OBJ_TO_PTR(obj);
    if (s->closed) { mp_raise_ValueError(MP_ERROR_TEXT("sensor deinitialised")); }
    return s;
}
static void sensor_capture_native_obj(mp_obj_t obj) {
    #if MICROPY_PY_SEEKFREE
    if (seekfree_sensor_is(obj)) {
        seekfree_sensor_capture(obj);
        return;
    }
    #endif
    sc_sensor_t *s = MP_OBJ_TO_PTR(obj);
    if (s->base.type == &sc_adc_type) {
        s->error = sc_adc_capture(s->ctx, s->adc);
    } else {
        s->error = sc_encoder_capture(s->ctx, &s->encoder);
    }
}
bool sc_sensor_is_running(mp_obj_t obj) {
    for (int i = 0; i < 4; ++i) {
        sc_ticker_t *t = MP_STATE_VM(sc_tickers)[i];
        if (t && t->running) {
            for (size_t j = 0; j < t->count; ++j) {
                if (t->sensors[j] == obj) { return true; }
            }
        }
    }
    return false;
}
static mp_obj_t sensor_capture(mp_obj_t obj) {
    sc_sensor_t *s = sensor_check(obj);
    if (sc_sensor_is_running(obj)) { mp_raise_OSError(MP_EBUSY); }
    sensor_capture_native_obj(obj);
    if (s->error) { mp_raise_OSError(s->error); }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sensor_capture_obj, sensor_capture);
static mp_obj_t sensor_get(mp_obj_t obj) {
    sc_sensor_t *s = sensor_check(obj);
    uint16_t values[16];
    mp_uint_t atomic = MICROPY_BEGIN_ATOMIC_SECTION();
    int error = s->error;
    int32_t enc = s->encoder;
    memcpy(values, s->adc, sizeof(values));
    MICROPY_END_ATOMIC_SECTION(atomic);
    if (error) { mp_raise_OSError(error); }
    if (s->base.type == &sc_encoder_type) { return mp_obj_new_int(enc); }
    mp_obj_t items[16];
    for (int i = 0; i < s->count; ++i) { items[i] = MP_OBJ_NEW_SMALL_INT(values[i]); }
    return mp_obj_new_list(s->count, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sensor_get_obj, sensor_get);
static mp_obj_t sensor_read(mp_obj_t obj) { sensor_capture(obj); return sensor_get(obj); }
static MP_DEFINE_CONST_FUN_OBJ_1(sensor_read_obj, sensor_read);
static mp_obj_t sensor_deinit(mp_obj_t obj) {
    sc_sensor_t *s = MP_OBJ_TO_PTR(obj);
    if (s->closed) { return mp_const_none; }
    if (s->attached) { mp_raise_OSError(MP_EBUSY); }
    if (s->base.type == &sc_adc_type) { sc_adc_deinit(s->ctx); } else { sc_encoder_deinit(s->ctx); }
    s->closed = true;
    for (int i = 0; i < 16; ++i) {
        if (MP_STATE_VM(sc_sensor_roots)[i] == obj) { MP_STATE_VM(sc_sensor_roots)[i] = MP_OBJ_NULL; }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sensor_deinit_obj, sensor_deinit);
static mp_obj_t adc_new(const mp_obj_type_t *type, size_t n, size_t nkw, const mp_obj_t *args) {
    mp_arg_check_num(n, nkw, 1, 1, false);
    mp_int_t id = mp_obj_get_int(args[0]);
    if (id < 1 || id > 2) { mp_raise_ValueError(MP_ERROR_TEXT("ADC id must be 1 or 2")); }
    int slot = sensor_root_slot();
    sc_sensor_t *s = mp_obj_malloc(sc_sensor_t, type);
    memset((char *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->id = id;
    s->ctx = sc_adc_init(id);
    MP_STATE_VM(sc_sensor_roots)[slot] = MP_OBJ_FROM_PTR(s);
    return MP_OBJ_FROM_PTR(s);
}
static mp_obj_t adc_add(mp_obj_t obj, mp_obj_t pin) {
    sc_sensor_t *s = sensor_check(obj);
    if (s->attached) { mp_raise_OSError(MP_EBUSY); }
    sc_adc_add(s->ctx, pin);
    ++s->count;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(adc_add_obj, adc_add);
static mp_obj_t adc_init(size_t n, const mp_obj_t *args, mp_map_t *kw) {
    sc_sensor_t *s = sensor_check(args[0]);
    const mp_arg_t allowed[] = {
        {MP_QSTR_id, MP_ARG_INT, {.u_int = s->id}},
        {MP_QSTR_period, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 3}},
        {MP_QSTR_average, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 16}},
    };
    mp_arg_val_t vals[3];
    mp_arg_parse_all(n - 1, args + 1, kw, 3, allowed, vals);
    if (s->attached) { mp_raise_OSError(MP_EBUSY); }
    if (vals[0].u_int != s->id) { mp_raise_ValueError(MP_ERROR_TEXT("cannot change ADC id")); }
    if (vals[1].u_int < 0 || vals[1].u_int > 3 || vals[2].u_int < 1 || vals[2].u_int > 32) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ADC sampling mode or average"));
    }
    sc_adc_configure(s->ctx, vals[1].u_int, vals[2].u_int);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(adc_init_obj, 1, adc_init);
#define SC_SENSOR_METHODS \
    {MP_ROM_QSTR(MP_QSTR_capture), MP_ROM_PTR(&sensor_capture_obj)}, \
    {MP_ROM_QSTR(MP_QSTR_get), MP_ROM_PTR(&sensor_get_obj)}, \
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&sensor_read_obj)}, \
    {MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&sensor_deinit_obj)}
static const mp_rom_map_elem_t adc_locals_table[] = {
    SC_SENSOR_METHODS,
    {MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&adc_init_obj)},
    {MP_ROM_QSTR(MP_QSTR_addch), MP_ROM_PTR(&adc_add_obj)},
    {MP_ROM_QSTR(MP_QSTR_PMODE0), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_PMODE1), MP_ROM_INT(1)},
    {MP_ROM_QSTR(MP_QSTR_PMODE2), MP_ROM_INT(2)},
    {MP_ROM_QSTR(MP_QSTR_PMODE3), MP_ROM_INT(3)},
    {MP_ROM_QSTR(MP_QSTR_AVG1), MP_ROM_INT(1)},
    {MP_ROM_QSTR(MP_QSTR_AVG4), MP_ROM_INT(4)},
    {MP_ROM_QSTR(MP_QSTR_AVG8), MP_ROM_INT(8)},
    {MP_ROM_QSTR(MP_QSTR_AVG16), MP_ROM_INT(16)},
    {MP_ROM_QSTR(MP_QSTR_AVG32), MP_ROM_INT(32)},
};
static MP_DEFINE_CONST_DICT(adc_locals, adc_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(sc_adc_type, MP_QSTR_ADC_Group, MP_TYPE_FLAG_NONE,
    make_new, adc_new, locals_dict, &adc_locals);

static mp_obj_t encoder_new(const mp_obj_type_t *type, size_t n, size_t nkw, const mp_obj_t *args) {
    enum {A, B, INVERT};
    static const mp_arg_t allowed[] = {
        {MP_QSTR_PhaseA, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_PhaseB, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_invert, MP_ARG_BOOL, {.u_bool = false}},
    };
    mp_arg_val_t vals[3];
    mp_arg_parse_all_kw_array(n, nkw, args, 3, allowed, vals);
    int slot = sensor_root_slot();
    sc_sensor_t *s = mp_obj_malloc(sc_sensor_t, type);
    memset((char *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
    s->ctx = sc_encoder_init(vals[A].u_obj, vals[B].u_obj, vals[INVERT].u_bool);
    MP_STATE_VM(sc_sensor_roots)[slot] = MP_OBJ_FROM_PTR(s);
    return MP_OBJ_FROM_PTR(s);
}
static const mp_rom_map_elem_t encoder_locals_table[] = {SC_SENSOR_METHODS};
static MP_DEFINE_CONST_DICT(encoder_locals, encoder_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(sc_encoder_type, MP_QSTR_encoder, MP_TYPE_FLAG_NONE,
    make_new, encoder_new, locals_dict, &encoder_locals);

static mp_obj_t dispatch_call(mp_obj_t obj, size_t n, size_t nkw, const mp_obj_t *args) {
    (void)n; (void)nkw; (void)args;
    sc_dispatch_t *d = MP_OBJ_TO_PTR(obj);
    sc_ticker_t *t = d->ticker;
    if (t->running && t->dispatch == d) {
        mp_call_function_1_protected(d->callback, MP_OBJ_FROM_PTR(t));
    }
    d->pending = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_OBJ_TYPE(sc_dispatch_type, MP_QSTR_function, MP_TYPE_FLAG_NONE, call, dispatch_call);

static void ticker_hw_idle(void);
void smartcar_irq(void) {
    #ifndef UNIX
    uint32_t started = mp_hal_ticks_cpu();
    #endif
    // All four logical channels run serially under the same TIM7 IRQ.
    for (int i = 0; i < 4; ++i) {
        sc_ticker_t *t = MP_STATE_VM(sc_tickers)[i];
        if (!t || !t->running || --t->remaining) { continue; }
        t->remaining = t->period;
        ++t->ticks;
        for (size_t j = 0; j < t->count; ++j) { sensor_capture_native_obj(t->sensors[j]); }
        sc_dispatch_t *d = t->dispatch;
        if (!d->pending) {
            d->pending = true;
            if (!mp_sched_schedule(MP_OBJ_FROM_PTR(d), mp_const_none)) { d->pending = false; }
        }
    }
    #ifndef UNIX
    // A capture exceeding the base tick can cause permanent IRQ tail-chaining.
    // Fail closed instead of starving the VM (which must remain able to stop).
    if ((uint32_t)(mp_hal_ticks_cpu() - started) >= SystemCoreClock / 1000) {
        for (int i = 0; i < 4; ++i) {
            sc_ticker_t *t = MP_STATE_VM(sc_tickers)[i];
            if (t && t->running) {
                t->running = false;
                t->error = MP_ETIMEDOUT;
                t->dispatch = NULL;
            }
        }
        ticker_hw_idle();
    }
    #endif
}
static void ticker_hw_start(sc_ticker_t *owner) {
    if (sc_timer_is_claimed(7)) { return; }
    sc_timer_claim(7, owner);
    #ifndef UNIX
    mp_hal_ticks_cpu_enable();
    timer_clock_enable(7);
    TIM7->CR1 = 0;
    uint32_t hz = timer_get_source_freq(7);
    TIM7->PSC = hz / 1000000 - 1;
    TIM7->ARR = 999;
    TIM7->EGR = TIM_EGR_UG;
    TIM7->SR = 0;
    TIM7->DIER = TIM_DIER_UIE;
    NVIC_SetPriority(TIM7_IRQn, IRQ_PRI_TIMX);
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    NVIC_EnableIRQ(TIM7_IRQn);
    TIM7->CR1 = TIM_CR1_CEN;
    #endif
}
static void ticker_hw_idle(void) {
    for (int i = 0; i < 4; ++i) {
        sc_ticker_t *t = MP_STATE_VM(sc_tickers)[i];
        if (t && t->running) { return; }
    }
    #ifndef UNIX
    if (sc_timer_is_claimed(7)) {
        TIM7->DIER = 0;
        TIM7->CR1 = 0;
        NVIC_DisableIRQ(TIM7_IRQn);
        NVIC_ClearPendingIRQ(TIM7_IRQn);
    }
    #endif
    MP_STATE_VM(sc_timer_owners)[7] = NULL;
}
static mp_obj_t ticker_new(const mp_obj_type_t *type, size_t n, size_t nkw, const mp_obj_t *args) {
    mp_arg_check_num(n, nkw, 1, 1, false);
    mp_int_t id = mp_obj_get_int(args[0]);
    if (id < 0 || id > 3) { mp_raise_ValueError(MP_ERROR_TEXT("ticker id must be 0..3")); }
    sc_ticker_t *t = MP_STATE_VM(sc_tickers)[id];
    if (!t) {
        t = mp_obj_malloc(sc_ticker_t, type);
        memset((char *)t + sizeof(t->base), 0, sizeof(*t) - sizeof(t->base));
        t->id = id;
        t->callback = mp_const_none;
        MP_STATE_VM(sc_tickers)[id] = t;
    }
    return MP_OBJ_FROM_PTR(t);
}
static mp_obj_t ticker_stop(mp_obj_t obj) {
    sc_ticker_t *t = MP_OBJ_TO_PTR(obj);
    mp_uint_t atomic = MICROPY_BEGIN_ATOMIC_SECTION();
    t->running = false;
    t->dispatch = NULL;
    ticker_hw_idle();
    MICROPY_END_ATOMIC_SECTION(atomic);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ticker_stop_obj, ticker_stop);
static mp_obj_t ticker_start(mp_obj_t obj, mp_obj_t period) {
    sc_ticker_t *t = MP_OBJ_TO_PTR(obj);
    mp_int_t ms = mp_obj_get_int(period);
    if (ms < 1 || ms > 0x3fffffff) { mp_raise_ValueError(MP_ERROR_TEXT("invalid period in ms")); }
    if (!mp_obj_is_callable(t->callback)) { mp_raise_ValueError(MP_ERROR_TEXT("set callback before start")); }
    sc_dispatch_t *d = mp_obj_malloc(sc_dispatch_t, &sc_dispatch_type);
    d->ticker = t; d->callback = t->callback; d->pending = false;
    mp_uint_t atomic = MICROPY_BEGIN_ATOMIC_SECTION();
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        // Ensure TIM7 and publish this run atomically: an overload IRQ must
        // not disable the hardware between those two operations.
        ticker_hw_start(t);
        nlr_pop();
    } else {
        MICROPY_END_ATOMIC_SECTION(atomic);
        nlr_jump(nlr.ret_val);
    }
    t->period = ms; t->remaining = ms; t->ticks = 0; t->error = 0;
    t->dispatch = d; t->running = true;
    MICROPY_END_ATOMIC_SECTION(atomic);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ticker_start_obj, ticker_start);
static mp_obj_t ticker_callback(mp_obj_t obj, mp_obj_t cb) {
    sc_ticker_t *t = MP_OBJ_TO_PTR(obj);
    if (!mp_obj_is_callable(cb) && cb != mp_const_none) { mp_raise_TypeError(MP_ERROR_TEXT("callback must be callable or None")); }
    // Changing callback stops this run; call start() to arm the replacement.
    ticker_stop(obj);
    t->callback = cb;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ticker_callback_obj, ticker_callback);
static mp_obj_t ticker_ticks(mp_obj_t obj) {
    sc_ticker_t *t = MP_OBJ_TO_PTR(obj);
    if (t->error) { mp_raise_OSError(t->error); }
    return mp_obj_new_int_from_uint(t->ticks);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ticker_ticks_obj, ticker_ticks);
static mp_obj_t ticker_captures(size_t n, const mp_obj_t *args) {
    sc_ticker_t *t = MP_OBJ_TO_PTR(args[0]);
    if (n > 9) { mp_raise_ValueError(MP_ERROR_TEXT("maximum 8 captures")); }
    if (t->running) { mp_raise_OSError(MP_EBUSY); }
    mp_obj_t sensors[8];
    for (size_t i = 1; i < n; ++i) {
        sensors[i - 1] = args[i];
        sc_sensor_t *s = NULL;
        bool is_seekfree = false;
        #if MICROPY_PY_SEEKFREE
        is_seekfree = seekfree_sensor_is(sensors[i - 1]);
        #endif
        if (!is_seekfree) { s = sensor_check(sensors[i - 1]); }
        if (s && s->base.type == &sc_adc_type && !s->count) { mp_raise_ValueError(MP_ERROR_TEXT("ADC group has no channels")); }
        for (size_t j = 1; j < i; ++j) {
            if (sensors[j - 1] == sensors[i - 1]) { mp_raise_ValueError(MP_ERROR_TEXT("duplicate capture")); }
        }
        bool ours = false;
        for (size_t j = 0; j < t->count; ++j) { ours |= t->sensors[j] == sensors[i - 1]; }
        #if MICROPY_PY_SEEKFREE
        if (is_seekfree) {
            if (seekfree_sensor_attached(sensors[i - 1]) && !ours) { mp_raise_OSError(MP_EBUSY); }
        } else
        #endif
        if (s->attached && !ours) { mp_raise_OSError(MP_EBUSY); }
    }
    for (size_t j = 0; j < t->count; ++j) {
        #if MICROPY_PY_SEEKFREE
        if (seekfree_sensor_is(t->sensors[j])) { seekfree_sensor_detach(t->sensors[j]); }
        else
        #endif
        { --((sc_sensor_t *)MP_OBJ_TO_PTR(t->sensors[j]))->attached; }
        t->sensors[j] = MP_OBJ_NULL;
    }
    t->count = n - 1;
    for (size_t j = 0; j < t->count; ++j) {
        t->sensors[j] = sensors[j];
        #if MICROPY_PY_SEEKFREE
        if (seekfree_sensor_is(sensors[j])) { seekfree_sensor_attach(sensors[j]); }
        else
        #endif
        { ++((sc_sensor_t *)MP_OBJ_TO_PTR(sensors[j]))->attached; }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ticker_captures_obj, 1, MP_OBJ_FUN_ARGS_MAX, ticker_captures);
static const mp_rom_map_elem_t ticker_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&ticker_start_obj)},
    {MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&ticker_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_callback), MP_ROM_PTR(&ticker_callback_obj)},
    {MP_ROM_QSTR(MP_QSTR_ticks), MP_ROM_PTR(&ticker_ticks_obj)},
    {MP_ROM_QSTR(MP_QSTR_capture_list), MP_ROM_PTR(&ticker_captures_obj)},
};
static MP_DEFINE_CONST_DICT(ticker_locals, ticker_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(sc_ticker_type, MP_QSTR_ticker, MP_TYPE_FLAG_NONE, make_new, ticker_new, locals_dict, &ticker_locals);

void smartcar_deinit(void) {
    for (int i = 0; i < 4; ++i) {
        sc_ticker_t *t = MP_STATE_VM(sc_tickers)[i];
        if (t) { ticker_stop(MP_OBJ_FROM_PTR(t)); }
    }
    for (int i = 0; i < 16; ++i) {
        mp_obj_t obj = MP_STATE_VM(sc_sensor_roots)[i];
        if (obj != MP_OBJ_NULL) {
            sc_sensor_t *s = MP_OBJ_TO_PTR(obj);
            s->attached = 0;
            if (s->base.type == &sc_adc_type) {
                if (!sc_adc_shutdown(s->ctx)) {
                    mp_printf(&mp_plat_print, "smartcar: ADC stop failed; reset board before reuse\n");
                }
            } else {
                sc_encoder_deinit(s->ctx);
            }
            s->closed = true;
            MP_STATE_VM(sc_sensor_roots)[i] = MP_OBJ_NULL;
        }
    }
}
#ifdef UNIX
static mp_obj_t sim_advance(mp_obj_t ms_obj) {
    mp_int_t ms = mp_obj_get_int(ms_obj);
    if (ms < 0 || ms > 1000000) { mp_raise_ValueError(MP_ERROR_TEXT("invalid simulated duration")); }
    for (mp_int_t i = 0; i < ms; ++i) {
        smartcar_irq();
        // At most four callbacks queued in one millisecond.
        for (int j = 0; j < 4; ++j) { mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS); }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sim_advance_obj, sim_advance);
static mp_obj_t sim_adc_set(mp_obj_t obj, mp_obj_t seq) {
    sc_sensor_t *s = sensor_check(obj);
    if (s->base.type != &sc_adc_type) { mp_raise_TypeError(MP_ERROR_TEXT("expected ADC_Group")); }
    size_t n; mp_obj_t *values;
    mp_obj_get_array(seq, &n, &values);
    sc_sim_adc_set(s->ctx, n, values);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sim_adc_set_obj, sim_adc_set);
static mp_obj_t sim_encoder_step(mp_obj_t obj, mp_obj_t step) {
    sc_sensor_t *s = sensor_check(obj);
    if (s->base.type != &sc_encoder_type) { mp_raise_TypeError(MP_ERROR_TEXT("expected encoder")); }
    mp_int_t delta = mp_obj_get_int(step);
    if (delta < INT32_MIN || delta > INT32_MAX) { mp_raise_ValueError(MP_ERROR_TEXT("step out of range")); }
    sc_sim_encoder_step(s->ctx, delta);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sim_encoder_step_obj, sim_encoder_step);
#endif
static const mp_rom_map_elem_t smartcar_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_smartcar)},
    {MP_ROM_QSTR(MP_QSTR_ticker), MP_ROM_PTR(&sc_ticker_type)},
    {MP_ROM_QSTR(MP_QSTR_ADC_Group), MP_ROM_PTR(&sc_adc_type)},
    {MP_ROM_QSTR(MP_QSTR_encoder), MP_ROM_PTR(&sc_encoder_type)},
    #ifdef UNIX
    {MP_ROM_QSTR(MP_QSTR__advance), MP_ROM_PTR(&sim_advance_obj)},
    {MP_ROM_QSTR(MP_QSTR__adc_set), MP_ROM_PTR(&sim_adc_set_obj)},
    {MP_ROM_QSTR(MP_QSTR__encoder_step), MP_ROM_PTR(&sim_encoder_step_obj)},
    #endif
};
static MP_DEFINE_CONST_DICT(smartcar_globals, smartcar_globals_table);
const mp_obj_module_t smartcar_module = {.base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&smartcar_globals};
MP_REGISTER_MODULE(MP_QSTR_smartcar, smartcar_module);
