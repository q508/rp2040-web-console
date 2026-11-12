# RP2040 三合一 combo 固件（DAPLink + PWM + LA 并行）

一块 RP2040 板子上**同时**跑三种仪器，引脚不再重叠、不再分时复用。基于 Raspberry Pi
官方 `debugprobe`（FreeRTOS + TinyUSB + CMSIS-DAP），合并了
[logic_analyzer_rp2040](https://github.com/dgatf/logic_analyzer_rp2040) 的 SUMP
逻辑分析仪和自研的 PWM 发生器。

## 功能与 USB 接口

一个复合 USB 设备常驻全部接口（`CFG_TUD_CDC=3`），三种功能**同时在线、互不干扰**：

| 接口 | 功能 | 上位机 |
|------|------|--------|
| IF0 CMSIS-DAP v2 | DAPLink 调试探针（**仅 SWD**，无 JTAG） | Keil / pyOCD / OpenOCD |
| CDC#0 | 目标 UART 透明桥（115200 8N1） | 任意串口工具 |
| CDC#1 | LA：2 通道流式逻辑分析仪（SUMP 握手） | PulseView / sigrok-cli / 脚本 |
| CDC#2 | PWM：2 路文本命令 | `combo_ui.py` / 任意串口工具 |
| Reset | RPI vendor 复位 | pyOCD 等 |

设备枚举出来是 **3 个 COM 口 + 1 个 CMSIS-DAP**。

## 引脚

| 功能 | 引脚 | 说明 |
|------|------|------|
| DAPLink SWD | SWCLK=GPIO2 / SWDIO=GPIO3 | 无 nRESET |
| DAPLink UART | TX=GPIO4 / RX=GPIO5 | 目标串口桥，115200 8N1 |
| PWM 2 路 | CH0=GPIO6 / CH1=GPIO7 | 独立频率/占空比 |
| LA 2 路 | CH0=GPIO8 / CH1=GPIO9 | 流式采样 |
| 状态 LED | GPIO25 | 高电平点亮，慢闪(300ms)=系统运行中 |

## PIO / DMA 资源分配

pio0 四个状态机恰好填满（三功能并行、无冲突）：

| 资源 | 占用者 |
|------|--------|
| pio0 SM0 | DAPLink SWD 位打 |
| pio0 SM1 | LA 流式采样 |
| pio0 SM2 / SM3 | PWM CH0 / CH1 |
| DMA ch0 / ch1 | LA 采样 / 环 reload |

autobaud（UART 自动波特率检测）已禁用：并行下 pio0 SM 已满，且它会动态抢 SM+DMA。

## 设置记忆

PWM 2 路参数（频率/占空比/开关）保存在 flash 最后一个 4KB 扇区（magic + 版本 + CRC32）。
修改后**安静约 1.5 秒自动落盘**，`save` 命令立即落盘。记录损坏/首次上电回落到默认值。

## PWM（连 CDC#2）

2 路独立方波，CH0-1 = GPIO6-7。频率 **1Hz-20MHz**（上限 = clk_sys/6），占空比 0-100%
（0.1 步进）。文本命令（以换行结尾）：

```
f<ch>:<freq>  设置频率  例 f0:1000
d<ch>:<duty>  设置占空比 例 d0:30
on<ch> / off<ch>  开/关
i             读全部 2 路状态
save          立即写 flash
```

**PWM 输出不依赖 USB 主机**：上电即按保存的参数输出，可只用充电头供电当信号源。
上位机 `combo_ui.py` 会自动探测 CDC#2 并连接。

## LA（连 CDC#1）

**2 通道流式 1MHz，无触发、无 RLE**。SUMP 握手（ID/元数据/run/reset）保留：
发 `run`(0x01) 开始持续流式输出，发 `reset`(0x00) 停止。

数据格式：2 通道打包，**每字节 4 个采样**，采样 N 在 `bit[2N : 2N+1]`（LSB 先），
`bit0 = CH0(GPIO8)`、`bit1 = CH1(GPIO9)`。1MHz 采样 = 250KB/s（USB Full Speed 稳跑）。

> PulseView：驱动选 *Openbench Logic Sniffer & SUMP compatibles (ols)*，手动填 CDC#1
> 的 COM 号。注意流式与 PulseView 的"有限采样"模型不完全匹配——更适合用脚本 /
> sigrok-cli 做持续抓取。

LA 文本命令以 `m` 开头（0x6D 不是合法 SUMP 命令）：

```
mi                     查看状态（通道/时钟/上限）
mladebug on | off      调试输出到 GPIO16 @115200
msave                  立即保存 LA 设置
```

## 烧录

按住 BOOTSEL 上电，把 `debugprobe_on_pico.uf2` 拖进 `RPI-RP2` 盘；或 `tools\flash_combo.ps1`。

## 实现要点 / 已知限制

- **固件运行在 flash 里（XIP）**，没用 `copy_to_ram`（省 ~90KB RAM）。写 flash 先停
  SysTick 再关中断（Cortex-M 的 SysTick 是系统异常，`PRIMASK` 屏蔽不了，擦写期 XIP
  停、中断取向量会死锁）。
- **单核**（`configNUMBER_OF_CORES=1`）：flash 写入不需要 `multicore_lockout`。
- **clk_sys 固定 125MHz**（SDK 启动默认），上电不切时钟、运行期永不切；开机 100ms 防抖。
- **LA 流式**：PIO `in pins,2` → DMA 4KB 环（带 reload）→ `la_task` 每轮 drain 到 CDC#1。
  RAM 从 ~250KB 降到 ~58KB bss（200KB 采集缓冲已删）。
- **任务模型**：`cdc_thread`(UART桥) + `dap_thread`(DAP bulk) + `la_task`(LA+喂狗/落盘/LED)
  + `pwm_task`(PWM 文本)，mount 时创建、unmount 时删除。`cdc_stdio` 按实例参数化。
- **看门狗**：3s，由 `la_task`/`pwm_task` 的 housekeeping 喂狗，任务首次进循环才 enable。
- **dev_mon 的 SOF 看门狗始终守护**：若上板发现 LA 流式持续跑 PIO/DMA 时 USB 反复重枚举，
  需在 dev_mon 里对"LA 采集中"做豁免（原模式轮换下 PWM/LA 是跳过它的）。

## 许可

- DAPLink / debugprobe 部分：MIT（Raspberry Pi Ltd）
- 逻辑分析仪部分：GPLv3（Daniel Gorbea），合并分发整体按 GPLv3
