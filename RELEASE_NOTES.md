# RP2040 三合一 Web 控制台 v1.0 · 首个公开版本

一根 USB 线，把 RP2040 变成 **DAPLink 调试器 + 双通道逻辑分析仪 + 双路 PWM 信号源**，上位机是浏览器，不用装客户端。

网页版：https://q508.github.io/rp2040-web-console/

## 这次有什么

- **三功能并行**：调试、采集、PWM 输出同时运行，不用切模式。DAP 走 vendor 端点，LA 占 CDC#1，PWM 占 CDC#2，网页里分别连接两个串口即可同时操作。
- **逻辑分析仪**：2 通道流式采样，250 kHz / 500 kHz / 1 MHz / 2 MHz 可选，每字节打包 4 个采样，32 KB DMA 环形缓冲吸收主机反压，主机读得慢也不丢样本。
- **长时间统计**：只累计统计量、不保存原始采样，可连续跑 8 小时以上。输出平均频率、周期抖动 σ、占空比、ppm 误差、中断丢样次数，每秒钟一行 CSV，用来验证晶振和定时精度。
- **PWM 信号源**：2 路独立 1 Hz – 20 MHz，占空比 0.1% 步进，参数掉电保存，上电自动恢复。
- **三态指示灯**：空闲常亮 / 采集中呼吸 / 1.5 秒无数据则快闪。

## 下载

| 文件 | 说明 |
|---|---|
| `combo_v1.0_1112_led3.uf2` | 固件，192 KB，内嵌版本号 `v1.0`，可用 `picotool info` 读出 |
| `firmware-src/` | 完整源码（Pico SDK 2.x CMake 工程 + FreeRTOS-Kernel + CMSIS-DAP） |

烧录：按住 BOOTSEL 插 USB，把 `.uf2` 拖进出现的 `RPI-RP2` 盘，板子自动重启。

SHA-256：`b5f6bd577e820ee83f703136b253c8eb417d83788990214f74cf3254c8e26f83`

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
- LA 环形缓冲 32 KB，250 kHz 下约 524 ms 反压余量，主机卡顿超过这个时长会丢数据（统计面板的「丢样」会记录）。
- 暂无触发捕获和 RLE 压缩。
- 网页只支持 Chrome / Edge 89+，Firefox 和 Safari 不支持 WebSerial。
- 必须通过 https 或 localhost 访问；纯 http + IP 时浏览器会禁用串口。
- DAPLink 只实现 SWD，不含 JTAG 和 SWO。
- `release.sh` 未入库，它依赖本机固件工程路径。

## 许可

MIT，见 `LICENSE`。`CMSIS_DAP/` 为 Apache-2.0，`freertos/` 为 MIT，上游基座 Raspberry Pi debugprobe 为 MIT。
