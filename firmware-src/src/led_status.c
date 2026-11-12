/*
 * led_status.c - 单灯三态指示：空闲常亮 / 工作呼吸 / 异常闪烁
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 状态只有三种，一眼可辨：
 *   空闲（没在采集/输出）: 常亮
 *   工作中（LA 采集中或 PWM 有输出）: 呼吸，0.5 Hz，亮度 20%~100%
 *   异常（采集中 1.5 s 无新数据，或显式报错）: 2 Hz 闪
 *
 * 亮度用 GPIO25 的硬件 PWM（slice4 B）实现：125 MHz / 125 = 1 MHz 计数，
 * wrap 999 -> 1 kHz，对人眼无频闪。PIO 的 PWM 输出不占用 PWM 外设，无冲突。
 */

#include "led_status.h"

#include "board_pico_config.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

#define LED_PIN       PROBE_USB_CONNECTED_LED
#define LED_TOP       999u    /* 1 kHz */
#define LED_FAULT_MS  1500u   /* 采集中超过该时间没有新字节 -> 异常 */

/* 呼吸曲线：16 点，每点 125 ms，2 s 一个来回 */
static const uint8_t kBreath[16] = {
    20, 23, 29, 36, 45, 55, 65, 75,
    84, 91, 96, 99, 100, 99, 96, 91
};

static struct {
    bool     running;
    bool     fault;
    uint32_t t0;            /* 当前状态起点，用于呼吸/闪烁相位 */
    uint32_t last_bytes;
    uint32_t last_rx;
} s;

static volatile uint32_t *bytes_src;

static void led_level(uint32_t level)
{
    pwm_set_gpio_level(LED_PIN, (uint16_t)level);
}

void led_status_init(volatile uint32_t *byte_counter)
{
    bytes_src = byte_counter;

    gpio_set_function(LED_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(LED_PIN);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 125.0f);   /* 125 MHz / 125 = 1 MHz */
    pwm_config_set_wrap(&cfg, LED_TOP);
    pwm_init(slice, &cfg, true);

    s.running = false;
    s.fault = false;
    s.t0 = 0;
    s.last_bytes = byte_counter ? *byte_counter : 0;
    s.last_rx = to_ms_since_boot(get_absolute_time());

    led_level(LED_TOP);                    /* 上电常亮 = 空闲 */
}

void led_status_fault(void)
{
    s.fault = true;
}

void led_status_tick(uint32_t now_ms, bool running)
{
    uint32_t b = bytes_src ? *bytes_src : 0;
    if (b != s.last_bytes) {
        s.last_bytes = b;
        s.last_rx = now_ms;
    }

    if (running != s.running) {            /* 状态切换：相位归零 */
        s.running = running;
        s.t0 = now_ms;
        s.last_rx = now_ms;
        if (!running)
            s.fault = false;               /* 停下即清异常 */
    }

    if (running && (now_ms - s.last_rx) > LED_FAULT_MS)
        s.fault = true;                    /* 说是采集却不出数 = 异常 */

    if (s.fault) {
        led_level((((now_ms - s.t0) / 250u) & 1u) ? LED_TOP : 0u);
    } else if (!running) {
        led_level(LED_TOP);
    } else {
        uint32_t pct = kBreath[((now_ms - s.t0) / 125u) & 15u];
        led_level(pct * LED_TOP / 100u);
    }
}
