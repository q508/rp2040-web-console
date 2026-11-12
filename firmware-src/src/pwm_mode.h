/*
 * pwm_mode.h - 2 路独立 PWM 信号发生器
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * CHAN0-1 = GPIO6-7，每路一个 PIO 状态机（pio0 SM2-3，避开 DAP SWD 的 SM0 与
 * LA 流式的 SM1），频率与占空比完全独立。
 *
 * 时钟：clk_sys 固定 125MHz（见 mode_manager.c），运行期不切时钟，本模块只在
 * 初始化时按 clock_get_hz(clk_sys) 算一次计数。
 * 2 路参数通过 combo_store 持久化，开机自动恢复。
 */

#ifndef PWM_MODE_H
#define PWM_MODE_H

#include <stdint.h>
#include <stdbool.h>

#define PWM_NUM_CHANNELS 2

/* 输出频率上限 = clk_sys/6：PIO 每个波形周期固定要 6 个时钟
 * （mov x + set pins,1 + jmp + mov x + set pins,0 + jmp）。
 * clk_sys=125MHz 时为 20.8MHz，这里留余量取 20MHz。 */
#define PWM_MAX_FREQ_HZ 20000000u

/* PWM 模式初始化：PIO/GPIO 配置 + 从 flash 恢复参数并输出。
 * 可重复调用（第二次起为空操作）。 */
void pwm_mode_init(void);

/* PWM 模式反初始化（释放 PIO SM / GPIO） */
void pwm_mode_deinit(void);

/* 按当前 clk_sys 重算并输出全部通道（初始化时用；运行期改了 clk_sys 也要调） */
void pwm_mode_apply_all(void);

/* 设置某通道频率（Hz，1..PWM_MAX_FREQ_HZ）与占空比（0..100，支持小数） */
void pwm_set_freq(uint ch, uint32_t hz);
void pwm_set_duty(uint ch, float pct);
void pwm_set_enable(uint ch, bool on);

/* 是否至少有一路在输出（供 LED 判断"工作中"） */
bool pwm_mode_any_enabled(void);

/* 处理一行 PWM 命令（f/d/on/off/i/save/help），返回 true=已消费 */
bool pwm_handle_line(const char *line, int len);

#endif
