# RP2040 三合一固件源码

DAPLink 调试器 + 双通道流式逻辑分析仪 + 双路 PWM 信号源，跑在 RP2040（Pico）上，通过一个 USB 复合设备同时提供三路功能。

网页上位机在仓库根目录：https://q508.github.io/rp2040-web-console/

## 功能

| 功能 | 说明 | 引脚 |
|---|---|---|
| CMSIS-DAP v2 调试器 | SWD 位打，pyOCD / OpenOCD / Keil 直接识别 | SWCLK GP2 / SWDIO GP3 |
| 串口桥 | CDC#0 ↔ uart1，115200 8N1 | TX GP4 / RX GP5 |
| 逻辑分析仪 | 2 通道流式采样，250 kHz ~ 2 MHz，4 采样/字节打包，32 KB DMA 环 | CH0 GP8 / CH1 GP9 |
| PWM 信号源 | 2 路独立频率/占空比，1 Hz ~ 20 MHz | CH0 GP6 / CH1 GP7 |
| 状态灯 | 空闲常亮 / 采集呼吸 / 异常快闪（PWM 调光） | GP25（板载） |

USB 枚举为 4 个接口：IF0 vendor（DAP）、CDC#0（串口桥）、CDC#1（LA）、CDC#2（PWM）。

## 编译

依赖：

- Pico SDK **2.x**（`PICO_SDK_PATH`）
- arm-none-eabi-gcc 13 以上
- CMake 3.13+、Ninja
- FreeRTOS-Kernel 已随仓库带上（`freertos/`），无需另外下载

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -G Ninja -B build -DPICO_BOARD=pico
cmake --build build -j
```

Windows PowerShell：

```powershell
$env:PICO_SDK_PATH = "E:\RP2040_DEV\pico-sdk"
cmake -G Ninja -B build -DPICO_BOARD=pico
cmake --build build -j 8
```

产物：`build/debugprobe_on_pico.uf2` —— 按住 BOOTSEL 插 USB，拖进 `RPI-RP2` 盘即可烧录。

> `get-version.sh` 用 `git describe` 取版本号；本仓库没有打 tag 时会退化成 commit 短哈希，不影响编译。

## 目录

```
src/            应用层：main / mode_manager / probe / la_mode / capture / pwm_mode / protocol_sump / led_status ...
include/        板级与产品配置（board_pico_config.h、probe_config.h）
CMSIS_DAP/      ARM CMSIS-DAP 实现（Apache-2.0）
freertos/       FreeRTOS-Kernel（MIT）
combo_ui.py     Windows 上位机（Tkinter，串口调试工具）
la_scope.py     早期 LA 打点脚本
*.spec          PyInstaller 打包配置
```

## 相对上游的改动

- 三功能从「模式轮换」改为**并行**：DAP 走 bulk 端点，LA 用 CDC#1，PWM 用 CDC#2；
- LA 改为**无限流式**：32 KB DMA 环 + staging 分块发送，主机反压不丢样本；
- 采样率上限提到 2 MHz（`capture.c` 按 `clk_sys/rate` 折算 PIO 分频）；
- `led_status.c` 三态指示替代原 DAPLink 调试灯；
- 去掉 nRESET（GP1 释放），PWM 改用自定义 PIO 程序，与 SWD/LA 分时复用 pio0。

## 许可

- 本工程代码：MIT，见 `LICENSE`
- `CMSIS_DAP/`：Apache-2.0（ARM Limited）
- `freertos/`：MIT（Amazon / FreeRTOS）
- 上游基座：Raspberry Pi debugprobe，见 `README-upstream.md`
