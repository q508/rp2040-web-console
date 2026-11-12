/*
 * combo_store.h - 三合一固件的统一 flash 设置存储
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 单个 4KB 扇区（flash 末尾）保存：当前模式 + PWM 8 路参数 + LA 配置。
 * 内存里有一份 RAM 副本，读写都在 RAM 上完成；改动用 store_mark_dirty()
 * 标脏，由 store_poll() 在"安静" STORE_QUIET_MS 之后落盘（省 flash 擦写次数
 * 并避免频繁擦写导致的信号中断），store_flush() 可立即落盘（切模式前用）。
 */

#ifndef COMBO_STORE_H
#define COMBO_STORE_H

#include <stdint.h>
#include <stdbool.h>

#define STORE_PWM_CHANNELS 8

/* 单通道 PWM 参数（与 flash 布局一致，勿随意调整字段顺序） */
typedef struct __attribute__((aligned(4))) {
    uint32_t freq_hz;
    float duty_pct;   /* 0.0 - 100.0 */
    uint32_t enabled; /* 0/1 */
} store_pwm_ch_t;

/* 开机时从 flash 读取设置到 RAM 副本；无有效记录时使用默认值 */
void store_init(void);

/* 模式 */
uint32_t store_mode(void);
void store_set_mode(uint32_t mode);

/* PWM 通道参数 */
void store_pwm_get(int ch, uint32_t *freq_hz, float *duty_pct, bool *enabled);
void store_pwm_set(int ch, uint32_t freq_hz, float duty_pct, bool enabled);

/* LA 配置：trigger_edge = 1 边沿触发 / 0 电平触发；la_debug = 1 打开调试输出 */
uint32_t store_la_trigger_edge(void);
void store_la_set_trigger_edge(uint32_t edge);
uint32_t store_la_debug(void);
void store_la_set_debug(uint32_t on);

/* 标脏 + 延迟落盘 */
void store_mark_dirty(void);
bool store_dirty(void);

/* 立即落盘（中断/SysTick 会被临时关闭 ~50ms） */
void store_flush(void);

/* 周期调用（mode_task 循环，~1ms）：脏且安静够久则落盘 */
void store_poll(void);

#endif
