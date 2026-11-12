#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
la_scope.py - LA 实时流式示波器（2 通道滚动显示）

配合三合一固件的 LA 流式模式（CDC#1，1MHz 采样，2 通道打包 4 采样/字节）。
用法：
  1. 启动后自动探测 CDC#1（LA 口）并连接；
  2. 发 SUMP run(0x01) 开始流式；
  3. 窗口滚动显示 CH0(GPIO8) / CH1(GPIO9) 波形；
  4. 关闭窗口自动停止。

数据格式：每字节 4 个采样，采样 N 在 bit[2N:2N+1]（LSB 先），bit0=CH0、bit1=CH1。
采样率 1MHz，USB 吞吐约 250KB/s。
"""

import sys
import time
import threading
import collections

import serial
from serial.tools import list_ports

import numpy as np
import matplotlib
matplotlib.use("TkAgg")
from matplotlib import pyplot as plt
from matplotlib.animation import FuncAnimation

SAMPLE_RATE = 1000000   # 1MHz
WIN_SAMPLES = 4000      # 显示窗口 = 4000 采样 = 4ms
BAUD = 115200


def find_la_port():
    """探测哪个 COM 口是 LA（CDC#1）：发 0x02 看是否回 "1ALS"。"""
    for p in list_ports.comports():
        if p.vid is None:
            continue
        try:
            s = serial.Serial(p.device, BAUD, timeout=0.3)
            s.write(b"\x02")
            time.sleep(0.3)
            r = s.read(16)
            s.close()
            if b"1ALS" in r:
                return p.device
        except Exception:
            pass
    return None


class Scope:
    def __init__(self, port):
        self.ser = serial.Serial(port, BAUD, timeout=0.1)
        self.ch0 = collections.deque(maxlen=WIN_SAMPLES)
        self.ch1 = collections.deque(maxlen=WIN_SAMPLES)
        for _ in range(WIN_SAMPLES):
            self.ch0.append(0)
            self.ch1.append(0)
        self.running = True
        self.ser.write(b"\x01")   # SUMP run：开始流式
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        while self.running:
            try:
                d = self.ser.read(4096)
            except Exception:
                break
            if not d:
                continue
            # 解包：每字节 4 个采样，采样 N 在 bit[2N:2N+1]
            for b in d:
                self.ch0.append(b & 1)
                self.ch1.append((b >> 1) & 1)
                self.ch0.append((b >> 2) & 1)
                self.ch1.append((b >> 3) & 1)
                self.ch0.append((b >> 4) & 1)
                self.ch1.append((b >> 5) & 1)
                self.ch0.append((b >> 6) & 1)
                self.ch1.append((b >> 7) & 1)

    def stop(self):
        self.running = False
        try:
            self.ser.write(b"\x00")   # SUMP reset：停止流式
            self.ser.close()
        except Exception:
            pass


def main():
    port = find_la_port()
    if not port:
        print("未找到 LA 设备（CDC#1），请确认固件已烧录且设备已枚举。")
        sys.exit(1)
    print(f"连接 LA: {port}，采样率 {SAMPLE_RATE/1e6:.0f} MHz")

    scope = Scope(port)

    x = np.arange(WIN_SAMPLES) / SAMPLE_RATE * 1000.0   # ms
    fig, (ax0, ax1) = plt.subplots(2, 1, figsize=(11, 6))
    fig.suptitle(f"LA 实时流式示波器  ({port}, {SAMPLE_RATE/1e6:.0f} MHz, 窗口 {WIN_SAMPLES/1000}ms)")
    line0, = ax0.plot(x, np.zeros(WIN_SAMPLES), color="#2dd4bf", linewidth=1)
    line1, = ax1.plot(x, np.zeros(WIN_SAMPLES), color="#f59e0b", linewidth=1)
    for ax in (ax0, ax1):
        ax.set_ylim(-0.15, 1.15)
        ax.set_yticks([0, 1])
        ax.set_xlim(0, WIN_SAMPLES / SAMPLE_RATE * 1000.0)
        ax.grid(True, alpha=0.3)
    ax0.set_ylabel("CH0 (GPIO8)")
    ax1.set_ylabel("CH1 (GPIO9)")
    ax1.set_xlabel("时间 (ms)")

    def update(_frame):
        line0.set_ydata(list(scope.ch0))
        line1.set_ydata(list(scope.ch1))
        return line0, line1

    ani = FuncAnimation(fig, update, interval=50, blit=True)

    try:
        plt.show()
    finally:
        scope.stop()
        print("已停止")


if __name__ == "__main__":
    main()
