/*
 * combo_store.c - 三合一固件的统一 flash 设置存储
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 存储位置：flash 最后一个 4KB 扇区（由 PICO_FLASH_SIZE_BYTES 推导，不再写死
 * 16MB，换 2MB/4MB 的板子也不会越界）。整条记录 = magic + version + 数据 +
 * CRC32，掉电写坏时 CRC 校验失败自动回落到默认值。
 *
 * 写 flash 的注意事项（沿用原 mode_persist 的做法）：
 *  - Cortex-M 的 SysTick 是系统异常，PRIMASK 屏蔽不了它；而 flash 擦写期间
 *    XIP 是停的，SysTick 中断取向量会取到 flash → 死锁。所以先停 SysTick
 *    再关中断。
 *  - FreeRTOS 只用单核（configNUMBER_OF_CORES=1），不存在另一个核取指的问题，
 *    因此不需要 multicore_lockout（它与 RP2040 port 的 SIO FIFO 中断冲突）。
 *  - SDK 的 flash_range_erase/program 本身是 RAM 函数（__no_inline_not_in_flash_func），
 *    并会自行 flush XIP cache，所以固件仍然可以放在 flash 里跑（XIP）。
 */

#include <stddef.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include "combo_store.h"

#define STORE_MAGIC   0x4D4F4445u /* "EDOM" */
#define STORE_VERSION 3u
/* v2 -> v3：LA 触发类型默认值由 edge(1) 改为 level(0)。见 store_init() 的迁移。 */
#define STORE_VERSION_LEGACY 2u

/* 最后一次修改后安静多久才落盘（ms）。避免 GUI 连续扫频时把 flash 写爆，
 * 同时避免频繁的 ~50ms 中断屏蔽。 */
#define STORE_QUIET_MS 1500u

/* Cortex-M SysTick 控制寄存器（系统异常，PRIMASK 屏蔽不了） */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010UL)

/* 落盘记录：整页 256 字节，尾部用 0xFF 填充 */
typedef struct __attribute__((aligned(4))) {
    uint32_t magic;
    uint32_t version;
    uint32_t mode;
    uint32_t la_trigger_edge;
    uint32_t la_debug;
    store_pwm_ch_t pwm[STORE_PWM_CHANNELS];
    uint32_t crc; /* 前面所有字节的 CRC32 */
} store_slot_t;

#define STORE_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

/* flash 镜像缓冲（必须是整页大小，flash_range_program 要求页对齐长度） */
static uint8_t slot_buf[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
static store_slot_t *const slot = (store_slot_t *)slot_buf;

static volatile bool dirty = false;
static volatile uint32_t dirty_ms = 0;

static uint32_t crc32_calc(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

static uint32_t slot_crc(void)
{
    return crc32_calc(slot_buf, offsetof(store_slot_t, crc));
}

static void store_set_defaults(void)
{
    memset(slot_buf, 0xFF, sizeof(slot_buf));
    slot->mode = 0; /* MODE_DAPLINK */
    slot->la_trigger_edge = 0;
    slot->la_debug = 0;
    for (int ch = 0; ch < STORE_PWM_CHANNELS; ch++) {
        slot->pwm[ch].freq_hz = 1000;
        slot->pwm[ch].duty_pct = 50.0f;
        slot->pwm[ch].enabled = 1;
    }
}

void store_init(void)
{
    const store_slot_t *flash_slot = (const store_slot_t *)(XIP_BASE + STORE_FLASH_OFFSET);
    bool crc_ok = (flash_slot->magic == STORE_MAGIC) &&
                  (flash_slot->crc == crc32_calc((const uint8_t *)flash_slot,
                                                 offsetof(store_slot_t, crc)));

    if (crc_ok && (flash_slot->version == STORE_VERSION ||
                   flash_slot->version == STORE_VERSION_LEGACY)) {
        memcpy(slot_buf, flash_slot, sizeof(slot_buf));

        if (flash_slot->version != STORE_VERSION) {
            /* v2 -> v3 迁移：模式/PWM 等参数原样保留，只把 LA 触发类型改为 level。
             * v2 默认是 edge，而 PulseView 只能发表触发，edge 解释下静态电平永远
             * 等不到边沿 → 采集不出数（见 2026-09 的 LA 问题定位）。 */
            slot->la_trigger_edge = 0;
            dirty_ms = 0;   /* 让 store_poll 尽快把 v3 记录写回 */
            dirty = true;
        } else {
            dirty = false;
        }
        return;
    }

    /* 无记录 / 版本不符 / CRC 失败：用默认值，不立刻写回（第一次修改时才落盘） */
    store_set_defaults();
    dirty = false;
}

void store_mark_dirty(void)
{
    dirty_ms = to_ms_since_boot(get_absolute_time());
    dirty = true;
}

bool store_dirty(void)
{
    return dirty;
}

void store_flush(void)
{
    if (!dirty)
        return;

    slot->magic = STORE_MAGIC;
    slot->version = STORE_VERSION;
    memset(&slot_buf[sizeof(store_slot_t)], 0xFF, sizeof(slot_buf) - sizeof(store_slot_t));
    slot->crc = slot_crc();

    uint32_t syst_csr = SYST_CSR;
    SYST_CSR = 0; /* 停 FreeRTOS tick，见文件头说明 */

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(STORE_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(STORE_FLASH_OFFSET, slot_buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);

    SYST_CSR = syst_csr;
    dirty = false;
}

void store_poll(void)
{
    if (!dirty)
        return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((uint32_t)(now - dirty_ms) >= STORE_QUIET_MS)
        store_flush();
}

uint32_t store_mode(void)
{
    return slot->mode;
}

void store_set_mode(uint32_t mode)
{
    if (slot->mode == mode)
        return;
    slot->mode = mode;
    store_mark_dirty();
}

void store_pwm_get(int ch, uint32_t *freq_hz, float *duty_pct, bool *enabled)
{
    if (ch < 0 || ch >= STORE_PWM_CHANNELS)
        return;
    if (freq_hz) *freq_hz = slot->pwm[ch].freq_hz;
    if (duty_pct) *duty_pct = slot->pwm[ch].duty_pct;
    if (enabled) *enabled = slot->pwm[ch].enabled != 0;
}

void store_pwm_set(int ch, uint32_t freq_hz, float duty_pct, bool enabled)
{
    if (ch < 0 || ch >= STORE_PWM_CHANNELS)
        return;
    slot->pwm[ch].freq_hz = freq_hz;
    slot->pwm[ch].duty_pct = duty_pct;
    slot->pwm[ch].enabled = enabled ? 1u : 0u;
    store_mark_dirty();
}

uint32_t store_la_trigger_edge(void)
{
    return slot->la_trigger_edge;
}

void store_la_set_trigger_edge(uint32_t edge)
{
    edge = edge ? 1u : 0u;
    if (slot->la_trigger_edge == edge)
        return;
    slot->la_trigger_edge = edge;
    store_mark_dirty();
}

uint32_t store_la_debug(void)
{
    return slot->la_debug;
}

void store_la_set_debug(uint32_t on)
{
    on = on ? 1u : 0u;
    if (slot->la_debug == on)
        return;
    slot->la_debug = on;
    store_mark_dirty();
}
