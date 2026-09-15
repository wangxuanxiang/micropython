// SPDX-License-Identifier: MIT
// ADC_Group port: H743 regular scan sequences, and deterministic Unix inputs.
#include <string.h>
#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/objstr.h"
#include "port.h"

#define SC_ADC_MAX_CHANNELS (16)

#ifndef UNIX
#include "pin.h"
#include "stm32h7xx_hal.h"
#if !defined(STM32H743xx)
#error smartcar ADC backend currently supports STM32H743 only
#endif
#endif

typedef struct {
    uint8_t id;
    uint8_t count;
    uint8_t sample_mode;
    uint8_t average;
    bool alive;
    volatile bool busy;
    uint8_t channels[SC_ADC_MAX_CHANNELS];
    #ifdef UNIX
    uint16_t input[SC_ADC_MAX_CHANNELS];
    #else
    ADC_HandleTypeDef handle;
    uint32_t hclk_hz;
    uint32_t sqr[4];
    #endif
} sc_adc_ctx_t;

static bool sc_adc_enter(sc_adc_ctx_t *ctx) {
    mp_uint_t state = MICROPY_BEGIN_ATOMIC_SECTION();
    bool available = !ctx->busy;
    if (available) {
        ctx->busy = true;
    }
    MICROPY_END_ATOMIC_SECTION(state);
    return available;
}

static void sc_adc_leave(sc_adc_ctx_t *ctx) {
    mp_uint_t state = MICROPY_BEGIN_ATOMIC_SECTION();
    ctx->busy = false;
    MICROPY_END_ATOMIC_SECTION(state);
}

static void sc_adc_require_idle(sc_adc_ctx_t *ctx) {
    if (!ctx->alive) {
        mp_raise_OSError(MP_ENODEV);
    }
    if (!sc_adc_enter(ctx)) {
        mp_raise_OSError(MP_EBUSY);
    }
}

#ifndef UNIX
// A cycle counter works even in a timer IRQ where SysTick cannot advance.
// The iteration limit also bounds failure if a debugger stops the counter.
static bool sc_adc_wait(volatile uint32_t *reg, uint32_t mask, bool set,
    uint32_t start, uint32_t budget) {
    for (unsigned guard = 0; guard < 2000000; ++guard) {
        if (((*reg & mask) != 0) == set) {
            return true;
        }
        if ((uint32_t)(mp_hal_ticks_cpu() - start) >= budget) {
            return false;
        }
    }
    return false;
}

static bool sc_adc_disable(sc_adc_ctx_t *ctx) {
    ADC_TypeDef *adc = ctx->handle.Instance;
    uint32_t start = mp_hal_ticks_cpu();
    uint32_t budget = SystemCoreClock / 500; // 2 ms maximum stop/disable
    if (adc->CR & ADC_CR_JADSTART) {
        SET_BIT(adc->CR, ADC_CR_JADSTP);
        if (!sc_adc_wait(&adc->CR, ADC_CR_JADSTART | ADC_CR_JADSTP, false, start, budget)) {
            return false;
        }
    }
    if (adc->CR & ADC_CR_ADSTART) {
        SET_BIT(adc->CR, ADC_CR_ADSTP);
        if (!sc_adc_wait(&adc->CR, ADC_CR_ADSTART | ADC_CR_ADSTP, false, start, budget)) {
            return false;
        }
    }
    if (adc->CR & ADC_CR_ADEN) {
        SET_BIT(adc->CR, ADC_CR_ADDIS);
        if (!sc_adc_wait(&adc->CR, ADC_CR_ADEN | ADC_CR_ADDIS, false, start, budget)) {
            return false;
        }
    }
    return true;
}

static void sc_adc_sequence(sc_adc_ctx_t *ctx) {
    ADC_TypeDef *adc = ctx->handle.Instance;
    uint32_t sqr[4] = {ctx->count ? ctx->count - 1 : 0, 0, 0, 0};
    uint32_t mask = 0;
    uint32_t smpr[2] = {0, 0};
    // PMODEs preserve the short -> long ordering; exact NXP times differ.
    static const uint8_t samples[] = {2, 4, 5, 6}; // 8.5, 32.5, 64.5, 387.5 cycles
    for (unsigned i = 0; i < ctx->count; ++i) {
        unsigned channel = ctx->channels[i];
        if (i < 4) {
            sqr[0] |= channel << (6 * (i + 1));
        } else {
            sqr[1 + (i - 4) / 5] |= channel << (6 * ((i - 4) % 5));
        }
        mask |= 1u << channel;
        smpr[channel / 10] |= samples[ctx->sample_mode] << (3 * (channel % 10));
    }
    adc->SQR1 = sqr[0];
    adc->SQR2 = sqr[1];
    adc->SQR3 = sqr[2];
    adc->SQR4 = sqr[3];
    memcpy(ctx->sqr, sqr, sizeof(sqr));
    adc->SMPR1 = smpr[0];
    adc->SMPR2 = smpr[1];
    adc->PCSEL = mask;
    adc->DIFSEL &= ~mask;
}
#endif

void *sc_adc_init(int id) {
    if (id != 1 && id != 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("ADC id must be 1 or 2"));
    }
    sc_adc_ctx_t *ctx = m_new0(sc_adc_ctx_t, 1);
    ctx->id = id;
    ctx->sample_mode = 3;
    ctx->average = 16;
    sc_adc_claim(id, ctx);
    #ifndef UNIX
    ADC_TypeDef *adc = id == 1 ? ADC1 : ADC2;
    ctx->handle.Instance = adc;
    // Despite the HAL's PCLK name, the synchronous source is HCLK. Rev.V
    // divides it by a further two inside the ADC (see ADC_ConfigureBoostMode).
    ctx->hclk_hz = HAL_RCC_GetHCLKFreq();
    uint32_t adc_hz = ctx->hclk_hz / 4;
    if (HAL_GetREVID() > REV_ID_Y) {
        adc_hz /= 2;
    }
    if (adc_hz == 0 || adc_hz > 36000000) {
        sc_adc_release(id, ctx);
        mp_raise_ValueError(MP_ERROR_TEXT("ADC clock exceeds 36 MHz; reduce CPU frequency"));
    }
    __HAL_RCC_ADC12_CLK_ENABLE();
    // Claim checks cover smartcar objects. Ordinary machine/pyb ADC objects
    // do not participate, so refuse an enabled peripheral rather than reset it.
    // While a group lives, users must not construct/use other ADC APIs on it.
    uint32_t clocks = ADC12_COMMON->CCR & (ADC_CCR_CKMODE | ADC_CCR_PRESC);
    if ((adc->CR & (ADC_CR_ADEN | ADC_CR_ADCAL | ADC_CR_ADSTART | ADC_CR_JADSTART))
        || (ADC12_COMMON->CCR & ADC_CCR_DUAL)
        || (((ADC1->CR | ADC2->CR) & ADC_CR_ADEN)
            && clocks != ADC_CLOCK_SYNC_PCLK_DIV4)) {
        sc_adc_release(id, ctx);
        mp_raise_OSError(MP_EBUSY);
    }
    ADC_InitTypeDef *init = &ctx->handle.Init;
    init->ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
    init->Resolution = ADC_RESOLUTION_12B;
    init->ScanConvMode = ADC_SCAN_ENABLE;
    init->EOCSelection = ADC_EOC_SINGLE_CONV;
    init->LowPowerAutoWait = ENABLE; // retain each rank until DR is read
    init->ContinuousConvMode = DISABLE;
    init->NbrOfConversion = 1;
    init->DiscontinuousConvMode = DISABLE;
    init->ExternalTrigConv = ADC_SOFTWARE_START;
    init->ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    init->ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    init->Overrun = ADC_OVR_DATA_PRESERVED;
    init->LeftBitShift = ADC_LEFTBITSHIFT_NONE;
    init->OversamplingMode = DISABLE;
    // HAL changes only ADC12 clock fields; do not use adc_config(), which
    // also rewrites ADC3_COMMON. Leave clocks enabled on deinit for ADC2/1.
    if (HAL_ADC_Init(&ctx->handle) != HAL_OK
        || HAL_ADCEx_Calibration_Start(&ctx->handle, ADC_CALIB_OFFSET_LINEARITY,
            ADC_SINGLE_ENDED) != HAL_OK) {
        sc_adc_disable(ctx);
        sc_adc_release(id, ctx);
        mp_raise_OSError(MP_EIO);
    }
    // This peripheral is exclusively owned. Remove stale offsets/watchdog
    // interrupts left by an earlier user before starting register-only scans.
    adc->IER = 0;
    // An old injected external trigger must not interrupt the regular scan.
    adc->JSQR = 0;
    adc->CFGR &= ~(ADC_CFGR_JAUTO | ADC_CFGR_JDISCEN);
    adc->OFR1 = 0;
    adc->OFR2 = 0;
    adc->OFR3 = 0;
    adc->OFR4 = 0;
    mp_hal_ticks_cpu_enable();
    #endif
    ctx->alive = true;
    return ctx;
}

void sc_adc_add(void *context, mp_obj_t pin_in) {
    sc_adc_ctx_t *ctx = context;
    if (!ctx->alive) {
        mp_raise_OSError(MP_ENODEV);
    }
    if (ctx->count == SC_ADC_MAX_CHANNELS) {
        mp_raise_ValueError(MP_ERROR_TEXT("at most 16 ADC channels"));
    }
    unsigned channel;
    #ifdef UNIX
    // This is a model of the exposed WeAct H743 analog pins, not fake RT1021
    // pin aliases. All accepted channels below exist on this target board.
    static const struct { const char *name; uint8_t channel; uint8_t mask; } pins[] = {
        {"A0", 16, 1}, {"A1", 17, 1}, {"A2", 14, 3}, {"A3", 15, 3},
        {"A4", 18, 3}, {"A5", 19, 3}, {"A6", 3, 3}, {"A7", 7, 3},
        {"B0", 9, 3}, {"B1", 5, 3}, {"C0", 10, 3}, {"C1", 11, 3},
        {"C2", 12, 3}, {"C3", 13, 3}, {"C4", 4, 3}, {"C5", 8, 3},
    };
    const char *name = mp_obj_str_get_str(pin_in);
    unsigned i;
    for (i = 0; i < MP_ARRAY_SIZE(pins); ++i) {
        if (!strcmp(name, pins[i].name) && (pins[i].mask & (1u << (ctx->id - 1)))) {
            break;
        }
    }
    if (i == MP_ARRAY_SIZE(pins)) {
        mp_raise_ValueError(MP_ERROR_TEXT("pin is not on selected ADC"));
    }
    channel = pins[i].channel;
    #else
    const machine_pin_obj_t *pin = pin_find(pin_in);
    unsigned mask = pin->adc_num;
    channel = pin->adc_channel;
    // PA1 is channel17 on ADC1. The AF CSV selects ADC12_INP1, which is the
    // separate PA1_C input and must not be used for the board's PA1 header.
    if (pin->port == PORT_A && pin->pin == 1) {
        mask = PIN_ADC1;
        channel = 17;
    }
    if (!(mask & (1u << (ctx->id - 1)))) {
        mp_raise_ValueError(MP_ERROR_TEXT("pin is not on selected ADC"));
    }
    #endif
    for (unsigned i = 0; i < ctx->count; ++i) {
        if (ctx->channels[i] == channel) {
            mp_raise_ValueError(MP_ERROR_TEXT("duplicate ADC channel"));
        }
    }
    sc_adc_require_idle(ctx);
    #ifndef UNIX
    if (!sc_adc_disable(ctx)) {
        sc_adc_leave(ctx);
        mp_raise_OSError(MP_ETIMEDOUT);
    }
    #endif
    // A failed claim must release busy, and leave the previous sequence intact.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        sc_pin_claim(pin_in, ctx);
        nlr_pop();
    } else {
        sc_adc_leave(ctx);
        nlr_jump(nlr.ret_val);
    }
    #ifndef UNIX
    mp_hal_pin_config(pin, MP_HAL_PIN_MODE_ADC, MP_HAL_PIN_PULL_NONE, 0);
    #endif
    ctx->channels[ctx->count++] = channel;
    #ifndef UNIX
    sc_adc_sequence(ctx);
    #endif
    sc_adc_leave(ctx);
}

void sc_adc_configure(void *context, int sample_mode, int average) {
    sc_adc_ctx_t *ctx = context;
    if (sample_mode < 0 || sample_mode > 3
        || !(average == 1 || average == 4 || average == 8 || average == 16 || average == 32)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ADC sampling mode or average"));
    }
    sc_adc_require_idle(ctx);
    #ifndef UNIX
    if (!sc_adc_disable(ctx)) {
        sc_adc_leave(ctx);
        mp_raise_OSError(MP_ETIMEDOUT);
    }
    #endif
    ctx->sample_mode = sample_mode;
    ctx->average = average;
    #ifndef UNIX
    sc_adc_sequence(ctx);
    #endif
    sc_adc_leave(ctx);
}

int sc_adc_capture(void *context, uint16_t *values) {
    sc_adc_ctx_t *ctx = context;
    if (!ctx->alive) {
        return MP_ENODEV;
    }
    if (!ctx->count) {
        return MP_EINVAL;
    }
    if (!sc_adc_enter(ctx)) {
        return MP_EBUSY;
    }
    #ifdef UNIX
    memcpy(values, ctx->input, sizeof(uint16_t) * ctx->count);
    sc_adc_leave(ctx);
    return 0;
    #else
    ADC_TypeDef *adc = ctx->handle.Instance;
    int error = 0;
    uint32_t sums[SC_ADC_MAX_CHANNELS] = {0};
    uint32_t start = mp_hal_ticks_cpu();
    uint32_t budget = SystemCoreClock / 50; // bounded 20 ms for entire capture
    // Catch clock changes and common accidental reconfiguration. Ordinary
    // machine.ADC/pyb.ADC reads must additionally respect the shared claim.
    const uint32_t cfgr_mask = ADC_CFGR_RES | ADC_CFGR_AUTDLY | ADC_CFGR_CONT
        | ADC_CFGR_EXTEN | ADC_CFGR_DMNGT | ADC_CFGR_DISCEN;
    if (HAL_RCC_GetHCLKFreq() != ctx->hclk_hz
        || (ADC12_COMMON->CCR & (ADC_CCR_CKMODE | ADC_CCR_PRESC | ADC_CCR_DUAL))
            != ADC_CLOCK_SYNC_PCLK_DIV4
        || (adc->CFGR & cfgr_mask) != (ADC_RESOLUTION_12B | ADC_CFGR_AUTDLY)
        || (adc->CFGR2 & (ADC_CFGR2_ROVSE | ADC_CFGR2_LSHIFT))
        || adc->SQR1 != ctx->sqr[0] || adc->SQR2 != ctx->sqr[1]
        || adc->SQR3 != ctx->sqr[2] || adc->SQR4 != ctx->sqr[3]) {
        sc_adc_leave(ctx);
        return MP_EBUSY;
    }
    if (!(adc->CR & ADC_CR_ADEN)) {
        adc->ISR = ADC_ISR_ADRDY;
        SET_BIT(adc->CR, ADC_CR_ADEN);
        if (!sc_adc_wait(&adc->ISR, ADC_ISR_ADRDY, true, start, budget)) {
            error = MP_ETIMEDOUT;
            goto done;
        }
    }
    for (unsigned sample = 0; sample < ctx->average; ++sample) {
        if (!sc_adc_wait(&adc->CR, ADC_CR_ADSTART, false, start, budget)) {
            error = MP_ETIMEDOUT;
            goto done;
        }
        adc->ISR = ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;
        SET_BIT(adc->CR, ADC_CR_ADSTART);
        for (unsigned i = 0; i < ctx->count; ++i) {
            if (!sc_adc_wait(&adc->ISR, ADC_ISR_EOC, true, start, budget)) {
                error = MP_ETIMEDOUT;
                goto done;
            }
            if (adc->ISR & ADC_ISR_OVR) {
                error = MP_EIO;
                goto done;
            }
            // Reading DR clears EOC and permits the next AUTDLY rank.
            sums[i] += adc->DR & 0xfff;
        }
    }
    for (unsigned i = 0; i < ctx->count; ++i) {
        values[i] = sums[i] / ctx->average;
    }
done:
    if (error && (adc->CR & ADC_CR_ADSTART)) {
        // Stop requested without an unbounded HAL/SysTick wait in the IRQ.
        SET_BIT(adc->CR, ADC_CR_ADSTP);
    }
    sc_adc_leave(ctx);
    return error;
    #endif
}

void sc_adc_deinit(void *context) {
    sc_adc_ctx_t *ctx = context;
    if (!ctx->alive) {
        return;
    }
    sc_adc_require_idle(ctx);
    #ifndef UNIX
    if (!sc_adc_disable(ctx)) {
        sc_adc_leave(ctx);
        mp_raise_OSError(MP_ETIMEDOUT);
    }
    // Do not reset the ADC12 block, gate its shared clock, or disturb ADC3.
    #endif
    ctx->alive = false;
    sc_pin_release_all(ctx);
    sc_adc_release(ctx->id, ctx);
    sc_adc_leave(ctx);
}

bool sc_adc_shutdown(void *context) {
    // Soft reset only: caller has stopped all ticker IRQs and is discarding
    // the VM. Always release GC roots, even if a damaged ADC fails to stop.
    sc_adc_ctx_t *ctx = context;
    bool stopped = true;
    if (ctx->alive) {
        ctx->alive = false;
        #ifndef UNIX
        ADC_TypeDef *adc = ctx->handle.Instance;
        adc->IER = 0;
        stopped = sc_adc_disable(ctx);
        // There is no DMA and no ADC IRQ callback in this backend, so an
        // unresponsive peripheral cannot access the discarded VM heap.
        // Do not reset ADC12 globally: another ADC may be in use.
        #endif
        sc_pin_release_all(ctx);
        sc_adc_release(ctx->id, ctx);
        ctx->busy = false;
    }
    return stopped;
}

#ifdef UNIX
void sc_sim_adc_set(void *context, size_t n, const mp_obj_t *values) {
    sc_adc_ctx_t *ctx = context;
    if (!ctx->alive) {
        mp_raise_OSError(MP_ENODEV);
    }
    if (n != ctx->count) {
        mp_raise_ValueError(MP_ERROR_TEXT("one sample required per ADC channel"));
    }
    uint16_t input[SC_ADC_MAX_CHANNELS];
    for (unsigned i = 0; i < n; ++i) {
        mp_int_t value = mp_obj_get_int(values[i]);
        if (value < 0 || value > 4095) {
            mp_raise_ValueError(MP_ERROR_TEXT("ADC input must be 0..4095"));
        }
        input[i] = value;
    }
    sc_adc_require_idle(ctx);
    memcpy(ctx->input, input, n * sizeof(uint16_t));
    sc_adc_leave(ctx);
}
#endif
