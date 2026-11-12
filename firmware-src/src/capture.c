/*
 * Logic Analyzer RP2040-SUMP
 * Copyright (C) 2023 Daniel Gorbea <danielgorbea@hotmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "capture.h"

#include "capture.pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "string.h"

/* 无限流式采样。
 *
 * 数据格式：2 通道打包，每字节 4 个采样，采样 N 在 bit[2N:2N+1]（LSB 先），
 *   bit0 = CH0(pin_base+0)、bit1 = CH1(pin_base+1)。1MHz 采样 = 250KB/s。
 *
 * PIO `in pins,2`（4 采样=1 字节） -> DMA 环（32KB，带 reload）-> 任务 drain。
 * 32KB 环 = 131ms @250KB/s，用来吸收 Windows CDC 的突发停顿。
 * 采样率 = clk_sys / clkdiv；clk_sys=125MHz、1MHz 采样即 clkdiv=125。 */

#define RING_BITS 15                      /* 32KB 环：DMA ring 字段最大值，约 131ms 余量 */
#define RING_SIZE (1u << RING_BITS)
#define RING_MASK (RING_SIZE - 1u)
#define RING_TRANSFER_COUNT ((0xffffffffu / RING_SIZE) * RING_SIZE)

#define DEFAULT_RATE 1000000u             /* 1 MHz */

static const uint sm_capture_ = 1, dma_capture_ = 0, dma_reload_ = 1;
static const uint32_t reload_count_ = RING_TRANSFER_COUNT;

static uint offset_capture_, pin_base_, pin_count_;
static volatile uint32_t read_pos_;
static bool capturing_ = false;
static uint8_t ring_[RING_SIZE] __attribute__((aligned(RING_SIZE)));

void capture_init(uint pin_base, uint pin_count)
{
    pin_base_ = pin_base;
    pin_count_ = pin_count;

    for (uint i = 0; i < pin_count_; i++) {
        gpio_set_function(pin_base_ + i, GPIO_FUNC_SIO);
        gpio_set_dir(pin_base_ + i, false);
        gpio_pull_down(pin_base_ + i);
    }
}

static inline void dma_safe_stop(uint channel)
{
    hw_write_masked(&dma_hw->ch[channel].al1_ctrl,
                    (uint32_t)0x3f << DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB,
                    DMA_CH0_CTRL_TRIG_TREQ_SEL_BITS);
    dma_hw->abort = 1u << channel;
    for (uint32_t g = 100000u; g && (dma_hw->ch[channel].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS); g--)
        tight_loop_contents();
    hw_clear_bits(&dma_hw->ch[channel].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
}

void capture_start(uint rate)
{
    if (capturing_)
        capture_abort();

    uint32_t rate_hz = rate ? rate : DEFAULT_RATE;

    if (rate_hz > 2000000u)
        rate_hz = 2000000u;

    read_pos_ = 0;

    /* PIO：`in pins,2` 每周期读 2 位，4 采样(8bit)自动 push 1 字节。 */
    offset_capture_ = pio_add_program(pio0, &capture_program);
    pio_sm_config cfg = capture_program_get_default_config(offset_capture_);
    sm_config_set_in_pins(&cfg, pin_base_);
    sm_config_set_in_shift(&cfg, false, true, 8);   /* LSB 先，8bit 自动 push = 4 采样/字节 */
    float div = (float)clock_get_hz(clk_sys) / (float)rate_hz;
    if (div < 1.0f)
        div = 1.0f;
    sm_config_set_clkdiv(&cfg, div);
    pio_sm_init(pio0, sm_capture_, offset_capture_, &cfg);
    pio_sm_clear_fifos(pio0, sm_capture_);

    /* DMA 环：RX FIFO -> ring_，环形寻址，传完由 reload 通道重新装填。 */
    dma_channel_config dcfg = dma_channel_get_default_config(dma_capture_);
    channel_config_set_transfer_data_size(&dcfg, DMA_SIZE_8);
    channel_config_set_ring(&dcfg, true, RING_BITS);
    channel_config_set_write_increment(&dcfg, true);
    channel_config_set_read_increment(&dcfg, false);
    channel_config_set_dreq(&dcfg, pio_get_dreq(pio0, sm_capture_, false));
    channel_config_set_chain_to(&dcfg, dma_reload_);
    dma_channel_configure(dma_capture_, &dcfg, ring_, &pio0->rxf[sm_capture_], RING_TRANSFER_COUNT, true);

    dma_channel_config rcfg = dma_channel_get_default_config(dma_reload_);
    channel_config_set_transfer_data_size(&rcfg, DMA_SIZE_32);
    channel_config_set_write_increment(&rcfg, false);
    channel_config_set_read_increment(&rcfg, false);
    dma_channel_configure(dma_reload_, &rcfg,
                          &dma_hw->ch[dma_capture_].al1_transfer_count_trig,
                          &reload_count_, 1, false);

    pio_sm_set_enabled(pio0, sm_capture_, true);
    capturing_ = true;
}

void capture_abort(void)
{
    if (!capturing_)
        return;
    capturing_ = false;

    pio_sm_set_enabled(pio0, sm_capture_, false);
    dma_safe_stop(dma_capture_);
    dma_safe_stop(dma_reload_);
    pio_sm_clear_fifos(pio0, sm_capture_);
    /* 只移除 LA 自己的程序。之前用 pio_clear_instruction_memory(pio0) 会把 pio0
     * 全部 32 条指令清空，连带删掉 DAP SWD 位打(probe, SM0) 和 PWM(square_asym,
     * SM2/3) 的程序 -> 之后 SWD 完全停摆（目标无响应 ACK=0x07）。 */
    pio_remove_program(pio0, &capture_program, offset_capture_);
}

bool capture_is_busy(void) { return capturing_; }

static inline uint32_t write_pos(void)
{
    return (RING_TRANSFER_COUNT - dma_hw->ch[dma_capture_].transfer_count) & RING_MASK;
}

uint32_t capture_available(void)
{
    if (!capturing_)
        return 0;
    return (write_pos() - read_pos_) & RING_MASK;
}

uint32_t capture_read(uint8_t *dst, uint32_t max)
{
    uint32_t n = 0;
    while (n < max) {
        uint32_t avail = (write_pos() - read_pos_) & RING_MASK;
        if (avail == 0)
            break;
        uint32_t chunk = avail;
        if (chunk > max - n)
            chunk = max - n;
        uint32_t start = read_pos_ & RING_MASK;
        if (start + chunk <= RING_SIZE) {
            memcpy(dst + n, ring_ + start, chunk);
        } else {
            uint32_t first = RING_SIZE - start;
            memcpy(dst + n, ring_ + start, first);
            memcpy(dst + n + first, ring_, chunk - first);
        }
        read_pos_ += chunk;
        n += chunk;
    }
    return n;
}
