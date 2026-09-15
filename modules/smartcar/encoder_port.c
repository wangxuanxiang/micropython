// SPDX-License-Identifier: MIT
// Hardware quadrature counting. No Python calls or allocation in capture.
#include <string.h>
#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/nlr.h"
#include "port.h"

#ifndef UNIX
#include "pin.h"
#include "timer.h"
#if !defined(STM32H743xx)
#error smartcar encoder backend currently supports STM32H743 only
#endif
#endif

typedef struct {
    uint32_t previous;
    uint32_t mask;
    unsigned timer_id;
    bool invert;
    bool active;
    #ifdef UNIX
    uint32_t counter;
    #else
    TIM_TypeDef *timer;
    const machine_pin_obj_t *a;
    const machine_pin_obj_t *b;
    #endif
} sc_encoder_ctx_t;

#ifdef UNIX
// Deliberately limited, board-safe simulation mappings. Hardware uses its AF
// tables, not this list. A precedes B (TIM CH1, CH2) in every pair.
static unsigned sc_encoder_sim_timer(mp_obj_t a, mp_obj_t b) {
    const char *a_name = mp_obj_str_get_str(a);
    const char *b_name = mp_obj_str_get_str(b);
    static const struct { const char *a; const char *b; unsigned timer; } pairs[] = {
        { "A0", "A1", 2 },
        { "A5", "A1", 2 },
        { "A15", "A1", 2 },
        { "A6", "A7", 3 },
        { "C6", "C7", 3 },
        { "A8", "A9", 1 },
        { "E9", "E11", 1 },
    };
    for (size_t i = 0; i < MP_ARRAY_SIZE(pairs); ++i) {
        if (!strcmp(a_name, pairs[i].a) && !strcmp(b_name, pairs[i].b)) {
            return pairs[i].timer;
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("unsupported encoder CH1/CH2 pin pair"));
}
#else
static bool sc_encoder_capable(unsigned id) {
    // STM32H743 encoder interface instances; other timer AFs (e.g. TIM12)
    // cannot be assumed to implement the encoder slave mode.
    return id == 1 || id == 2 || id == 3 || id == 4 || id == 5 || id == 8;
}

static const pin_af_obj_t *sc_encoder_find_pair(const machine_pin_obj_t *a,
    const machine_pin_obj_t *b, const pin_af_obj_t **b_af) {
    for (size_t i = 0; i < a->num_af; ++i) {
        const pin_af_obj_t *af = &a->af[i];
        if (af->fn != AF_FN_TIM || !sc_encoder_capable(af->unit)
            || (af->type != AF_PIN_TYPE_TIM_CH1 && af->type != AF_PIN_TYPE_TIM_CH1_ETR)) {
            continue;
        }
        for (size_t j = 0; j < b->num_af; ++j) {
            const pin_af_obj_t *other = &b->af[j];
            if (other->fn == AF_FN_TIM && other->unit == af->unit
                && other->type == AF_PIN_TYPE_TIM_CH2) {
                *b_af = other;
                return af;
            }
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("encoder requires same TIM CH1 then CH2 pins"));
}
#endif

void *sc_encoder_init(mp_obj_t a, mp_obj_t b, bool invert) {
    #ifdef UNIX
    unsigned timer_id = sc_encoder_sim_timer(a, b);
    #else
    const machine_pin_obj_t *pin_a = pin_find(a);
    const machine_pin_obj_t *pin_b = pin_find(b);
    const pin_af_obj_t *af_b = NULL;
    const pin_af_obj_t *af_a = sc_encoder_find_pair(pin_a, pin_b, &af_b);
    unsigned timer_id = af_a->unit;
    #endif

    sc_encoder_ctx_t *ctx = m_new0(sc_encoder_ctx_t, 1);
    ctx->timer_id = timer_id;
    ctx->invert = invert;
    ctx->mask = timer_id == 2 || timer_id == 5 ? UINT32_MAX : UINT16_MAX;
    #ifndef UNIX
    ctx->timer = timer_id_to_reg(timer_id);
    ctx->a = pin_a;
    ctx->b = pin_b;
    #endif

    // Transactional claims: a conflicting second pin must not leak the first
    // pin or timer. Claim helpers also reject board/system-owned resources.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        sc_timer_claim(timer_id, ctx);
        #ifndef UNIX
        // Guard active PWM or another timer consumer even if it is outside the
        // smartcar claim registry. Do not reconfigure a running peripheral.
        if (ctx->timer->CR1 & TIM_CR1_CEN) {
            mp_raise_OSError(MP_EBUSY);
        }
        #endif
        sc_pin_claim(a, ctx);
        sc_pin_claim(b, ctx);
        nlr_pop();
    } else {
        sc_pin_release_all(ctx);
        sc_timer_release(timer_id, ctx);
        nlr_jump(nlr.ret_val);
    }

    #ifndef UNIX
    timer_clock_enable(timer_id);
    TIM_TypeDef *tim = ctx->timer;
    tim->CR1 = 0;
    tim->CR2 = 0;
    tim->DIER = 0;
    tim->CCER = 0;
    tim->SMCR = TIM_ENCODERMODE_TI12;
    tim->CCMR1 = TIM_CCMR1_CC1S_0 | TIM_CCMR1_CC2S_0;
    tim->CCMR2 = 0;
    // A prior timer user may have routed internal signals into TI1/TI2.
    // Explicitly select the physical channel pins for this encoder.
    tim->TISEL = 0;
    tim->PSC = 0;
    tim->ARR = ctx->mask;
    if (IS_TIM_REPETITION_COUNTER_INSTANCE(tim)) {
        tim->RCR = 0;
    }
    tim->EGR = TIM_EGR_UG;
    tim->SR = 0;
    tim->CNT = 0;
    mp_hal_pin_config(pin_a, MP_HAL_PIN_MODE_ALT, MP_HAL_PIN_PULL_UP, af_a->idx);
    mp_hal_pin_config(pin_b, MP_HAL_PIN_MODE_ALT, MP_HAL_PIN_PULL_UP, af_b->idx);
    tim->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E;
    tim->CR1 = TIM_CR1_CEN;
    #endif
    ctx->active = true;
    return ctx;
}

int sc_encoder_capture(void *ctx_in, int32_t *value) {
    sc_encoder_ctx_t *ctx = ctx_in;
    // A foreground read and ticker IRQ capture must not consume the same
    // interval twice, or overwrite each other's counter baseline.
    mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
    if (!ctx->active) {
        MICROPY_END_ATOMIC_SECTION(atomic_state);
        return MP_ENODEV;
    }
    #ifdef UNIX
    uint32_t current = ctx->counter;
    #else
    if (!(ctx->timer->CR1 & TIM_CR1_CEN)) {
        MICROPY_END_ATOMIC_SECTION(atomic_state);
        return MP_EIO;
    }
    uint32_t current = ctx->timer->CNT;
    #endif
    uint32_t difference = (current - ctx->previous) & ctx->mask;
    ctx->previous = current;
    MICROPY_END_ATOMIC_SECTION(atomic_state);
    // Direction is mathematically ambiguous at exactly half the counter
    // range. Sampling must occur before 2^(width-1) transitions have elapsed.
    uint32_t half = (ctx->mask >> 1) + 1;
    if (difference == half) {
        return MP_ERANGE;
    }
    int32_t delta = difference < half ? (int32_t)difference
        : -(int32_t)((ctx->mask - difference) + 1);
    *value = ctx->invert ? -delta : delta;
    return 0;
}

void sc_encoder_deinit(void *ctx_in) {
    sc_encoder_ctx_t *ctx = ctx_in;
    if (!ctx->active) {
        return;
    }
    ctx->active = false;
    #ifndef UNIX
    ctx->timer->CR1 &= ~TIM_CR1_CEN;
    ctx->timer->CCER = 0;
    ctx->timer->SMCR = 0;
    mp_hal_pin_input(ctx->a);
    mp_hal_pin_input(ctx->b);
    #endif
    sc_pin_release_all(ctx);
    sc_timer_release(ctx->timer_id, ctx);
}

#ifdef UNIX
void sc_sim_encoder_step(void *ctx_in, int32_t step) {
    sc_encoder_ctx_t *ctx = ctx_in;
    if (!ctx->active) {
        mp_raise_OSError(MP_ENODEV);
    }
    ctx->counter = (ctx->counter + (uint32_t)step) & ctx->mask;
}
#endif
