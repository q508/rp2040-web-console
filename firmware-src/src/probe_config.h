/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#ifndef PROBE_CONFIG_H_
#define PROBE_CONFIG_H_

#include "FreeRTOS.h"
#include "task.h"

#if false
#define probe_info(format,args...) \
do { \
	vTaskSuspendAll(); \
	printf(format, ## args); \
	xTaskResumeAll(); \
} while (0)
#else
#define probe_info(format,...) ((void)0)
#endif


#if false
#define probe_debug(format,args...) \
do { \
	vTaskSuspendAll(); \
	printf(format, ## args); \
	xTaskResumeAll(); \
} while (0)
#else
#define probe_debug(format,...) ((void)0)
#endif

#if false
#define probe_dump(format,args...)\
do { \
	vTaskSuspendAll(); \
	printf(format, ## args); \
	xTaskResumeAll(); \
} while (0)
#else
#define probe_dump(format,...) ((void)0)
#endif

// TODO tie this up with PICO_BOARD defines in the main SDK

#ifdef DEBUG_ON_PICO 
#include "board_pico_config.h"
#else
#include "board_debug_probe_config.h"
#endif
//#include "board_example_config.h"

/* 三者同时在线（2026-09-21 改造）：PWM 2 路 / LA 2 路 / DAPLink 4 脚，引脚错开不再复用。
 *   DAPLink: SWCLK=GPIO2, SWDIO=GPIO3, UART_TX=GPIO4, UART_RX=GPIO5（无 nRESET）
 *   PWM    : CH0=GPIO6, CH1=GPIO7
 *   LA     : CH0=GPIO8, CH1=GPIO9（连续，PIO `in pins` 按连续引脚读）
 * 迁移中：PWM_CH_ 与 LA_CH_ 是目标分配；旧的 COMBO_CH_ 暂留，等 USB/任务/流式
 * 改造完成后再删除。换板只改这里 + 上位机通道映射。 */
#define PWM_CH_PIN_BASE 6
#define PWM_CH_PIN_COUNT 2
#define LA_CH_PIN_BASE 8
#define LA_CH_PIN_COUNT 2

#define COMBO_CH_PIN_BASE 2
#define COMBO_CH_PIN_COUNT 8

// Add the configuration to binary information
void bi_decl_config();

#define PROTO_DAP_V1 1
#define PROTO_DAP_V2 2

// Interface config
#ifndef PROBE_DEBUG_PROTOCOL
#define PROBE_DEBUG_PROTOCOL PROTO_DAP_V2
#endif

#endif
