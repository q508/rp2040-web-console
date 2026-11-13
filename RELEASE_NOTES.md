# RP2040 三合一 Web 控制台 v1.2

一根 USB 线，把 RP2040 变成 **DAPLink 调试器 + 双通道逻辑分析仪 + 双路 PWM 信号源**，上位机是浏览器，不用装客户端。

网页版：https://q508.github.io/rp2040-web-console/

## 本次修复：网页「进入升级模式」真正可用了

v1.1 的按钮还有问题——点了之后设备要么卡死、要么复位回应用模式。v1.2 换成两段式实现，实测两个串口都能干净进入 BOOTSEL：

| 入口 | 结果 |
|---|---|
| PWM `bootsel`（CDC#2） | **0.53 s** 串口消失 + RPI-RP2 出现 |
| LA `mbootsel`（CDC#1） | **0.50 s** |
| 盘写入 / 投递后恢复 | 可写，1 s 恢复三个串口 |

### 三个叠在一起的根因

1. **在 FreeRTOS 任务里直接调 `reset_usb_boot()` 会崩**。它在 RP2040 上走 `rom_reboot()`，内部要 `rom_func_lookup` 查 ROM 函数表并调用，要求环境干净。官方 `pico_bootsel_via_double_reset` 也是在 `main()` 之前的构造函数里调，而不是在任务里。
2. **`watchdog_reboot()` 不会复位**。它靠看门狗超时，而 `housekeeping()` 在另一个任务里每 1 ms 调 `watchdog_update()` 喂狗，把超时刷掉了。
3. **`tud_disconnect()` 是个坏主意**。主动断开让主机侧状态变脏，ROM 接管后枚举超时（实测 3.2 s）又退回去从 flash 启动。

### 现在的做法

- 命令侧只做两件事：写 3 个 magic 到 `__uninitialized_ram`（软复位不清 SRAM），然后写 AIRCR 的 `SYSRESETREQ` 立即复位；
- `main()` 里 `board_init()` 之后、USB 初始化之前检查标记，命中才调 `reset_usb_boot(0, 0)`——和官方库同一个时机。

## 功能

- **三功能并行**：调试、采集、PWM 输出同时运行，不用切模式。DAP 走 vendor 端点，LA 占 CDC#1，PWM 占 CDC#2。
- **逻辑分析仪**：2 通道流式采样，250 kHz / 500 kHz / 1 MHz / 2 MHz 可选，每字节打包 4 个采样，32 KB DMA 环形缓冲。
- **长时间统计**：只累计统计量、不保存原始采样，可连续跑 8 小时以上，输出平均频率、周期抖动 σ、占空比、ppm 误差、丢样次数。
- **PWM 信号源**：2 路独立 1 Hz – 20 MHz，占空比 0.1% 步进，参数掉电保存。
- **三态指示灯**：空闲常亮 / 采集中呼吸 / 1.5 秒无数据则快闪。

## 下载

| 文件 | 说明 |
|---|---|
| `combo_v1.2_1113_led3.uf2` | 固件，189 KB，内嵌版本号 `v1.2` |
| `firmware-src/` | 完整源码（Pico SDK 2.x CMake 工程 + FreeRTOS-Kernel + CMSIS-DAP） |

烧录：网页「引脚 / 接线」页点「进入升级模式」，出现 `RPI-RP2` 盘后双击 `flash_uf2.bat` 自动投递；也可按住 BOOTSEL 插 USB。

SHA-256：见下方 Release 附件。

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
