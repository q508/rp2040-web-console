# RP2040 三合一 Web 控制台 v1.1

一根 USB 线，把 RP2040 变成 **DAPLink 调试器 + 双通道逻辑分析仪 + 双路 PWM 信号源**，上位机是浏览器，不用装客户端。

网页版：https://q508.github.io/rp2040-web-console/

## 本次更新（相对 v1.0）

- **修复：网页「进入升级模式」会让设备卡死**。CDC#2 上还有未发完的数据时直接跳 ROM，ROM 接管失败，
  表现为"回了话却不重启、该串口之后再也写不进去"。现在跳转前会清空 CDC 发送缓冲、主动断开 USB、
  等枚举稳定、关闭 3 秒看门狗，并加了一层硬复位兜底——任何情况都不会把设备留在"没 USB 也没重启"的状态。
- **固件版本号升到 v1.1**，可用 `picotool info` 读出。
- `release.sh` 修复：改 `VERSION` 后 CMake 不会重编 `probe_config.c`，导致二进制里版本号仍是旧的，现在编译前强制 touch。

## 功能

- **三功能并行**：调试、采集、PWM 输出同时运行，不用切模式。DAP 走 vendor 端点，LA 占 CDC#1，PWM 占 CDC#2。
- **逻辑分析仪**：2 通道流式采样，250 kHz / 500 kHz / 1 MHz / 2 MHz 可选，每字节打包 4 个采样，32 KB DMA 环形缓冲。
- **长时间统计**：只累计统计量、不保存原始采样，可连续跑 8 小时以上，输出平均频率、周期抖动 σ、占空比、ppm 误差、丢样次数。
- **PWM 信号源**：2 路独立 1 Hz – 20 MHz，占空比 0.1% 步进，参数掉电保存。
- **三态指示灯**：空闲常亮 / 采集中呼吸 / 1.5 秒无数据则快闪。

## 下载

| 文件 | 说明 |
|---|---|
| `combo_v1.1_1113_led3.uf2` | 固件，189 KB，内嵌版本号 `v1.1` |
| `firmware-src/` | 完整源码（Pico SDK 2.x CMake 工程 + FreeRTOS-Kernel + CMSIS-DAP） |

烧录：网页点「进入升级模式」（v1.1 起可用），或按住 BOOTSEL 插 USB；出现 `RPI-RP2` 盘后双击 `flash_uf2.bat` 或手动拖入。

SHA-256：`82acf11dad5c2b4374c42c95ffad0f4c152320914885b43e42b92446e0d580cb`

## 接线

| 功能 | 信号 | GPIO | Pico 针号 |
|---|---|---|---|
| SWD 调试 | SWCLK / SWDIO | GP2 / GP3 | 4 / 5 |
| 串口桥 | TX / RX | GP4 / GP5 | 6 / 7 |
| LA 输入 | CH0 / CH1 | GP8 / GP9 | 11 / 12 |
| PWM 输出 | CH0 / CH1 | GP6 / GP7 | 9 / 10 |
| 指示灯 | STATUS | GP25 | 板载 |

全部 3.3 V 电平，LA 输入不耐 5 V，必须与目标板共地。

## 已知限制

- **2 MHz 采样时 USB 吞吐约 447 KB/s**（需求的 91.5%），长跑建议 ≤1 MHz；8 小时统计建议 250 kHz。
- LA 环形缓冲 32 KB，250 kHz 下约 524 ms 反压余量，主机卡顿超过这个时长会丢数据。
- 暂无触发捕获和 RLE 压缩。
- 网页只支持 Chrome / Edge 89+，必须通过 https 或 localhost 访问。
- DAPLink 只实现 SWD，不含 JTAG 和 SWO。
- `release.sh` 未入库，依赖本机固件工程路径。

## 许可

MIT，见 `LICENSE`。`CMSIS_DAP/` 为 Apache-2.0，`freertos/` 为 MIT，上游基座 Raspberry Pi debugprobe 为 MIT。
