/*
 * cdc_stdio.h - 统一的 CDC 字节流数据层（按实例参数化，3 路 CDC 并行）
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * debugprobe 的 CDC 接口原来由 cdc_uart.c 手动管理。三功能并行改造（2026-09-21）
 * 后有三路 CDC，本层按实例参数化，供 LA/PWM 两个任务各自使用：
 *   itf 0 = CDC#0（UART 桥，由 cdc_uart.c 直接操作，不经过本层）
 *   itf 1 = CDC#1（LA：SUMP 握手 + 流式数据）
 *   itf 2 = CDC#2（PWM：文本命令）
 * 每个实例有独立的 TX 缓冲，LA/PWM 两个任务并行调用互不干扰。
 *
 * 直接操作 TinyUSB CDC 端点，不依赖 stdio，不依赖 DTR（libsigrok 不设 DTR）。
 * 约束：tud_task() 由 usb_thread 独占，本层只调 tud_cdc_n_*（endpoint 未打开时安全返回 0）。
 */

#ifndef CDC_STDIO_H
#define CDC_STDIO_H

#include <stdint.h>

/* 非阻塞读一个字节，超时返回 PICO_ERROR_TIMEOUT(-1)。timeout_us=0 立即返回。 */
int cdc_getchar_timeout(uint8_t itf, uint32_t timeout_us);

/* 写一个字节到 CDC TX（内部缓冲，自动 flush） */
void cdc_putchar(uint8_t itf, char c);

/* 把一段数据推给 CDC TX，返回**实际被接受的字节数**（可能小于 len）。
 * 主机不取走时超时返回已接受长度、不丢数据，由调用者下一轮接着送。 */
uint32_t cdc_write_block(uint8_t itf, const uint8_t *data, uint32_t len);

/* 格式化输出到 CDC TX */
void cdc_printf(uint8_t itf, const char *fmt, ...);

/* 立刻 flush TX 缓冲 */
void cdc_flush(uint8_t itf);

#endif
