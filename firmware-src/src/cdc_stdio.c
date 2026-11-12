/*
 * cdc_stdio.c - 统一的 CDC 字节流数据层（按实例参数化）
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 直接操作 TinyUSB CDC 端点。不依赖 stdio，不依赖 DTR。
 * 注意：
 *  - 不调用 tud_task()（由 usb_thread 独占，跨线程调用非线程安全）。
 *  - 不检查 DTR/connected（libsigrok 不设 DTR，检查会丢数据）。
 *    tud_cdc_n_write / tud_cdc_n_available 在 endpoint 未打开时安全返回 0。
 *  - 2026-09-21 起按实例参数化：每个 itf 独立 TX 缓冲，LA(itf1)/PWM(itf2) 并行安全。
 *
 * 历史（2026-09-18 实测）：曾把发送改成"环形缓冲 + usb_thread drain"的单一写者
 * 模型，结果比直写更糟，已回退。以后若再做单一写者改造，必须有上板大采集数据支撑。
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "FreeRTOS.h"
#include "task.h"

#include "cdc_stdio.h"

/* 只用于 printf/putchar 这类小输出；LA 大块数据走 cdc_write_block 不经过这里 */
#define CDC_TX_BUF 1024

/* 主机停止读取后的等待上限（us）。略高于 libsigrok 的 100ms 空闲判定，
 * 既能给慢速主机留余量，又能在主机放弃后很快停下来。 */
#define CDC_TX_STALL_LIMIT_US 150000u

#define CDC_MAX_ITF 3   /* CFG_TUD_CDC = 3 */

static uint8_t tx_buf[CDC_MAX_ITF][CDC_TX_BUF];
static uint16_t tx_len[CDC_MAX_ITF];

int cdc_getchar_timeout(uint8_t itf, uint32_t timeout_us)
{
    if (!tud_cdc_n_available(itf)) {
        if (timeout_us == 0)
            return PICO_ERROR_TIMEOUT;

        uint32_t deadline = time_us_32() + timeout_us;
        while (!tud_cdc_n_available(itf)) {
            /* 有符号差值比较：time_us_32 会回绕，直接比大小会出错 */
            if ((int32_t)(time_us_32() - deadline) >= 0)
                return PICO_ERROR_TIMEOUT;
            taskYIELD();
        }
    }
    return (int)tud_cdc_n_read_char(itf);
}

void cdc_flush(uint8_t itf)
{
    if (tx_len[itf] == 0) return;

    uint16_t written = 0;
    uint32_t stall_start = 0;
    bool stalling = false;
    while (written < tx_len[itf]) {
        uint16_t n = tud_cdc_n_write(itf, tx_buf[itf] + written, tx_len[itf] - written);
        if (n == 0) {
            tud_cdc_n_write_flush(itf);
            if (!stalling) { stalling = true; stall_start = time_us_32(); }
            else if (time_us_32() - stall_start > CDC_TX_STALL_LIMIT_US) {
                break;
            }
            taskYIELD();
        } else {
            stalling = false;
            written += n;
        }
    }
    tud_cdc_n_write_flush(itf);
    tx_len[itf] = 0;
}

void cdc_putchar(uint8_t itf, char c)
{
    tx_buf[itf][tx_len[itf]++] = (uint8_t)c;
    if (tx_len[itf] >= CDC_TX_BUF)
        cdc_flush(itf);
}

/* 大块传输用：整块直接喂 TinyUSB，超时就返回已接受的长度（不丢数据，
 * 由调用者下一轮接着送）。 */
uint32_t cdc_write_block(uint8_t itf, const uint8_t *data, uint32_t len)
{
    if (len == 0) return 0;
    if (tx_len[itf]) cdc_flush(itf);   /* 先把软缓冲里的老数据推走，保持字节流顺序 */

    uint32_t sent = 0, stall_start = 0;
    bool stalling = false;

    while (sent < len) {
        uint16_t n = tud_cdc_n_write(itf, data + sent, (uint16_t)(len - sent));
        if (n == 0) {
            tud_cdc_n_write_flush(itf);
            if (!stalling) { stalling = true; stall_start = time_us_32(); }
            else if ((uint32_t)(time_us_32() - stall_start) > CDC_TX_STALL_LIMIT_US) break;
            taskYIELD();               /* 让 usb_thread 去 drain，别饿死它 */
        } else {
            stalling = false;
            sent += n;
        }
    }
    tud_cdc_n_write_flush(itf);
    return sent;
}

void cdc_printf(uint8_t itf, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    for (const char *p = tmp; *p; p++)
        cdc_putchar(itf, *p);
}
