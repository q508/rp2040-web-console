/*
 * Logic Analyzer RP2040-SUMP
 * Copyright (C) 2023 Daniel Gorbea <danielgorbea@hotmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef CAPTURE_H
#define CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"

/* 无限流式采样：2 通道，4 采样/字节（bit0=CH0、bit1=CH1，LSB 先）。
 * PIO `in pins,2` + 8bit autopush -> DMA 环（32KB，带 reload）-> la_task drain。
 * 1MHz 采样 = 250KB/s；32KB 环可吸收约 131ms 的主机反压。 */

void capture_init(uint pin_base, uint pin_count);

/* 开始连续流式采样。rate 单位 Hz，0 = 默认 1MHz；上限 2MHz（USB FS 限制）。 */
void capture_start(uint rate);

void capture_abort(void);
bool capture_is_busy(void);

/* 环里当前可读字节数。 */
uint32_t capture_available(void);

/* 从环里 drain 最多 max 字节到 dst，返回实际字节数。 */
uint32_t capture_read(uint8_t *dst, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif
