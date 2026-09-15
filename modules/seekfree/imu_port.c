// SPDX-License-Identifier: MIT
// IMU660RA (BMI270), IMU660RB and IMU660RC native SPI backend.
// Register settings follow the supplied SeekFree STC device documentation.
#include <string.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/nlr.h"
#include "imu_port.h"
#include "bmi270_config.h"
#include <stdint.h>

extern void sc_pin_claim(mp_obj_t pin, void *owner);
extern void sc_pin_release_all(void *owner);
#ifndef UNIX
#include "pin.h"
#include "spi.h"
#if !defined(STM32H743xx)
#error IMU660 native backend requires STM32H743
#endif
#endif

typedef struct {
    unsigned spi_id;
    uint8_t model;
    bool active;
    bool bus_started;
    #ifdef UNIX
    uint8_t regs[128];
    size_t config_written;
    int fault;
    #else
    const spi_t *bus;
    const machine_pin_obj_t *cs;
    #endif
} imu_ctx_t;

MP_REGISTER_ROOT_POINTER(void *seekfree_spi_owners[7]);
bool seekfree_spi_claimed(unsigned id) {
    return id > 0 && id < 7 && MP_STATE_VM(seekfree_spi_owners)[id] != NULL;
}
const char *imu_port_model(void *ctx_in) {
    static const char *const names[] = { "IMU660RA", "IMU660RB", "IMU660RC" };
    return names[((imu_ctx_t *)ctx_in)->model];
}
float imu_port_acc_scale(void *ctx_in) {
    static const float scale[] = { 4096.0f, 4098.0f, 4098.36f };
    return scale[((imu_ctx_t *)ctx_in)->model];
}
float imu_port_gyro_scale(void *ctx_in) {
    static const float scale[] = { 16.4f, 14.3f, 14.2857f };
    return scale[((imu_ctx_t *)ctx_in)->model];
}

#ifdef UNIX
static int sim_model;
static int sim_error;
void imu_port_sim_config(int model, int error) {
    if (model < 0 || model > 3 || error < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid simulated IMU configuration"));
    }
    sim_model = model;
    sim_error = error;
}
void imu_port_fault(void *ctx_in, int error) {
    if (error < 0) { mp_raise_ValueError(MP_ERROR_TEXT("invalid error")); }
    ((imu_ctx_t *)ctx_in)->fault = error;
}
void imu_port_set(void *ctx_in, size_t count, const mp_obj_t *values) {
    imu_ctx_t *ctx = ctx_in;
    if (count != 6) { mp_raise_ValueError(MP_ERROR_TEXT("expected six IMU values")); }
    int16_t raw[6];
    for (size_t i = 0; i < 6; ++i) {
        mp_int_t value = mp_obj_get_int(values[i]);
        if (value < INT16_MIN || value > INT16_MAX) {
            mp_raise_ValueError(MP_ERROR_TEXT("IMU value out of int16 range"));
        }
        raw[i] = value;
    }
    for (size_t i = 0; i < 6; ++i) {
        unsigned reg = ctx->model == 0 ? 0x0c + i * 2 : (i < 3 ? 0x28 + i * 2 : 0x22 + (i - 3) * 2);
        ctx->regs[reg] = (uint16_t)raw[i];
        ctx->regs[reg + 1] = (uint16_t)raw[i] >> 8;
    }
}
static int imu_read(imu_ctx_t *ctx, uint8_t reg, uint8_t *out, size_t count, unsigned dummy) {
    (void)dummy;
    if (ctx->fault) { return ctx->fault; }
    memcpy(out, ctx->regs + reg, count);
    return 0;
}
static int imu_write_block(imu_ctx_t *ctx, uint8_t reg, const uint8_t *data, size_t count) {
    if (ctx->fault) { return ctx->fault; }
    if (reg == 0x5e && ctx->model == 0) {
        if (ctx->config_written + count > sizeof(bmi270_config)
            || memcmp(data, bmi270_config + ctx->config_written, count)) { return MP_EIO; }
        ctx->config_written += count;
    } else {
        memcpy(ctx->regs + reg, data, count);
        if (ctx->model == 0 && reg == 0x59 && data[0] == 1) {
            ctx->regs[0x21] = ctx->config_written == sizeof(bmi270_config) ? 1 : 0;
        }
    }
    return 0;
}
#else
// Native transport below deliberately avoids HAL's tick-based waits and DMA.
static int imu_transfer(imu_ctx_t *ctx, uint8_t command, const uint8_t *tx,
    uint8_t *rx, size_t count, unsigned dummy) {
    SPI_TypeDef *spi = ctx->bus->spi->Instance;
    size_t total = 1 + dummy + count;
    // No event hook, allocation or SysTick dependency, including error paths.
    // DWT runs inside a hard IRQ; iteration limit also bounds a stopped DWT.
    uint32_t started = DWT->CYCCNT;
    uint32_t timeout = SystemCoreClock / 500; // two milliseconds per transaction
    uint32_t budget = SystemCoreClock / 100;
    int error = 0;
    CLEAR_BIT(spi->CR1, SPI_CR1_SPE);
    spi->IFCR = 0xffffffff;
    spi->CR2 = total;
    SET_BIT(spi->CR1, SPI_CR1_SPE);
    mp_hal_pin_low(ctx->cs);
    SET_BIT(spi->CR1, SPI_CR1_CSTART);
    size_t sent = 0, received = 0;
    while (received < total) {
        uint32_t status = spi->SR;
        if (status & (SPI_SR_OVR | SPI_SR_MODF | SPI_SR_UDR)) {
            error = MP_EIO;
            break;
        }
        if (sent < total && (status & SPI_SR_TXP)) {
            uint8_t value = sent == 0 ? command : (tx && sent > dummy ? tx[sent - 1 - dummy] : 0);
            *(__IO uint8_t *)&spi->TXDR = value;
            ++sent;
        }
        if (status & SPI_SR_RXP) {
            uint8_t value = *(__IO uint8_t *)&spi->RXDR;
            // RX contains the command byte, optional BMI270 dummy byte(s),
            // then payload.  Do not let either prefix shift the first sample.
            if (rx && received >= 1 + dummy) { rx[received - 1 - dummy] = value; }
            ++received;
        }
        if (--budget == 0 || (uint32_t)(DWT->CYCCNT - started) >= timeout) {
            error = MP_ETIMEDOUT;
            break;
        }
    }
    while (!error && !(spi->SR & SPI_SR_EOT)) {
        if (--budget == 0 || (uint32_t)(DWT->CYCCNT - started) >= timeout) { error = MP_ETIMEDOUT; }
    }
    CLEAR_BIT(spi->CR1, SPI_CR1_SPE);
    spi->IFCR = 0xffffffff;
    mp_hal_pin_high(ctx->cs);
    return error;
}
static int imu_read(imu_ctx_t *ctx, uint8_t reg, uint8_t *out, size_t count, unsigned dummy) {
    return imu_transfer(ctx, reg | 0x80, NULL, out, count, dummy);
}
static int imu_write_block(imu_ctx_t *ctx, uint8_t reg, const uint8_t *data, size_t count) {
    return imu_transfer(ctx, reg, data, NULL, count, 0);
}
#endif

static int imu_write(imu_ctx_t *ctx, uint8_t reg, uint8_t value) {
    return imu_write_block(ctx, reg, &value, 1);
}
static void imu_delay(unsigned ms) {
    #ifndef UNIX
    mp_hal_delay_ms(ms);
    #else
    (void)ms;
    #endif
}
static int imu_configure(imu_ctx_t *ctx) {
    uint8_t id = 0;
    int error;
    // A first dummy read switches the BMI270 from I2C to SPI after power-up.
    if ((error = imu_read(ctx, 0, &id, 1, 1))) { return error; }
    imu_delay(2);
    if ((error = imu_read(ctx, 0, &id, 1, 1))) { return error; }
    if (id == 0x24) {
        ctx->model = 0;
    } else {
        if ((error = imu_read(ctx, 0x0f, &id, 1, 0))) { return error; }
        if (id != 0x6b && id != 0x70) { return MP_ENODEV; }
        ctx->model = id == 0x6b ? 1 : 2;
    }
    #define IMU_WRITE(reg, value) do { if ((error = imu_write(ctx, reg, value))) { return error; } } while (0)
    if (ctx->model == 0) {
        IMU_WRITE(0x7c, 0);
        imu_delay(1);
        IMU_WRITE(0x59, 0);
        // INIT_ADDR is expressed in words. Explicit addresses allow bounded
        // 32-byte bursts without an 8192-byte allocation or long IRQ masking.
        for (size_t offset = 0; offset < sizeof(bmi270_config); offset += 32) {
            // INIT_ADDR_0 contains the low eight address bits; the upper
            // nibble is in INIT_ADDR_1 (the 8192-byte blob is addressed in
            // 16-bit words).
            IMU_WRITE(0x5b, (offset / 2) & 0xff);
            IMU_WRITE(0x5c, (offset / 2) >> 4);
            if ((error = imu_write_block(ctx, 0x5e, bmi270_config + offset, 32))) { return error; }
        }
        IMU_WRITE(0x59, 1);
        imu_delay(150);
        if ((error = imu_read(ctx, 0x21, &id, 1, 1))) { return error; }
        if ((id & 0x0f) != 1) { return MP_EIO; }
        IMU_WRITE(0x7d, 0x0e);
        IMU_WRITE(0x40, 0xa7);
        IMU_WRITE(0x42, 0xa9);
        IMU_WRITE(0x41, 0x02);
        IMU_WRITE(0x43, 0x00);
    } else if (ctx->model == 1) {
        IMU_WRITE(0x0d, 0x03);
        IMU_WRITE(0x10, 0x3c);
        IMU_WRITE(0x11, 0x5c);
        IMU_WRITE(0x12, 0x44);
        IMU_WRITE(0x13, 0x02);
        IMU_WRITE(0x14, 0x00);
        IMU_WRITE(0x15, 0x00);
        IMU_WRITE(0x16, 0x00);
        IMU_WRITE(0x18, 0x01);
    } else {
        IMU_WRITE(0x01, 0x04);
        imu_delay(30);
        IMU_WRITE(0x12, 0x44);
        IMU_WRITE(0x17, 0x02);
        IMU_WRITE(0x15, 0x04);
        IMU_WRITE(0x10, 0x15);
        IMU_WRITE(0x11, 0x18);
        IMU_WRITE(0x16, 0x01);
        IMU_WRITE(0x18, 0x08);
    }
    #undef IMU_WRITE
    return 0;
}

int imu_port_capture(void *ctx_in, int16_t out[6]) {
    imu_ctx_t *ctx = ctx_in;
    if (!ctx || !ctx->active) { return MP_ENODEV; }
    uint8_t data[12];
    // Both sensor families expose all six axes consecutively. BMI270 orders
    // accelerometer first; the ST parts order gyroscope first.
    int error = imu_read(ctx, ctx->model == 0 ? 0x0c : 0x22, data, 12, ctx->model == 0);
    if (error) { return error; }
    for (size_t axis = 0; axis < 6; ++axis) {
        size_t index = ctx->model == 0 ? axis : (axis + 3) % 6;
        out[axis] = (int16_t)((uint16_t)data[2 * index] | (uint16_t)data[2 * index + 1] << 8);
    }
    return 0;
}

void imu_port_deinit(void *ctx_in) {
    imu_ctx_t *ctx = ctx_in;
    if (!ctx) { return; }
    ctx->active = false;
    if (MP_STATE_VM(seekfree_spi_owners)[ctx->spi_id] == ctx) {
        MP_STATE_VM(seekfree_spi_owners)[ctx->spi_id] = NULL;
    }
    #ifndef UNIX
    if (ctx->bus_started) {
        mp_hal_pin_high(ctx->cs);
        spi_deinit(ctx->bus);
    }
    #endif
    ctx->bus_started = false;
    sc_pin_release_all(ctx);
}

void *imu_port_init(int spi_id, mp_obj_t cs) {
    if (spi_id < 1 || spi_id > 6) { mp_raise_ValueError(MP_ERROR_TEXT("invalid SPI id")); }
    if (seekfree_spi_claimed(spi_id)) { mp_raise_OSError(MP_EBUSY); }
    if (cs == mp_const_none || cs == MP_OBJ_NULL) { cs = mp_obj_new_str("B12", 3); }
    mp_obj_t pins[3];
    #ifdef UNIX
    // Same board-safe defaults as the target; unsupported buses are rejected.
    if (spi_id == 1) {
        pins[0] = mp_obj_new_str("A5", 2);
        pins[1] = mp_obj_new_str("A6", 2);
        pins[2] = mp_obj_new_str("A7", 2);
    } else if (spi_id == 2) {
        pins[0] = mp_obj_new_str("B13", 3);
        pins[1] = mp_obj_new_str("B14", 3);
        pins[2] = mp_obj_new_str("B15", 3);
    } else if (spi_id == 4) {
        pins[0] = MP_OBJ_NEW_QSTR(MP_QSTR_E12);
        pins[1] = MP_OBJ_NEW_QSTR(MP_QSTR_E13);
        pins[2] = MP_OBJ_NEW_QSTR(MP_QSTR_E14);
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("SPI unavailable in simulator (use 2 or 4)"));
    }
    #else
    spi_find_index(MP_OBJ_NEW_SMALL_INT(spi_id));
    const spi_t *bus = &spi_obj[spi_id - 1];
    if (bus->spi->State != HAL_SPI_STATE_RESET) { mp_raise_OSError(MP_EBUSY); }
    const machine_pin_obj_t *pin_cs = pin_find(cs);
    switch (spi_id) {
        #if defined(MICROPY_HW_SPI1_SCK) && defined(MICROPY_HW_SPI1_MISO) && defined(MICROPY_HW_SPI1_MOSI)
        case 1:
            pins[0] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI1_SCK);
            pins[1] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI1_MISO);
            pins[2] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI1_MOSI);
            break;
        #endif
        #if defined(MICROPY_HW_SPI2_SCK) && defined(MICROPY_HW_SPI2_MISO) && defined(MICROPY_HW_SPI2_MOSI)
        case 2:
            pins[0] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI2_SCK);
            pins[1] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI2_MISO);
            pins[2] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI2_MOSI);
            break;
        #endif
        #if defined(MICROPY_HW_SPI3_SCK) && defined(MICROPY_HW_SPI3_MISO) && defined(MICROPY_HW_SPI3_MOSI)
        case 3:
            pins[0] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI3_SCK);
            pins[1] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI3_MISO);
            pins[2] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI3_MOSI);
            break;
        #endif
        #if defined(MICROPY_HW_SPI4_SCK) && defined(MICROPY_HW_SPI4_MISO) && defined(MICROPY_HW_SPI4_MOSI)
        case 4:
            pins[0] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI4_SCK);
            pins[1] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI4_MISO);
            pins[2] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI4_MOSI);
            break;
        #endif
        #if defined(MICROPY_HW_SPI5_SCK) && defined(MICROPY_HW_SPI5_MISO) && defined(MICROPY_HW_SPI5_MOSI)
        case 5:
            pins[0] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI5_SCK);
            pins[1] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI5_MISO);
            pins[2] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI5_MOSI);
            break;
        #endif
        #if defined(MICROPY_HW_SPI6_SCK) && defined(MICROPY_HW_SPI6_MISO) && defined(MICROPY_HW_SPI6_MOSI)
        case 6:
            pins[0] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI6_SCK);
            pins[1] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI6_MISO);
            pins[2] = MP_OBJ_FROM_PTR(MICROPY_HW_SPI6_MOSI);
            break;
        #endif
        default: mp_raise_ValueError(MP_ERROR_TEXT("SPI requires SCK/MISO/MOSI"));
    }
    #endif
    imu_ctx_t *ctx = m_new0(imu_ctx_t, 1);
    ctx->spi_id = spi_id;
    #ifndef UNIX
    ctx->bus = bus;
    ctx->cs = pin_cs;
    #endif
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        sc_pin_claim(cs, ctx);
        for (size_t i = 0; i < 3; ++i) { sc_pin_claim(pins[i], ctx); }
        #ifdef UNIX
        if (sim_model > 2) { mp_raise_OSError(MP_ENODEV); }
        ctx->model = sim_model;
        ctx->fault = sim_error;
        ctx->regs[0] = sim_model == 0 ? 0x24 : 0;
        ctx->regs[0x0f] = sim_model == 1 ? 0x6b : sim_model == 2 ? 0x70 : 0;
        #else
        SPI_InitTypeDef *init = &bus->spi->Init;
        memset(init, 0, sizeof(*init));
        init->Mode = SPI_MODE_MASTER;
        init->Direction = SPI_DIRECTION_2LINES;
        init->NSS = SPI_NSS_SOFT;
        init->TIMode = SPI_TIMODE_DISABLE;
        init->CRCCalculation = SPI_CRCCALCULATION_DISABLE;
        init->CRCPolynomial = 7;
        init->FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
        init->MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;
        spi_set_params(bus, 0xffffffff, 1000000, 0, 0, 8, SPI_FIRSTBIT_MSB);
        mp_hal_pin_high(pin_cs);
        mp_hal_pin_output(pin_cs);
        // Set before calling spi_init so a partial init is cleaned up.
        ctx->bus_started = true;
        int error = spi_init(bus, false);
        if (error) { mp_raise_OSError(-error); }
        bus->spi->Instance->IER = 0;
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        #endif
        MP_STATE_VM(seekfree_spi_owners)[spi_id] = ctx;
        imu_delay(20);
        int config_error = imu_configure(ctx);
        if (config_error) { mp_raise_OSError(config_error); }
        ctx->active = true;
        nlr_pop();
    } else {
        imu_port_deinit(ctx);
        nlr_jump(nlr.ret_val);
    }
    return ctx;
}
