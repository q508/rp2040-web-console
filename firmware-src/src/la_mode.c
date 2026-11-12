/*
 * la_mode.c - 2-channel logic analyzer (SUMP streaming)
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 复用 logic_analyzer_rp2040 的 capture.c + protocol_sump.c。
 * SUMP 字节流通过 cdc_stdio 重定向到 USB CDC#1。
 *
 * 当前模式：无限流式采样
 *   - 2 通道（GPIO8/9），每字节 4 采样（bit0=CH0、bit1=CH1，LSB 先）
 *   - 1 MHz 采样 = 250 KB/s，32 KB DMA 环吸收主机反压
 *   - 无触发、无 RLE
 *
 * 文本命令以 'm' 开头（0x6D 不是合法 SUMP 命令）。
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

#include "cdc_stdio.h"
#include "common.h"
#include "capture.h"
#include "protocol_sump.h"
#include "la_mode.h"
#include "combo_store.h"
#include "probe_config.h"

_Static_assert(CHANNEL_COUNT == LA_CH_PIN_COUNT,
               "CHANNEL_COUNT must match LA_CH_PIN_COUNT");

/* ---- LA globals (originally defined in LA main.c) ---- */
config_t config_;
capture_config_t capture_config_;
static char debug_message_[DEBUG_BUFFER_SIZE];

/* 发送 staging：只有上一块完全被 CDC 接受后才从环里取下一块。
 * cdc_write_block() 在主机反压时会返回已接受长度；不能丢弃剩余字节。 */
static uint8_t stage_[256];
static uint32_t stage_len_, stage_off_;

/* 已通过 CDC#1 发出的字节总数：LED 用它判断"还在出数据" */
volatile uint32_t la_bytes_total;

static void set_pin_config(void)
{
    /* GPIO18=debug enable, GPIO19=trigger stages（本板未引出，读回恒为高） */
    gpio_init_mask((1 << GPIO_DEBUG_ENABLE) | (1 << GPIO_TRIGGER_STAGES));
    gpio_set_dir_masked((1 << GPIO_DEBUG_ENABLE) | (1 << GPIO_TRIGGER_STAGES), false);
    gpio_pull_up(GPIO_TRIGGER_STAGES);
    gpio_pull_up(GPIO_DEBUG_ENABLE);

    /* 默认值来自 flash 持久化设置 */
    config_.trigger_edge = store_la_trigger_edge() != 0;
    config_.debug = store_la_debug() != 0;

    /* 硬件跳线（如接了）优先 */
    if (!gpio_get(GPIO_TRIGGER_STAGES)) config_.trigger_edge = false;
    if (!gpio_get(GPIO_DEBUG_ENABLE)) config_.debug = true;
}

void la_mode_init(void)
{
    /* 流式 2 通道：clk_sys 保持 125MHz（1MHz 采样即分频 125）。 */
    set_pin_config();
    config_.channels = capture_config_.channels = CHANNEL_COUNT;

    debug_init(115200, &debug_message_[0], &config_.debug);

    capture_init(LA_CH_PIN_BASE, capture_config_.channels);
}

void la_mode_deinit(void)
{
    if (capture_is_busy())
        capture_abort();
    for (uint i = 0; i < CHANNEL_COUNT; i++)
        gpio_deinit(LA_CH_PIN_BASE + i);
}

/* Feed one byte into the SUMP dispatcher. */
void la_mode_poll_byte(uint8_t c)
{
    command_t command = sump_dispatch((int)c);

    if (command == COMMAND_CAPTURE) {
        /* run：开始流式采样（采样率由 SUMP 分频比得出，默认 1MHz） */
        capture_start(capture_config_.rate);
    } else if (command == COMMAND_RESET) {
        if (capture_is_busy()) capture_abort();
        stage_len_ = stage_off_ = 0;
        sump_reset();
    }

    la_mode_poll();
}

/* Polled every la_task loop iteration (even when no byte is available).
 *
 * 仅当 staging 已完全发送时才从 32KB 环取下一块；cdc_write_block() 返回的
 * sent 可能小于 chunk，剩余部分下一轮继续，不丢样本。 */
void la_mode_poll(void)
{
    if (stage_off_ >= stage_len_) {
        stage_len_ = capture_read(stage_, sizeof(stage_));
        stage_off_ = 0;
    }

    if (stage_len_) {
        uint32_t sent = cdc_write_block(1, stage_ + stage_off_, stage_len_ - stage_off_);
        stage_off_ += sent;
        la_bytes_total += sent;
        if (stage_off_ >= stage_len_) {
            stage_len_ = 0;
            stage_off_ = 0;
        }
    }

    cdc_flush(1);
}

/* ---- text commands (LA 模式下以 'm' 作为转义前缀) ---- */

static void print_status(void)
{
    cdc_printf(1, "LA: %u ch streaming (GPIO%d-%d), clk_sys=%u MHz\n",
               (unsigned)CHANNEL_COUNT, LA_CH_PIN_BASE,
               LA_CH_PIN_BASE + CHANNEL_COUNT - 1,
               (unsigned)(clock_get_hz(clk_sys) / 1000000u));
    cdc_printf(1, "    stream 1 MHz, 2 ch packed 4 samples/byte, 32KB ring, no trigger/RLE\n");
    cdc_printf(1, "    text cmds need 'm' prefix: mi mhelp mladebug on|off msave\n");
    if (store_dirty())
        cdc_printf(1, "    (settings pending, auto-save in ~1.5s)\n");
    cdc_flush(1);
}

static void print_help(void)
{
    cdc_printf(1, "LA commands (type with the 'm' prefix):\n");
    cdc_printf(1, "  mi            show status\n");
    cdc_printf(1, "  mladebug on|off    LA debug output on GPIO16 @115200\n");
    cdc_printf(1, "  msave         write settings to flash now\n");
    cdc_flush(1);
}

bool la_handle_line(const char *line, int len)
{
    (void) len;

    if (strcmp(line, "i") == 0) { print_status(); return true; }
    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) { print_help(); return true; }

    if (strcmp(line, "trig") == 0) {
        cdc_printf(1, "trigger setting = %s (not implemented in capture)\n",
                   config_.trigger_edge ? "edge" : "level");
        cdc_flush(1);
        return true;
    }
    if (strncmp(line, "trig ", 5) == 0) {
        const char *arg = line + 5;
        if (strcmp(arg, "edge") == 0) {
            config_.trigger_edge = true;
            store_la_set_trigger_edge(1);
            cdc_printf(1, "trigger = edge (not implemented in capture)\n");
        } else if (strcmp(arg, "level") == 0) {
            config_.trigger_edge = false;
            store_la_set_trigger_edge(0);
            cdc_printf(1, "trigger = level (not implemented in capture)\n");
        } else {
            cdc_printf(1, "usage: trig edge|level\n");
        }
        cdc_flush(1);
        return true;
    }

    if (strcmp(line, "ladebug") == 0) {
        cdc_printf(1, "ladebug = %s (usage: ladebug on|off)\n", config_.debug ? "on" : "off");
        cdc_flush(1);
        return true;
    }
    if (strncmp(line, "ladebug ", 8) == 0) {
        const char *arg = line + 8;
        if (strcmp(arg, "on") == 0) {
            config_.debug = true;
            store_la_set_debug(1);
            /* 使能 GPIO16 上的 UART0 调试输出（关闭时只是不再打印，不拆 UART） */
            debug_init(115200, debug_message_, &config_.debug);
            cdc_printf(1, "ladebug = on (GPIO16 @115200)\n");
        } else if (strcmp(arg, "off") == 0) {
            config_.debug = false;
            store_la_set_debug(0);
            cdc_printf(1, "ladebug = off\n");
        } else {
            cdc_printf(1, "usage: ladebug on|off\n");
        }
        cdc_flush(1);
        return true;
    }

    return false;
}
