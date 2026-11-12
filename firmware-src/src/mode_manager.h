/*
 * mode_manager.h - combo (DAPLink + PWM + LA) manager
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 三功能并行（2026-09-21）：不再有"模式轮换"，DAPLink / PWM / LA 同时在线。
 * 一个复合 USB 设备：IF0 CMSIS-DAP + CDC#0(UART桥) + CDC#1(LA) + CDC#2(PWM) + Reset。
 * DAPLink 由 dap_thread + cdc_thread（cdc_uart.c）跑；LA / PWM 各一个任务。
 */

#ifndef MODE_MANAGER_H
#define MODE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"

/* Boot-time: init LED + restore persisted settings. Safe before scheduler. */
void mode_init(void);

/* Boot-time resource init（PWM 无主机也要输出波形，LA 配置 PIO/GPIO）。
 * Call after mode_init(), before the scheduler starts. */
void mode_boot_apply(void);

/* la_task: owns CDC#1（SUMP 握手 + 流式数据），并承担喂狗/落盘/LED 等 housekeeping。 */
void la_task(void *ptr);

/* pwm_task: owns CDC#2（文本命令）。 */
void pwm_task(void *ptr);

extern TaskHandle_t la_taskhandle;
extern TaskHandle_t pwm_taskhandle;

/* 全局文本命令（debug on|off / save / bootsel），返回 true=已消费。
 * itf 是当前任务的 CDC 实例号（la_task 传 1，pwm_task 传 2）。 */
bool mode_handle_line(uint8_t itf, const char *line, int len);

/* Debug print flag（CDC-side，默认 off，debug on|off 切换，不持久化）。 */
bool combo_dbg_enabled(void);
void combo_dbg_set(bool on);

#endif
