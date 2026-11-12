/*
 * Logic Analyzer RP2040-SUMP
 * Copyright (C) 2023 Daniel Gorbea <danielgorbea@hotmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PROTOCOL_SUMP_H
#define PROTOCOL_SUMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"

extern capture_config_t capture_config_;
extern config_t config_;

uint sump_read(void);
uint sump_dispatch(int c);
void sump_reset(void);

/* 流式（2026-09-21）：采集数据不再走"缓冲后整批发"的状态机，而是由 la_mode_poll()
 * 每轮从 capture.c 的 DMA 环里 drain 新字节直接写 CDC。这里不再需要发送状态机。 */

#ifdef __cplusplus
}
#endif

#endif
