/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2021 Peter Lawrence
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

#include "FreeRTOS.h"
#include "task.h"

#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#if PICO_SDK_VERSION_MAJOR >= 2
#include "bsp/board_api.h"
#else
#include "bsp/board.h"
#endif
#include "tusb.h"

#include "probe_config.h"
#include "probe.h"
#include "cdc_uart.h"
#include "get_serial.h"
#include "tusb_edpt_handler.h"
#include "DAP.h"
#include "hardware/structs/usb.h"
#include "hardware/watchdog.h"

#include "mode_manager.h"
#include "pwm_mode.h"
#include "la_mode.h"
#include "cdc_stdio.h"

// UART0 for debugprobe debug
// UART1 for debugprobe to target device

static uint8_t TxDataBuffer[CFG_TUD_HID_EP_BUFSIZE];

#define THREADED 1

#define UART_TASK_PRIO (tskIDLE_PRIORITY + 3)
#define TUD_TASK_PRIO  (tskIDLE_PRIORITY + 4)
#define DAP_TASK_PRIO  (tskIDLE_PRIORITY + 1)

TaskHandle_t dap_taskhandle, tud_taskhandle, mon_taskhandle;

static int was_configured;

void dev_mon(void *ptr)
{
    uint32_t sof[3];
    int i = 0;
    TickType_t wake;
    wake = xTaskGetTickCount();
    do {
        /* ~5 SOF events per tick */
        xTaskDelayUntil(&wake, 100);

        if (tud_connected() && !tud_suspended()) {
            sof[i++] = usb_hw->sof_rd & USB_SOF_RD_BITS;
            i = i % 3;
        } else {
            for (i = 0; i < 3; i++)
                sof[i] = 0;
        }
        if ((sof[0] | sof[1] | sof[2]) != 0) {
            if ((sof[0] == sof[1]) && (sof[1] == sof[2])) {
                probe_info("Watchdog timeout! Resetting USBD\n");
                /* uh oh, signal disconnect (implicitly resets the controller) */
                tud_deinit(0);
                /* Make sure the port got the message */
                xTaskDelayUntil(&wake, 1);
                tud_init(0);
            }
        }
    } while (1);
}

void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
  (void) rhport;
  (void) eventid;
  BaseType_t blah;
  if (in_isr) {
    xTaskNotifyFromISR(tud_taskhandle, 0, 0, &blah);
  } else {
    xTaskNotify(tud_taskhandle, 0, 0);
  }
}

void tud_unmount_cb(void);

void usb_thread(void *ptr)
{
  uint32_t cmd;
  /* LED (GPIO25) is owned by mode_manager for mode indication */
    TickType_t wake;
    wake = xTaskGetTickCount();
    do {
        tud_task();
        // implied bus-reset detection
        if (!tud_connected() && was_configured)
            tud_unmount_cb();

        // If suspended or disconnected, unconditional delay for 1ms (20 ticks)
        if (tud_suspended() || !tud_connected())
            xTaskDelayUntil(&wake, 20);
        // Go to sleep if nothing to do
        else if (!tud_task_event_ready())
          xTaskNotifyWait(0, 0xFFFFFFFFu, &cmd, 1);

    } while (1);
}

// Workaround API change in 0.13
#if (TUSB_VERSION_MAJOR == 0) && (TUSB_VERSION_MINOR <= 12)
#define tud_vendor_flush(x) ((void)0)
#endif

int main(void) {
    // Declare pins in binary information
    bi_decl_config();

    board_init();
    /* 上电防抖：等电源/晶振稳定后再做 flash 读取、模式轮换与 USB 初始化，
     * 避免快速拔插切模式时的电源抖动导致后续初始化偶发失败（LED 不亮）。 */
    sleep_ms(100);
    usb_serial_init();

    /* 三合一：先恢复持久化设置（模式 + PWM/LA 参数），后面所有初始化再按模式走 */
    mode_init();

    /* 三功能并行：UART 桥、DAP、PWM、LA 全部初始化（引脚已错开，不再重叠）。 */
    cdc_uart_init();

    tusb_init();
    stdio_uart_init();

    DAP_Setup();

    /* PWM 无主机也要输出波形；LA 配置 PIO/GPIO。 */
    mode_boot_apply();

    probe_info("Welcome to debugprobe-combo!\n");

    if (THREADED) {
        xTaskCreate(usb_thread, "TUD", configMINIMAL_STACK_SIZE, NULL, TUD_TASK_PRIO, &tud_taskhandle);
#if (configNUMBER_OF_CORES > 1)
        vTaskCoreAffinitySet(tud_taskhandle, (1 << 0));
#endif
#if PICO_RP2040
        xTaskCreate(dev_mon, "WDOG", configMINIMAL_STACK_SIZE, NULL, TUD_TASK_PRIO, &mon_taskhandle);
#if (configNUMBER_OF_CORES > 1)
        vTaskCoreAffinitySet(mon_taskhandle, (1 << 0));
#endif
#endif
        vTaskStartScheduler();
    }

    return 0;
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
  // TODO not Implemented
  (void) itf;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) reqlen;

  return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* RxDataBuffer, uint16_t bufsize)
{
  uint32_t response_size = TU_MIN(CFG_TUD_HID_EP_BUFSIZE, bufsize);

  // This doesn't use multiple report and report ID
  (void) itf;
  (void) report_id;
  (void) report_type;

  DAP_ProcessCommand(RxDataBuffer, TxDataBuffer);

  tud_hid_report(0, TxDataBuffer, response_size);
}

#if (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V2)
extern uint8_t const desc_ms_os_20[];

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request)
{
  // nothing to with DATA & ACK stage
  if (stage != CONTROL_STAGE_SETUP) return true;

  switch (request->bmRequestType_bit.type)
  {
    case TUSB_REQ_TYPE_VENDOR:
      switch (request->bRequest)
      {
        case 1:
          if ( request->wIndex == 7 )
          {
            // Get Microsoft OS 2.0 compatible descriptor
            uint16_t total_len;
            memcpy(&total_len, desc_ms_os_20+8, 2);

            return tud_control_xfer(rhport, request, (void*) desc_ms_os_20, total_len);
          }else
          {
            return false;
          }

        default: break;
      }
    break;
    default: break;
  }

  // stall unknown request
  return false;
}
#endif

void tud_suspend_cb(bool remote_wakeup_en)
{
  probe_info("Suspended\n");
  /* Were we actually configured? If not, threads don't exist */
  if (was_configured) {
	  vTaskSuspend(uart_taskhandle);
	  vTaskSuspend(dap_taskhandle);
	  vTaskSuspend(la_taskhandle);
	  vTaskSuspend(pwm_taskhandle);
  }
  /* slow down clk_sys for power saving ? */
}

void tud_resume_cb(void)
{
  probe_info("Resumed\n");
  if (was_configured) {
    vTaskResume(uart_taskhandle);
    vTaskResume(dap_taskhandle);
    vTaskResume(la_taskhandle);
    vTaskResume(pwm_taskhandle);
  }
}

void tud_unmount_cb(void)
{
  probe_info("Disconnected/reset\n");
  if (uart_taskhandle) {
    vTaskSuspend(uart_taskhandle);
    vTaskDelete(uart_taskhandle);
    uart_taskhandle = NULL;
  }
  if (dap_taskhandle) {
    vTaskSuspend(dap_taskhandle);
    vTaskDelete(dap_taskhandle);
    dap_taskhandle = NULL;
  }
  if (la_taskhandle) {
    vTaskSuspend(la_taskhandle);
    vTaskDelete(la_taskhandle);
    la_taskhandle = NULL;
  }
  if (pwm_taskhandle) {
    vTaskSuspend(pwm_taskhandle);
    vTaskDelete(pwm_taskhandle);
    pwm_taskhandle = NULL;
  }
  was_configured = 0;
}

void tud_mount_cb(void)
{
  probe_info("Connected, Configured: %d\n", !!was_configured);
  if (!was_configured) {
    /* UART needs to preempt USB as if we don't, characters get lost */
    xTaskCreate(cdc_thread, "UART", configMINIMAL_STACK_SIZE, NULL, UART_TASK_PRIO, &uart_taskhandle);
    /* Lowest priority thread is debug - need to shuffle buffers before we can toggle swd... */
    xTaskCreate(dap_thread, "DAP", configMINIMAL_STACK_SIZE, NULL, DAP_TASK_PRIO, &dap_taskhandle);
    /* LA: CDC#1（SUMP + 流式）+ housekeeping；PWM: CDC#2（文本命令） */
    xTaskCreate(la_task, "LA", 512, NULL, UART_TASK_PRIO, &la_taskhandle);
    xTaskCreate(pwm_task, "PWM", 512, NULL, UART_TASK_PRIO, &pwm_taskhandle);
#if(configNUMBER_OF_CORES > 1)
    vTaskCoreAffinitySet(dap_taskhandle, (1 << 1));
    vTaskCoreAffinitySet(uart_taskhandle, (1 << 0));
#endif
    was_configured = 1;
  }
}

void vApplicationTickHook (void)
{
};

void vApplicationStackOverflowHook(TaskHandle_t Task, char *pcTaskName)
{
  panic("stack overflow (not the helpful kind) for %s\n", *pcTaskName);
}

void vApplicationMallocFailedHook(void)
{
  panic("Malloc Failed\n");
};
