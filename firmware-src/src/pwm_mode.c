/*
 * pwm_mode.c - 2 路独立 PWM 信号发生器
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 时钟由 mode_manager 统一设置（clk_sys 固定 125MHz），本模块不再自己设置。
 * 2 路参数（频率/占空比/开关）通过 combo_store 持久化：开机自动恢复上次设置，
 * 修改后延迟落盘（见 combo_store.c 的 STORE_QUIET_MS），也可用 save 命令立即
 * 落盘。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#include "pwm_mode.h"
#include "cdc_stdio.h"
#include "combo_store.h"
#include "probe_config.h"

/* 回显重定向到 CDC#2（PWM 文本命令实例） */
#define printf(fmt, ...) cdc_printf(2, fmt, ##__VA_ARGS__)

#define SQUARE_MIN_PHASE_CYCLES 3u

/* 输出频率上限 PWM_MAX_FREQ_HZ（=clk_sys/6）定义在 pwm_mode.h */

/* 通道引脚来自板级配置，不再写死 */
#define PWM_PIN(ch) (PWM_CH_PIN_BASE + (ch))

_Static_assert(PWM_NUM_CHANNELS == PWM_CH_PIN_COUNT,
               "PWM_NUM_CHANNELS must match PWM_CH_PIN_COUNT");

/* 每路独立 HIGH/LOW 计数的方波 PIO 程序（单份加载，4 SM 共用偏移） */
static const uint16_t square_asym_instructions[] = {
    0x80a0, /* pull   block           ; HIGH -> OSR */
    0xa0c7, /* mov    isr, osr        */
    0x80a0, /* pull   block           ; LOW -> OSR */
    0xa047, /* mov    y, osr          */
    0xa026, /* mov    x, isr          ; wrap_target */
    0xe001, /* set    pins, 1         */
    0x0046, /* jmp    x--, 6          */
    0xa022, /* mov    x, y            */
    0xe000, /* set    pins, 0         */
    0x0049, /* jmp    x--, 9          ; wrap */
};

static const struct pio_program square_asym_program = {
    .instructions = square_asym_instructions,
    .length = 10,
    .origin = -1,
};

#define SQUARE_ASYM_WRAP_TARGET 4
#define SQUARE_ASYM_WRAP 9

typedef struct {
    uint32_t freq_hz;
    float duty_pct;
    bool enabled;
    uint32_t high_count;
    uint32_t low_count;
} pwm_channel_t;

static pwm_channel_t channels[PWM_NUM_CHANNELS];
static uint offset_pio0, offset_pio1;
static bool inited = false;

static void compute_counts(uint32_t freq_hz, float duty_pct,
                           uint32_t *hi, uint32_t *lo)
{
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint64_t period_cycles = ((uint64_t)sys_hz + freq_hz / 2) / freq_hz;
    /* 每个周期固定开销 2*SQUARE_MIN_PHASE_CYCLES = 6 个时钟（mov x + set + jmp
     * 各计一次，高低两段各 3），所以最高频率被限制在 clk_sys/6，见 PWM_MAX_FREQ_HZ。 */
    if (period_cycles < 2 * SQUARE_MIN_PHASE_CYCLES)
        period_cycles = 2 * SQUARE_MIN_PHASE_CYCLES;

    float duty = duty_pct / 100.0f;
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;

    int64_t budget = (int64_t)period_cycles - 2 * (int64_t)SQUARE_MIN_PHASE_CYCLES;
    int64_t high = lroundf((float)budget * duty);
    if (high < 0) high = 0;
    if (high > budget) high = budget;

    *hi = (uint32_t)high;
    *lo = (uint32_t)(budget - high);
}

static void apply_channel(int ch)
{
    pwm_channel_t *c = &channels[ch];
    PIO pio = ch < 4 ? pio0 : pio1;
    uint sm = 2 + ch;  /* SM2/SM3：避开 DAP SWD(pio0 SM0) 与 LA 流式(pio0 SM1) */
    uint offset = ch < 4 ? offset_pio0 : offset_pio1;

    compute_counts(c->freq_hz, c->duty_pct, &c->high_count, &c->low_count);

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);

    pio_sm_config cfg = pio_get_default_sm_config();
    sm_config_set_set_pins(&cfg, PWM_PIN(ch), 1);
    sm_config_set_wrap(&cfg, offset + SQUARE_ASYM_WRAP_TARGET, offset + SQUARE_ASYM_WRAP);
    sm_config_set_clkdiv(&cfg, 1.0f);
    pio_sm_init(pio, sm, offset, &cfg);

    pio_sm_put_blocking(pio, sm, c->high_count);
    pio_sm_put_blocking(pio, sm, c->low_count);

    if (c->enabled)
        pio_sm_set_enabled(pio, sm, true);
}

/* 按当前 clk_sys 重算并输出全部通道。初始化时调用；运行期若改了 clk_sys
 * 也必须补调一次，否则 high/low 计数还是按旧时钟算的，频率会偏。 */
void pwm_mode_apply_all(void)
{
    for (int ch = 0; ch < PWM_NUM_CHANNELS; ch++)
        apply_channel(ch);
}

/* 从持久化设置恢复通道参数（带范围钳位，防止坏数据/NaN） */
static void load_channels(void)
{
    for (int ch = 0; ch < PWM_NUM_CHANNELS; ch++) {
        uint32_t freq = 1000;
        float duty = 50.0f;
        bool enabled = true;

        store_pwm_get(ch, &freq, &duty, &enabled);

        if (freq < 1u) freq = 1u;
        if (freq > PWM_MAX_FREQ_HZ) freq = PWM_MAX_FREQ_HZ;
        if (!(duty >= 0.0f)) duty = 0.0f; /* 同时挡掉 NaN */
        if (duty > 100.0f) duty = 100.0f;

        channels[ch].freq_hz = freq;
        channels[ch].duty_pct = duty;
        channels[ch].enabled = enabled;
    }
}

void pwm_mode_init(void)
{
    if (inited) return;

    offset_pio0 = pio_add_program(pio0, &square_asym_program);
    offset_pio1 = pio_add_program(pio1, &square_asym_program);

    for (int ch = 0; ch < PWM_NUM_CHANNELS; ch++) {
        PIO pio = ch < 4 ? pio0 : pio1;
        uint sm = 2 + ch;  /* SM2/SM3：避开 DAP SWD(pio0 SM0) 与 LA 流式(pio0 SM1) */
        pio_gpio_init(pio, PWM_PIN(ch));
        pio_sm_set_consecutive_pindirs(pio, sm, PWM_PIN(ch), 1, true);
    }

    load_channels();       /* 恢复上次保存的参数 */
    pwm_mode_apply_all();  /* 按统一后的 clk_sys 算出 high/low 并开输出 */

    inited = true;
}

void pwm_mode_deinit(void)
{
    if (!inited) return;
    for (int ch = 0; ch < PWM_NUM_CHANNELS; ch++) {
        PIO pio = ch < 4 ? pio0 : pio1;
        uint sm = 2 + ch;  /* SM2/SM3：避开 DAP SWD(pio0 SM0) 与 LA 流式(pio0 SM1) */
        pio_sm_set_enabled(pio, sm, false);
        gpio_deinit(PWM_PIN(ch));
    }
    pio_remove_program(pio0, &square_asym_program, offset_pio0);
    pio_remove_program(pio1, &square_asym_program, offset_pio1);
    inited = false;
}

void pwm_set_freq(uint ch, uint32_t hz)
{
    if (ch >= PWM_NUM_CHANNELS) return;
    if (hz < 1) hz = 1;
    if (hz > PWM_MAX_FREQ_HZ) hz = PWM_MAX_FREQ_HZ;
    channels[ch].freq_hz = hz;
    store_pwm_set(ch, channels[ch].freq_hz, channels[ch].duty_pct, channels[ch].enabled);
    if (inited) apply_channel(ch);
}

void pwm_set_duty(uint ch, float pct)
{
    if (ch >= PWM_NUM_CHANNELS) return;
    if (!(pct >= 0.0f)) pct = 0.0f; /* 同时挡掉 NaN */
    if (pct > 100.0f) pct = 100.0f;
    channels[ch].duty_pct = pct;
    store_pwm_set(ch, channels[ch].freq_hz, channels[ch].duty_pct, channels[ch].enabled);
    if (inited) apply_channel(ch);
}

bool pwm_mode_any_enabled(void)
{
    for (int ch = 0; ch < PWM_NUM_CHANNELS; ch++)
        if (channels[ch].enabled)
            return true;
    return false;
}

void pwm_set_enable(uint ch, bool on)
{
    if (ch >= PWM_NUM_CHANNELS) return;
    channels[ch].enabled = on;
    store_pwm_set(ch, channels[ch].freq_hz, channels[ch].duty_pct, channels[ch].enabled);
    if (inited) apply_channel(ch);
}

static void print_status(void)
{
    printf("Ch | GPIO | Freq(Hz)   | Duty%% | State\n");
    for (int ch = 0; ch < PWM_NUM_CHANNELS; ch++)
        printf(" %d |  %2d  | %9u | %5.1f | %s\n",
               ch, PWM_PIN(ch), channels[ch].freq_hz,
               channels[ch].duty_pct, channels[ch].enabled ? "ON" : "OFF");
    printf("settings: saved to flash (auto after ~1.5s idle, or 'save')\n");
    printf("end\n");   /* 上位机解析状态表的结束标记，勿删 */
}

static void print_help(void)
{
    printf("PWM commands:\n");
    printf("  f<ch>:<freq>  set channel frequency Hz (ch 0-1, 1-%u)\n",
           (unsigned)PWM_MAX_FREQ_HZ);
    printf("  d<ch>:<duty>  set channel duty %% (ch 0-1, 0-100, 0.1 step)\n");
    printf("  on<ch> / off<ch>  enable / disable channel\n");
    printf("  i             show all channels\n");
    printf("  save          write settings to flash now (auto after ~1.5s idle)\n");
    printf("  help          this list\n");
    printf("  mode pwm|la|daplink   switch mode\n");
}

bool pwm_handle_line(const char *line, int len)
{
    (void) len;
    if (strcmp(line, "i") == 0) { print_status(); return true; }
    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) { print_help(); return true; }

    char c0 = line[0];

    if (c0 == 'f' || c0 == 'd') {
        int ch = line[1] - '0';
        if (ch < 0 || ch >= PWM_NUM_CHANNELS) { printf("bad channel (0-1)\n"); return true; }
        const char *colon = strchr(line, ':');
        if (!colon) { printf("format: %c<ch>:<value>\n", c0); return true; }
        double val = atof(colon + 1);
        if (c0 == 'f') {
            if (val >= 1.0 && val <= (double)PWM_MAX_FREQ_HZ) { pwm_set_freq(ch, (uint32_t)val); printf("ch%d freq = %u Hz\n", ch, channels[ch].freq_hz); }
            else printf("bad freq (1-%u Hz)\n", (unsigned)PWM_MAX_FREQ_HZ);
        } else {
            if (val >= 0.0 && val <= 100.0) { pwm_set_duty(ch, (float)val); printf("ch%d duty = %.1f%%\n", ch, channels[ch].duty_pct); }
            else printf("bad duty (0-100%%)\n");
        }
        return true;
    }

    if (strncmp(line, "on", 2) == 0) {
        int ch = line[2] - '0';
        if (ch >= 0 && ch < PWM_NUM_CHANNELS) { pwm_set_enable(ch, true); printf("ch%d ON\n", ch); }
        return true;
    }
    if (strncmp(line, "off", 3) == 0) {
        int ch = line[3] - '0';
        if (ch >= 0 && ch < PWM_NUM_CHANNELS) { pwm_set_enable(ch, false); printf("ch%d OFF\n", ch); }
        return true;
    }

    return false;
}
