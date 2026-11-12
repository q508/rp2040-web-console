/*
 * la_mode.h - 2-channel logic analyzer (SUMP streaming)
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 复用 logic_analyzer_rp2040 的 capture.c + protocol_sump.c。
 * SUMP 字节流通过 cdc_stdio 重定向到 USB CDC#1。
 * 当前为无限流式（4 采样/字节，1MHz，32KB DMA 环）。
 */

#ifndef LA_MODE_H
#define LA_MODE_H

#include <stdint.h>

void la_mode_init(void);
void la_mode_deinit(void);

/* Feed one host->device byte into the SUMP command dispatcher. Non-blocking. */
void la_mode_poll_byte(uint8_t c);

/* Drain the DMA ring to CDC#1. Must be called every loop iteration even when
 * no byte arrived. Retries partial cdc_write_block() sends without losing data. */
void la_mode_poll(void);

/* 处理 LA 文本命令（mi/mhelp/mtrig/ladebug），返回 true=已消费。
 * 调用者负责去掉 LA 模式下的 'm' 转义前缀。 */
bool la_handle_line(const char *line, int len);

/* 已发送字节总数（单调递增，供 LED 判断数据是否在流动） */
extern volatile uint32_t la_bytes_total;

#endif
