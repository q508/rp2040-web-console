/*
 * led_status.h - 单灯三态指示：空闲常亮 / 工作呼吸 / 异常闪烁
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LED_STATUS_H
#define LED_STATUS_H

#include <stdint.h>
#include <stdbool.h>

/* 初始化：把 LED 引脚切到 PWM 调光，上电常亮。
 * byte_counter 指向 LA 累计发送字节数，用于判断"还在出数据"。 */
void led_status_init(volatile uint32_t *byte_counter);

/* 每 1 ms 调用一次。running = LA 采集或 PWM 输出中。 */
void led_status_tick(uint32_t now_ms, bool running);

/* 外部显式报异常（如环形缓冲溢出），下一次 tick 起闪烁。 */
void led_status_fault(void);

#endif
