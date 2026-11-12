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

#include "protocol_sump.h"

#include "capture.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

/*
 * combo 底座把 stdio 接到了 UART0（debugprobe 调试口），SUMP 协议字节流
 * 必须走 USB CDC。这里把 protocol_sump.c 的字节级收发重定向到 cdc_stdio。
 * 注意：debug_block/debug 走 common.c（UART0），不受此影响。
 */
#include "cdc_stdio.h"
/* LA 固定用 CDC instance 1（CDC#1） */
#define getchar_timeout_us(us) cdc_getchar_timeout(1, (us))
#define putchar(c) cdc_putchar(1, (c))
#define printf(fmt, ...) cdc_printf(1, fmt, ##__VA_ARGS__)

// Sump metadata
#define DEVICE_NAME "RP2040"
#define DEVICE_VERSION "v0.1"
#define MAX_TOTAL_SAMPLES 65536    // 元数据（流式模式不限采样数）
/* 流式 2 通道：每字节 4 个采样（bit0=CH0、bit1=CH1，LSB 先），无限流式，
 * 无触发、无 RLE。CLOCK_RATE 必须是 100MHz（libsigrok ols 驱动写死的协议基准，
 * 分频比按它算），固件再折算成 PIO 分频 clk_sys/rate，实际采样率恒等于 rate。
 * MAX_SAMPLE_RATE 报实际采样率上限 1MHz。 */
#define MAX_SAMPLE_RATE 1000000    // Hz
#define CLOCK_RATE 100000000       // Hz
#define PROTOCOL_VERSION 2

// Number of stages
#define STAGES_COUNT 4

// Trigger Config
#define TRIGGER_START (1 << (3 + 24))
#define TRIGGER_SERIAL (1 << (2 + 24))
#define TRIGGER_CHANNEL_MASK (31 << (4 + 16))
#define TRIGGER_CHANNEL(NUMBER) (NUMBER << (4 + 16))
#define TRIGGER_LEVEL_MASK (3 << (0 + 24))
#define TRIGGER_LEVEL(NUMBER) (NUMBER << (0 + 24))

typedef enum sump_flag_bits_t {
    FLAG_DEMUX_MODE = (1 << 0),
    FLAG_NOISE_FILTER = (1 << 1),
    FLAG_DISABLE_CHANGROUP_1 = (1 << 2),
    FLAG_DISABLE_CHANGROUP_2 = (1 << 3),
    FLAG_DISABLE_CHANGROUP_3 = (1 << 4),
    FLAG_DISABLE_CHANGROUP_4 = (1 << 5),
    FLAG_CLOCK_EXTERNAL = (1 << 6),
    FLAG_INVERT_EXT_CLOCK = (1 << 7),
    FLAG_RLE = (1 << 8),
    FLAG_SWAP_CHANNELS = (1 << 9),
    FLAG_EXTERNAL_TEST_MODE = (1 << 10),
    FLAG_INTERNAL_TEST_MODE = (1 << 11),
    FLAG_RESERVED_0 = (1 << 12),
    FLAG_RESERVED_1 = (1 << 13),
    FLAG_RLE_MODE_0 = (1 << 14),
    FLAG_RLE_MODE_1 = (1 << 15)
} sump_flag_bits_t;

typedef struct sump_trigger_t {
    uint mask;
    uint values;
    uint configuration;
} sump_trigger_t;

static uint divisor_, flags_;
static sump_trigger_t sump_trigger_[STAGES_COUNT];

static inline void prepare_adquisition(void);
static inline uint32_t get_uint32(void);
static inline void put_uint32(uint32_t value);

uint sump_dispatch(int c) {
    switch (c) {
            case 0x00:  // reset
                debug_block("\nReset (0x%X)", c);
                return COMMAND_RESET;
                break;
            case 0x01:  // run
                debug_block("\nRun (0x%X)...", c);
                prepare_adquisition();
                return COMMAND_CAPTURE;
                break;
            case 0x02:  // send id
                printf("1ALS");
                debug_block("\nSend ID (0x%X)", c);
                break;
            case 0x04:  // send metadata
                // device name
                putchar(0x01);
                printf("%s", DEVICE_NAME);
                putchar(0x00);
                // firmware version
                putchar(0x02);
                printf("%s", DEVICE_VERSION);
                putchar(0x00);
                // sample memory
                putchar(0x21);
                put_uint32(MAX_TOTAL_SAMPLES);
                // sample rate
                putchar(0x23);
                put_uint32(MAX_SAMPLE_RATE);
                // number of channels
                putchar(0x40);
                putchar(capture_config_.channels);
                // protocol version
                putchar(0x41);
                putchar(PROTOCOL_VERSION);
                // eof
                putchar(0x00);
                debug_block(
                    "\nSend metadata (0x%X):"
                    "\n-Name: %s"
                    "\n-Version: %s"
                    "\n-Max samples: %u"
                    "\n-Max rate: %u"
                    "\n-Probes: %u"
                    "\n-Protocol: %u",
                    c, DEVICE_NAME, DEVICE_VERSION, MAX_TOTAL_SAMPLES, MAX_SAMPLE_RATE, capture_config_.channels,
                    PROTOCOL_VERSION);
                break;
            // stage 0
            case 0xC0:  // trigger mask stage 0
                sump_trigger_[0].mask = get_uint32();
                debug_block("\nRead trigger stage 0 mask (0x%X): %u", c, sump_trigger_[0].mask);
                break;
            case 0xC1:  // trigger values stage 0
                sump_trigger_[0].values = get_uint32();
                debug_block("\nRead trigger stage 0 values (0x%X): 0x%X", c, sump_trigger_[0].values);
                break;
            case 0xC2:  // trigger configuration stage 0
                sump_trigger_[0].configuration = get_uint32();
                debug_block("\nRead trigger stage 0 configuration (0x%X): 0x%X", c, sump_trigger_[0].configuration);
                break;
            // stage 1
            case 0xC4:  // trigger mask stage 1
                sump_trigger_[1].mask = get_uint32();
                debug_block("\nRead trigger stage 1 mask (0x%X): %u", c, sump_trigger_[1].mask);
                break;
            case 0xC5:  // trigger values stage 1
                sump_trigger_[1].values = get_uint32();
                debug_block("\nRead trigger stage 1 values (0x%X): 0x%X", c, sump_trigger_[1].values);
                break;
            case 0xC6:  // trigger configuration stage 1
                sump_trigger_[1].configuration = get_uint32();
                debug_block("\nRead trigger stage 1 configuration (0x%X): 0x%X", c, sump_trigger_[1].configuration);
                break;
            // stage 2
            case 0xC8:  // trigger mask stage 2
                sump_trigger_[2].mask = get_uint32();
                debug_block("\nRead trigger stage 2 mask (0x%X): %u", c, sump_trigger_[2].mask);
                break;
            case 0xC9:  // trigger values stage 2
                sump_trigger_[2].values = get_uint32();
                debug_block("\nRead trigger stage 2 values (0x%X): 0x%X", c, sump_trigger_[2].values);
                break;
            case 0xCA:  // trigger configuration stage 2
                sump_trigger_[2].configuration = get_uint32();
                debug_block("\nRead trigger stage 2 configuration (0x%X): 0x%X", c, sump_trigger_[2].configuration);
                break;
            // stage 3
            case 0xCC:  // trigger mask stage 3
                sump_trigger_[3].mask = get_uint32();
                debug_block("\nRead trigger stage 3 mask (0x%X): %u", c, sump_trigger_[3].mask);
                break;
            case 0xCD:  // trigger values stage 3
                sump_trigger_[3].values = get_uint32();
                debug_block("\nRead trigger stage 3 values (0x%X): 0x%X", c, sump_trigger_[3].values);
                break;
            case 0xCE:  // trigger configuration stage 3
                sump_trigger_[3].configuration = get_uint32();
                debug_block("\nRead trigger stage 3 configuration (0x%X): 0x%X", c, sump_trigger_[3].configuration);
                break;
            case 0x80:  // divisor
                divisor_ = get_uint32();
                debug_block("\nRead divisor (0x%X): %u", c, divisor_);
                break;
            case 0x81:  // sample size & pre trigger size
            {
                uint32_t value = get_uint32();
                capture_config_.total_samples = ((uint16_t)value * 4 + 4);
                capture_config_.pre_trigger_samples = capture_config_.total_samples - (((value >> 16) * 4) + 4);
                debug_block("\nRead samples (0x%X): %u", c, capture_config_.total_samples);
                debug_block("\nRead pre trigger samples (0x%X): %u", c, capture_config_.pre_trigger_samples);
                break;
            }
            case 0x82:  // flags. samplerate <= clock rate: demux off. samplerate > clock rate: demux on
                flags_ = get_uint32();
                {
                    /* 主机若请求 >CLOCK_RATE 的采样率，分频比会回绕成 0xFFFFFFFF，
                     * +1 即 0，直接除会抛 HardFault。这里兜住并保证 rate >= 1。 */
                    uint32_t div = divisor_ + 1;
                    if (div == 0)
                        div = 0xFFFFFFFFu;
                    capture_config_.rate =
                        ((flags_ & FLAG_DEMUX_MODE) ? 2u * CLOCK_RATE : CLOCK_RATE) / div;
                    if (capture_config_.rate < 1)
                        capture_config_.rate = 1;
                }
                debug_block(
                    "\nRead flags (0x%X): 0x%X"
                    "\n-Demux: %s -> Rate: %u"
                    "\n-RLE: %s"
                    "\n-Channel group 1: %s"
                    "\n-Channel group 2: %s"
                    "\n-Channel group 3: %s"
                    "\n-Channel group 4: %s",
                    c, flags_, flags_ & FLAG_DEMUX_MODE ? "enabled" : "disabled", capture_config_.rate,
                    flags_ & FLAG_RLE ? "enabled" : "disabled",
                    flags_ & FLAG_DISABLE_CHANGROUP_1 ? "disabled" : "enabled",
                    flags_ & FLAG_DISABLE_CHANGROUP_2 ? "disabled" : "enabled",
                    flags_ & FLAG_DISABLE_CHANGROUP_3 ? "disabled" : "enabled",
                    flags_ & FLAG_DISABLE_CHANGROUP_4 ? "disabled" : "enabled");
                break;
            case 0x83:  // sample size
                capture_config_.total_samples = get_uint32();
                debug_block("\nRead samples (0x%X): %u", c, capture_config_.total_samples);
                break;
            case 0x84:  // pre trigger size
            {
                uint32_t value = get_uint32();
                capture_config_.pre_trigger_samples = capture_config_.total_samples - ((uint16_t)value * 4 + 4);
                debug_block("\nRead pre trigger samples (0x%X): %u", c, capture_config_.pre_trigger_samples);
                break;
            }
            default:
                debug_block("\nUnknown command: 0x%X", c);
                break;
        }
    return COMMAND_NONE;
}

uint sump_read(void) {
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT)
        return COMMAND_NONE;
    return sump_dispatch(c);
}

/* 流式（2026-09-21）：数据发送由 la_mode_poll() 从 capture.c 的 DMA 环 drain 到
 * CDC，这里不再有"缓冲后整批发"的发送状态机。 */

void sump_reset(void) {
    for (uint i = 0; i < STAGES_COUNT; i++) {
        sump_trigger_[i].mask = 0;
        sump_trigger_[i].values = 0;
        sump_trigger_[i].configuration = 0;
    }
}

static inline void prepare_adquisition(void) {
    /*
     * All stages must be level 0 (immediate) and armed
     * Level triggers (1 stage for all level triggers): parallel. Note: also serial size 1, which uses 1 stage for each
     * Edge triggers (serial, size 2)
     */

    for (uint i = 0; i < TRIGGERS_COUNT; i++) {
        capture_config_.trigger[i].is_enabled = false;
    }

    uint trigger_count = 0;
    for (uint stage = 0; stage < STAGES_COUNT; stage++) {
        debug_block("\nStage: %u Mask: 0x%00000000X Values: 0x%00000000X Configuration: 0x%00000000X", stage,
                    sump_trigger_[stage].mask, sump_trigger_[stage].values, sump_trigger_[stage].configuration);

        if (sump_trigger_[stage].mask &&
            ((sump_trigger_[stage].configuration & TRIGGER_START) &&
             ((sump_trigger_[stage].configuration & TRIGGER_LEVEL_MASK) == 0))) {
            // level triggers (parallel trigger)
            if (!(sump_trigger_[stage].configuration & TRIGGER_SERIAL)) {
                for (uint channel = 0; channel < config_.channels; channel++) {
                    if (((sump_trigger_[stage].mask >> channel) & 1) != 0) {
                        capture_config_.trigger[trigger_count].is_enabled = true;
                        capture_config_.trigger[trigger_count].pin = channel;

                        if (!config_.trigger_edge) {
                            if (((sump_trigger_[stage].values >> channel) & 1) == 1)
                                capture_config_.trigger[trigger_count].match = TRIGGER_TYPE_LEVEL_HIGH;
                            else
                                capture_config_.trigger[trigger_count].match = TRIGGER_TYPE_LEVEL_LOW;
                        } else {
                            if (((sump_trigger_[stage].values >> channel) & 1) == 1)
                                capture_config_.trigger[trigger_count].match = TRIGGER_TYPE_EDGE_HIGH;
                            else
                                capture_config_.trigger[trigger_count].match = TRIGGER_TYPE_EDGE_LOW;
                        }

                        trigger_count++;
                        if (trigger_count > 3) {
                            debug("\nTrigger ignored. Reached maximum number of triggers (%u)", TRIGGERS_COUNT);
                            return;
                        }
                    }
                }
            }
            // edge triggers (serial, mask 0b11)
            else if ((sump_trigger_[stage].configuration & TRIGGER_SERIAL) && (sump_trigger_[stage].mask == 0b11)) {
                capture_config_.trigger[trigger_count].is_enabled = true;
                capture_config_.trigger[trigger_count].pin =
                    (sump_trigger_[stage].configuration & TRIGGER_CHANNEL_MASK) >> 20;

                if (((sump_trigger_[stage].values & 1) == 0) && (((sump_trigger_[stage].values >> 1) & 1) == 1)) {
                    capture_config_.trigger[trigger_count].match = TRIGGER_TYPE_EDGE_HIGH;
                    trigger_count++;
                } else if (((sump_trigger_[stage].values & 1) == 1) &&
                           (((sump_trigger_[stage].values >> 1) & 1) == 0)) {
                    capture_config_.trigger[trigger_count].match = TRIGGER_TYPE_EDGE_LOW;
                    trigger_count++;
                } else {
                    capture_config_.trigger[trigger_count].is_enabled = false;
                }
            } else if ((sump_trigger_[stage].configuration & TRIGGER_SERIAL) && (sump_trigger_[stage].mask == 0b1)) {
                capture_config_.trigger[trigger_count].is_enabled = true;
                capture_config_.trigger[trigger_count].pin =
                    (sump_trigger_[stage].configuration & TRIGGER_CHANNEL_MASK) >> 20;

                if ((sump_trigger_[stage].values & 1) == 1) {
                    capture_config_.trigger[trigger_count].match = TRIGGER_TYPE_LEVEL_HIGH;
                    trigger_count++;
                } else {
                    capture_config_.trigger[trigger_count].match = TRIGGER_TYPE_LEVEL_LOW;
                    trigger_count++;
                }
            }

            if (trigger_count > TRIGGERS_COUNT - 1) {
                debug("\nTrigger ignored. Reached maximum number of triggers (%u)", TRIGGERS_COUNT);
                return;
            }
        }
    }

    if (trigger_count < TRIGGERS_COUNT) {
        capture_config_.trigger[trigger_count].is_enabled = false;
    }
}

static inline uint32_t get_uint32(void) {
    uint32_t value = getchar_timeout_us(1000);
    value |= getchar_timeout_us(1000) << 8;
    value |= getchar_timeout_us(1000) << 16;
    value |= getchar_timeout_us(1000) << 24;
    return value;
}

static inline void put_uint32(uint32_t value) {
    // SUMP metadata 32-bit values are big-endian (libsigrok reads with RB32).
    putchar(value >> 24);
    putchar(value >> 16);
    putchar(value >> 8);
    putchar(value);
}