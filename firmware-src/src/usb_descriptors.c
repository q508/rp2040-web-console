/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2021 Peter Lawrence
 * Copyright (c) 2022 Raspberry Pi Ltd
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

#include "tusb.h"
#include "get_serial.h"
#include "probe_config.h"
#include "pico/usb_reset.h"
#include "mode_manager.h"

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+

/* Product IDs per mode:
 *   DAPLink: 0x000c (CMSIS-DAP Debug Probe)  -- composite: DAP Bulk + CDC + Reset
 *   PWM/LA : 0x000c kept too, so Windows keeps the SAME CDC instance
 *   (VID+PID+MI+serial) and does NOT re-assign a new COM port number.
 *   The only difference is the DAP Bulk interface (IF0) turns into a
 *   zero-endpoint placeholder in PWM/LA mode. */
#define PID_DEVICE 0x000c

tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
#if (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V2)
    .bcdUSB             = 0x0210, // USB Specification version 2.1 for BOS
#else
    .bcdUSB             = 0x0110,
#endif
    .bDeviceClass       = 0x00, // Each interface specifies its own
    .bDeviceSubClass    = 0x00, // Each interface specifies its own
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0x2E8A, // Pi
    .idProduct          = PID_DEVICE,
    .bcdDevice          = 0x0231, // Version 02.31
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const * tud_descriptor_device_cb(void)
{
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

/* 三功能同时在线（2026-09-21 改造）：一个复合设备常驻全部接口，不再按模式切换描述符。
 *   IF0     CMSIS-DAP v2 (vendor bulk)  -> DAPLink 调试
 *   IF1/IF2 CDC#0 (UART 透明桥)          -> 目标串口
 *   IF3/IF4 CDC#1 (SUMP 二进制)          -> PulseView (LA)
 *   IF5/IF6 CDC#2 (文本命令)             -> combo_ui (PWM)
 *   IF7     Reset (RPI vendor)
 * 三种功能各自用独立 CDC，互不干扰；VID/PID 不变，Windows 会分到 3 个 COM 口。 */
enum
{
  ITF_NUM_DAP,       // 0 (Keil MDK 旧版只看 interface 0)
  ITF_NUM_UART_COM,  // 1  CDC#0 控制
  ITF_NUM_UART_DATA, // 2  CDC#0 数据
  ITF_NUM_LA_COM,    // 3  CDC#1 控制
  ITF_NUM_LA_DATA,   // 4  CDC#1 数据
  ITF_NUM_PWM_COM,   // 5  CDC#2 控制
  ITF_NUM_PWM_DATA,  // 6  CDC#2 数据
  ITF_NUM_RESET,     // 7
  ITF_NUM_TOTAL
};

/* 端点分配（IN 带 0x80 位）：
 *   CDC#0(UART): 0x81 通知 / 0x02 出 / 0x83 入
 *   DAP        : 0x04 出 / 0x85 入
 *   CDC#1(LA)  : 0x86 通知 / 0x05 出 / 0x87 入
 *   CDC#2(PWM) : 0x88 通知 / 0x06 出 / 0x89 入 */
#define UART_NOTIF_EP 0x81
#define UART_OUT_EP   0x02
#define UART_IN_EP    0x83
#define DAP_OUT_EP    0x04
#define DAP_IN_EP     0x85
#define LA_NOTIF_EP   0x86
#define LA_OUT_EP     0x05
#define LA_IN_EP      0x87
#define PWM_NOTIF_EP  0x88
#define PWM_OUT_EP    0x06
#define PWM_IN_EP     0x89

#if (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V1)
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + 3*TUD_CDC_DESC_LEN + TUD_RPI_RESET_DESC_LEN)
#else
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + 3*TUD_CDC_DESC_LEN + TUD_RPI_RESET_DESC_LEN)
#endif

static uint8_t const desc_hid_report[] =
{
  TUD_HID_REPORT_DESC_GENERIC_INOUT(CFG_TUD_HID_EP_BUFSIZE)
};

uint8_t const * tud_hid_descriptor_report_cb(uint8_t itf)
{
  (void) itf;
  return desc_hid_report;
}

uint8_t desc_configuration[] =
{
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
  // Interface 0
#if (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V1)
  // HID (named interface)
  TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_DAP, 4, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), DAP_OUT_EP, DAP_IN_EP, CFG_TUD_HID_EP_BUFSIZE, 1),
#elif (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V2)
  // Bulk (named interface)
  TUD_VENDOR_DESCRIPTOR(ITF_NUM_DAP, 5, DAP_OUT_EP, DAP_IN_EP, 64),
#elif (PROBE_DEBUG_PROTOCOL == PROTO_OPENOCD_CUSTOM)
  // Bulk
  TUD_VENDOR_DESCRIPTOR(ITF_NUM_DAP, 0, DAP_OUT_EP, DAP_IN_EP, 64),
#endif
  // CDC#0: UART 透明桥 (IF1/IF2)
  TUD_CDC_DESCRIPTOR(ITF_NUM_UART_COM, 6, UART_NOTIF_EP, 64, UART_OUT_EP, UART_IN_EP, 64),
  // CDC#1: LA/SUMP (IF3/IF4)
  TUD_CDC_DESCRIPTOR(ITF_NUM_LA_COM, 8, LA_NOTIF_EP, 64, LA_OUT_EP, LA_IN_EP, 64),
  // CDC#2: PWM 文本 (IF5/IF6)
  TUD_CDC_DESCRIPTOR(ITF_NUM_PWM_COM, 9, PWM_NOTIF_EP, 64, PWM_OUT_EP, PWM_IN_EP, 64),
  // Reset interface
  TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, 7),
};

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index;
  /* 给 CDC#0（UART 桥）打上 CAP_BREAK 支持：主机才能发 SEND_BREAK 控制请求。
   * 偏移 = TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + (8+9+5+5+4-1)，
   * 指向 CDC#0 的 ACM 功能描述符里 bmCapabilities 字节（0x2 -> 0x6）。
   * 本工程固定 PROTO_DAP_V2，所以用 TUD_VENDOR_DESC_LEN 作为 DAP 段长度。 */
  desc_configuration[TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + 8 + 9 + 5 + 5 + 4 - 1] = 0x6;
  return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

// array of pointer to string descriptors
char const* string_desc_arr [] =
{
  (const char[]) { 0x09, 0x04 }, // 0: is supported language is English (0x0409)
  "Raspberry Pi", // 1: Manufacturer
  "RP2040 Combo (DAP + LA + PWM)", // 2: Product
  usb_serial,     // 3: Serial, uses flash unique ID
  "CMSIS-DAP v1 Interface", // 4: Interface descriptor for HID transport
  "CMSIS-DAP v2 Interface", // 5: Interface descriptor for Bulk transport
  "CDC UART Interface",     // 6: CDC#0 (UART bridge)
  "Reset",                  // 7: Reset
  "CDC Logic Analyzer",     // 8: CDC#1 (LA/SUMP)
  "CDC PWM",                // 9: CDC#2 (PWM)
};

static uint16_t _desc_str[32];

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void) langid;

  uint8_t chr_count;

  if ( index == 0)
  {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  }else
  {
    // Convert ASCII string into UTF-16

    if ( !(index < sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) ) return NULL;

    const char* str = string_desc_arr[index];

    // Cap at max char
    chr_count = strlen(str);
    if ( chr_count > 31 ) chr_count = 31;

    for(uint8_t i=0; i<chr_count; i++)
    {
      _desc_str[1+i] = str[i];
    }
  }

  // first byte is length (including header), second byte is string type
  _desc_str[0] = (TUSB_DESC_STRING << 8 ) | (2*chr_count + 2);

  return _desc_str;
}

/* [incoherent gibbering to make Windows happy] */

//--------------------------------------------------------------------+
// BOS Descriptor
//--------------------------------------------------------------------+

/* Microsoft OS 2.0 registry property descriptor
Per MS requirements https://msdn.microsoft.com/en-us/library/windows/hardware/hh450799(v=vs.85).aspx
device should create DeviceInterfaceGUIDs. It can be done by driver and
in case of real PnP solution device should expose MS "Microsoft OS 2.0
registry property descriptor". Such descriptor can insert any record
into Windows registry per device/configuration/interface. In our case it
will insert "DeviceInterfaceGUIDs" multistring property.


https://developers.google.com/web/fundamentals/native-hardware/build-for-webusb/
(Section Microsoft OS compatibility descriptors)
*/
#define MS_OS_FEATURE_DESC_LEN 0xA0
#define MS_OS_20_DESC_LEN  (0x0A + 0x08 + MS_OS_FEATURE_DESC_LEN + RPI_RESET_MS_OS_20_DESC_LEN)

#define BOS_TOTAL_LEN      (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

uint8_t const desc_bos[] =
{
  // total length, number of device caps
  TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),

  // Microsoft OS 2.0 descriptor
  TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, 1)
};

uint8_t const desc_ms_os_20[] =
{
  // Set header: length, type, windows version, total length
  U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

  // Configuration subset header: length, type, configuration index, reserved, configuration total length
  U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN-0x0A),

  // Function Subset header: length, type, first interface, reserved, subset length
  U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_NUM_DAP, 0, U16_TO_U8S_LE(MS_OS_FEATURE_DESC_LEN),

  // MS OS 2.0 Compatible ID descriptor: length, type, compatible ID, sub compatible ID
  U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sub-compatible

  // MS OS 2.0 Registry property descriptor: length, type
  U16_TO_U8S_LE(MS_OS_FEATURE_DESC_LEN-0x08-0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
  U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType, wPropertyNameLength and PropertyName "DeviceInterfaceGUIDs\0" in UTF-16
  'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
  'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
  U16_TO_U8S_LE(0x0050), // wPropertyDataLength
  // bPropertyData "{CDB3B5AD-293B-4663-AA36-1AAE46463776}" as a UTF-16 string (b doesn't mean bytes)
  '{', 0x00, 'C', 0x00, 'D', 0x00, 'B', 0x00, '3', 0x00, 'B', 0x00, '5', 0x00, 'A', 0x00, 'D', 0x00, '-', 0x00,
  '2', 0x00, '9', 0x00, '3', 0x00, 'B', 0x00, '-', 0x00, '4', 0x00, '6', 0x00, '6', 0x00, '3', 0x00, '-', 0x00,
  'A', 0x00, 'A', 0x00, '3', 0x00, '6', 0x00, '-', 0x00, '1', 0x00, 'A', 0x00, 'A', 0x00, 'E', 0x00, '4', 0x00,
  '6', 0x00, '4', 0x00, '6', 0x00, '3', 0x00, '7', 0x00, '7', 0x00, '6', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,

  RPI_RESET_MS_OS_20_DESCRIPTOR(ITF_NUM_RESET)
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

uint8_t const * tud_descriptor_bos_cb(void)
{
  return desc_bos;
}
