/*
 * mode_manager.c - combo (DAPLink + PWM + LA) manager
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 三功能并行（2026-09-21）：不再有"模式轮换"。
 *  - DAPLink：dap_thread(bulk) + cdc_thread(UART 桥，cdc_uart.c)，main.c 里创建。
 *  - LA：la_task 读 CDC#1（SUMP 握手 + 流式数据），并承担喂狗/落盘/LED。
 *  - PWM：pwm_task 读 CDC#2（文本命令）。
 * 时钟：统一 125MHz（SDK 默认），运行期不切。
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

#include "tusb.h"

#include "probe_config.h"
#include "mode_manager.h"
#include "pwm_mode.h"
#include "la_mode.h"
#include "cdc_stdio.h"
#include "combo_store.h"
#include "tusb_edpt_handler.h"
#include "capture.h"
#include "led_status.h"

TaskHandle_t la_taskhandle;
TaskHandle_t pwm_taskhandle;

/* CDC-side debug print flag. Default off, toggled by "debug on|off", never persisted. */
static volatile bool dbg_enabled = false;

bool combo_dbg_enabled(void) { return dbg_enabled; }
void combo_dbg_set(bool on) { dbg_enabled = on; }

/* ============ 时钟：保持 SDK 默认 125MHz，上电不碰时钟 ============
 *
 * 用户反馈（2026-09-18）：上电切时钟偶发导致 LED 不亮（疑似 PLL 重配置在电源
 * 抖动下 hard_assert 挂死）。结论：彻底不在上电时改时钟，全程 125MHz ——
 * 最稳，也满足"三功能同一频率、运行期永不切"。
 * PWM 最高输出 = clk_sys/6 ≈ 20.8MHz；LA 1MHz 采样分频 125，都绰绰有余。 */
static void combo_clocks_init(void)
{
    /* 空操作：保持 board_init() 设好的 125MHz。 */
}

/* ============ housekeeping：喂狗 + 延迟落盘 ============ */

static bool wd_inited = false;
static void housekeeping(void)
{
    /* 看门狗在任务首次进循环时才 enable：不能在 main() 早 enable，因为喂狗任务
     * 要等 Windows 枚举完（tud_mount_cb）才被创建，枚举慢（>3s）会被早开的
     * 看门狗复位 → 设备反复重启。la_task/pwm_task 都会调这里，单核下 bool 保护够。 */
    if (!wd_inited) { watchdog_enable(3000, false); wd_inited = true; }
    watchdog_update();      /* 喂狗：任务挂死 3 秒后自动复位 */
    store_poll();           /* 设置延迟落盘（脏且安静够久才写 flash） */
}

/* ============ 全局文本命令 ============ */

bool mode_handle_line(uint8_t itf, const char *line, int len)
{
    if (dbg_enabled) {
        cdc_printf(itf, "[dbg] len=%d line=", len);
        for (int i = 0; i < len && i < 16; i++)
            cdc_printf(itf, "%02x ", (uint8_t)line[i]);
        cdc_printf(itf, "\n");
        cdc_flush(itf);
    }

    if (strcmp(line, "debug on") == 0)  { combo_dbg_set(true);  cdc_printf(itf, "debug on\n");  cdc_flush(itf); return true; }
    if (strcmp(line, "debug off") == 0) { combo_dbg_set(false); cdc_printf(itf, "debug off\n"); cdc_flush(itf); return true; }

    if (strcmp(line, "save") == 0) {
        store_flush();
        cdc_printf(itf, "settings saved\n");
        cdc_flush(itf);
        return true;
    }
    if (strcmp(line, "bootsel") == 0 || strcmp(line, "update") == 0) {
        /* 软重启进 USB 烧录模式（出现 RPI-RP2 盘），免按 BOOTSEL 更新固件 */
        cdc_printf(itf, "[combo] 重启进入 BOOTSEL/UF2 烧录模式...\n");
        cdc_flush(itf);
        reset_usb_boot(0, 0);
        return true;
    }
    return false;
}

/* MLINE_MAX：LA 模式下 'm' 文本命令的行缓冲上限 */
#define MLINE_MAX 16

/* ============ la_task: owns CDC#1（SUMP + 流式）============ */

void la_task(void *ptr)
{
    (void) ptr;

    for (;;) {
        housekeeping();

        /* LED 三态：空闲常亮 / 工作呼吸 / 异常闪。
         * "工作" 只认 LA 采集：PWM 常开是常态，若也算工作就永远看不到空闲。 */
        led_status_tick(to_ms_since_boot(get_absolute_time()),
                        capture_is_busy());

        int c = cdc_getchar_timeout(1, 0);
        if (c == PICO_ERROR_TIMEOUT) {
            /* 无字节也 drain 流式数据 */
            la_mode_poll();
            vTaskDelay(1);
            continue;
        }

        if (c == 'm') {
            /* 'm' 不是合法 SUMP 命令，用作文本转义前缀。累加直到换行或 100ms 超时，
             * 避免快速键入的文本被拆到多个 USB 包里。 */
            uint8_t l[MLINE_MAX];
            int n = 0;
            l[n++] = 'm';
            while (n < MLINE_MAX - 1) {
                int d = cdc_getchar_timeout(1, 100000);
                if (d == PICO_ERROR_TIMEOUT || d == '\n' || d == '\r') break;
                l[n++] = (uint8_t)d;
            }
            l[n] = 0;
            if (n > 0) {
                const char *cmd = (const char *)l;
                int clen = n;
                if (strncmp(cmd, "mode", 4) != 0) { cmd++; clen--; }
                if (!mode_handle_line(1, cmd, clen))
                    la_handle_line(cmd, clen);
            }
            la_mode_poll();
        } else {
            la_mode_poll_byte((uint8_t)c);   /* 内部会调 la_mode_poll() */
        }
    }
}

/* ============ pwm_task: owns CDC#2（文本命令）============ */

void pwm_task(void *ptr)
{
    (void) ptr;

    static uint8_t pline[64];
    static int pline_len = 0;

    for (;;) {
        housekeeping();

        int c = cdc_getchar_timeout(2, 0);
        if (c == PICO_ERROR_TIMEOUT) {
            cdc_flush(2);
            vTaskDelay(1);
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (pline_len > 0) {
                pline[pline_len] = 0;
                if (!mode_handle_line(2, (const char *)pline, pline_len))
                    pwm_handle_line((const char *)pline, pline_len);
                pline_len = 0;
            }
        } else if (pline_len < (int)sizeof(pline) - 1) {
            pline[pline_len++] = (uint8_t)c;
        }
    }
}

/* ============ boot ============ */

/* Boot-time: restore persisted settings (PWM/LA), init LED. */
void mode_init(void)
{
    store_init();
    led_status_init(&la_bytes_total);
    combo_clocks_init();
}

/* Boot-time resource init（调度器未启动、USB 未枚举）：
 * PWM 无主机也要输出波形；LA 配置 PIO/GPIO。两者都不依赖 USB 主机。 */
void mode_boot_apply(void)
{
    pwm_mode_init();
    la_mode_init();
}
