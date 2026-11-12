# -*- coding: utf-8 -*-
"""
combo_ui.py - RP2040 三合一上位机（PWM 信号源 + LA 流式示波器）
Copyright (c) 2026

功能：
  1. PWM 标签页：2 路独立频率/占空比方波发生器（连 CDC#2）
  2. LA 标签页：2 通道实时流式示波器（连 CDC#1，1MHz 采样）

三功能并行：设备枚举为 3 个 COM 口 + 1 个 CMSIS-DAP。
  - CDC#0 = UART 桥（本工具不用）
  - CDC#1 = LA（本工具 LA 标签页）
  - CDC#2 = PWM（本工具 PWM 标签页）

PWM 协议：文本命令 f<ch>:<freq>  d<ch>:<duty>  on<ch>/off<ch>  i  save
LA 协议：SUMP 握手，run(0x01) 开始流式，数据每字节 4 个采样（bit[2N:2N+1]，bit0=CH0）。
"""

import time
import os
import re
import shutil
import string
import threading
import collections

import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext, filedialog

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None

import numpy as np
import matplotlib
matplotlib.use("TkAgg")
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg

BAUD = 115200

APP_NAME    = "RP2040 Combo"
APP_VERSION = "2.0"
APP_DATE    = "2026-09-21"
APP_DESC    = "PWM 信号源 + LA 流式示波器"

# CHAN0-1 = GPIO6-7（三功能并行：DAPLink=GPIO2-5，PWM=GPIO6-7，LA=GPIO8-9）
CH_GPIO = {0: 6, 1: 7}

LA_SAMPLE_RATE = 1000000   # 1MHz
LA_WIN_SAMPLES = 4000      # 显示窗口 4ms

# ================= 现代配色 =================
C_BG      = "#f4f6f9"
C_CARD    = "#ffffff"
C_HEADER  = "#1e293b"
C_HEADER_T = "#ffffff"
C_TEXT    = "#1e293b"
C_MUTED   = "#64748b"
C_BORDER  = "#e2e8f0"
C_ACCENT  = "#2563eb"
C_ACCENT_D = "#1d4ed8"
C_GREEN   = "#16a34a"
C_RED     = "#dc2626"
C_AMBER   = "#d97706"


class ToggleSwitch(tk.Canvas):
    """iOS 风格滑动开关。"""
    W, H = 46, 24

    def __init__(self, master, variable, command=None, **kw):
        super().__init__(master, width=self.W, height=self.H, highlightthickness=0,
                         bd=0, bg=master["bg"], cursor="hand2", **kw)
        self.var = variable
        self.command = command
        self.bind("<Button-1>", self._on_click)
        self.var.trace_add("write", lambda *_: self._redraw())
        self._redraw()

    def _on_click(self, _evt=None):
        self.var.set(not self.var.get())
        if self.command:
            self.command()

    def _redraw(self):
        self.delete("all")
        on = bool(self.var.get())
        bg = C_GREEN if on else "#cbd5e1"
        x = 24 if on else 2
        self.create_oval(1, 1, 23, 23, fill="#ffffff", outline="")
        self.create_rectangle(12, 2, 34, 22, fill=bg, outline="")
        self.create_oval(22, 1, 45, 23, fill="#ffffff", outline="")
        self.create_oval(x, 1, x + 22, 23, fill="#ffffff", outline=bg, width=2)


class ComboUI:
    def __init__(self, root):
        self.root = root
        self.root.title(f"{APP_NAME}  v{APP_VERSION}")
        self.root.configure(bg=C_BG)
        self.root.geometry("1000x820")
        self.root.minsize(880, 680)

        # PWM 串口状态
        self.ser = None
        self.running = False
        self.rx_buffer = b""
        self._last_duty_send = {}
        self._syncing = False
        # LA 串口状态
        self.la_ser = None
        self.la_running = False
        self.la_ch0 = collections.deque(maxlen=LA_WIN_SAMPLES)
        self.la_ch1 = collections.deque(maxlen=LA_WIN_SAMPLES)

        self._build()

        self.root.after(600, self.auto_connect)

    # ================= 样式 =================
    def _setup_style(self):
        s = ttk.Style()
        try:
            s.theme_use("clam")
        except Exception:
            pass
        s.configure("TCombobox", fieldbackground=C_CARD, background=C_CARD,
                    foreground=C_TEXT, bordercolor=C_BORDER, lightcolor=C_BORDER,
                    darkcolor=C_BORDER, arrowcolor=C_MUTED, padding=4)
        s.configure("TScale", background=C_CARD, troughcolor="#e5eaf2",
                    lightcolor=C_ACCENT, darkcolor=C_ACCENT)
        s.configure("TNotebook", background=C_BG, borderwidth=0)
        s.configure("TNotebook.Tab", padding=(16, 8), font=("Microsoft YaHei UI", 10))

    # ================= UI 搭建 =================
    def _build(self):
        self._setup_style()
        self._build_header()

        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True, padx=10, pady=(4, 10))
        self.nb = nb

        self.pwm_tab = tk.Frame(nb, bg=C_BG)
        self.la_tab = tk.Frame(nb, bg=C_BG)
        nb.add(self.pwm_tab, text="  PWM 信号源  ")
        nb.add(self.la_tab, text="  LA 流式示波器  ")

        self._build_pwm_tab()
        self._build_la_tab()

    def _build_header(self):
        bar = tk.Frame(self.root, bg=C_HEADER, height=60)
        bar.pack(fill="x")
        bar.pack_propagate(False)

        left = tk.Frame(bar, bg=C_HEADER)
        left.pack(side="left", padx=16, pady=8)
        tk.Label(left, text=APP_NAME, bg=C_HEADER, fg=C_HEADER_T,
                 font=("Microsoft YaHei UI", 15, "bold")).pack(anchor="w")
        tk.Label(left, text=f"{APP_DESC}    v{APP_VERSION}", bg=C_HEADER,
                 fg="#94a3b8", font=("Microsoft YaHei UI", 9)).pack(anchor="w")

        right = tk.Frame(bar, bg=C_HEADER)
        right.pack(side="right", padx=16)

        self.help_btn = tk.Button(right, text="? 帮助", command=self._open_help,
                                  bg="#334155", fg=C_HEADER_T, relief="flat",
                                  activebackground="#475569", activeforeground="white",
                                  font=("Microsoft YaHei UI", 9), cursor="hand2")
        self.help_btn.pack(side="left", padx=(0, 12))

        self.update_btn = tk.Button(right, text="更新固件", command=self._update_firmware,
                                    bg="#7c3aed", fg="white", relief="flat",
                                    activebackground="#6d28d9", activeforeground="white",
                                    font=("Microsoft YaHei UI", 9), cursor="hand2")
        self.update_btn.pack(side="left")

    # ================= PWM 标签页 =================
    def _build_pwm_tab(self):
        self._build_pwm_conn_row()
        self._build_pwm_panel()
        self._build_log_panel()

    def _build_pwm_conn_row(self):
        frame = tk.Frame(self.pwm_tab, bg=C_BG)
        frame.pack(fill="x", padx=6, pady=(8, 2))

        tk.Label(frame, text="PWM 信号源", bg=C_BG, fg=C_GREEN,
                 font=("Microsoft YaHei UI", 10, "bold")).pack(side="left")
        tk.Label(frame, text="连 CDC#2（GPIO6/7）", bg=C_BG, fg=C_MUTED,
                 font=("Microsoft YaHei UI", 9)).pack(side="left", padx=8)

        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(frame, textvariable=self.port_var, width=9, state="readonly")
        self.port_cb.pack(side="right", padx=(0, 4))

        self.refresh_btn = tk.Button(frame, text="↻", command=self.refresh_ports,
                                     bg=C_HEADER, fg="white", relief="flat",
                                     activebackground="#334155", activeforeground="white",
                                     font=("", 11), width=2, cursor="hand2")
        self.refresh_btn.pack(side="right", padx=(0, 6))

        self.connect_btn = tk.Button(frame, text="连接", command=self.toggle_connect,
                                     bg=C_ACCENT, fg="white", relief="flat",
                                     activebackground=C_ACCENT_D, activeforeground="white",
                                     font=("Microsoft YaHei UI", 10, "bold"),
                                     width=6, cursor="hand2")
        self.connect_btn.pack(side="right", padx=(0, 6))

        self.status_dot = tk.Label(frame, text="●", bg=C_BG, fg="#64748b", font=("", 13))
        self.status_dot.pack(side="right", padx=(0, 4))

    def _build_pwm_panel(self):
        panel = tk.Frame(self.pwm_tab, bg=C_CARD, highlightbackground=C_BORDER,
                         highlightthickness=1)
        panel.pack(fill="both", expand=True, padx=6, pady=6)

        head = tk.Frame(panel, bg=C_CARD)
        head.pack(fill="x", padx=14, pady=(12, 6))
        tk.Label(head, text="PWM 2 通道", bg=C_CARD, fg=C_TEXT,
                 font=("Microsoft YaHei UI", 12, "bold")).pack(side="left")
        tk.Label(head, text="CHAN0-1 = GPIO6-7", bg=C_CARD, fg=C_MUTED,
                 font=("Microsoft YaHei UI", 9)).pack(side="left", padx=10)
        self.refresh_state_btn = tk.Button(head, text="刷新状态", command=lambda: self.send_cmd("i"),
                                           bg=C_HEADER, fg="white", relief="flat",
                                           activebackground="#334155", activeforeground="white",
                                           font=("Microsoft YaHei UI", 9), cursor="hand2")
        self.refresh_state_btn.pack(side="right", padx=12)

        grid = tk.Frame(panel, bg=C_CARD)
        grid.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        self.freq_vars = {}
        self.duty_vars = {}
        self.duty_entry_vars = {}
        self.on_vars = {}
        self.ch_card = {}

        for ch in range(2):
            card = tk.Frame(grid, bg=C_BG, highlightbackground=C_BORDER, highlightthickness=1)
            card.grid(row=0, column=ch, sticky="nsew", padx=6, pady=5)
            grid.columnconfigure(ch, weight=1)
            self.ch_card[ch] = card
            self._build_ch_card(card, ch)

        grid.rowconfigure(0, weight=1)

    def _build_ch_card(self, card, ch):
        top = tk.Frame(card, bg=C_BG)
        top.pack(fill="x", padx=10, pady=(8, 4))
        tk.Label(top, text=f"CH{ch}", bg=C_BG, fg=C_ACCENT,
                 font=("Consolas", 11, "bold")).pack(side="left")
        tk.Label(top, text=f"GPIO{CH_GPIO[ch]}", bg=C_BG, fg=C_MUTED,
                 font=("Consolas", 9)).pack(side="left", padx=8)
        self.on_vars[ch] = tk.BooleanVar(value=True)
        ToggleSwitch(top, self.on_vars[ch],
                     command=lambda c=ch: self.send_enable(c)).pack(side="right")

        frow = tk.Frame(card, bg=C_BG)
        frow.pack(fill="x", padx=10, pady=2)
        tk.Label(frow, text="频率", bg=C_BG, fg=C_MUTED,
                 font=("Microsoft YaHei UI", 9)).pack(side="left")
        tk.Label(frow, text="Hz", bg=C_BG, fg=C_MUTED,
                 font=("Microsoft YaHei UI", 9)).pack(side="right")
        self.freq_vars[ch] = tk.StringVar(value="1000")
        fe = tk.Entry(frow, textvariable=self.freq_vars[ch], bg=C_CARD, fg=C_TEXT,
                      insertbackground=C_TEXT, relief="solid", bd=1,
                      highlightbackground=C_BORDER, highlightthickness=1,
                      font=("Consolas", 10), justify="right")
        fe.pack(side="right", fill="x", expand=True, padx=(6, 0))
        fe.bind("<Return>", lambda e, c=ch: self.send_freq(c))
        fe.bind("<FocusOut>", lambda e, c=ch: self.send_freq(c))

        drow = tk.Frame(card, bg=C_BG)
        drow.pack(fill="x", padx=10, pady=(2, 8))
        tk.Label(drow, text="占空比", bg=C_BG, fg=C_MUTED,
                 font=("Microsoft YaHei UI", 9)).pack(side="left")
        self.duty_vars[ch] = tk.IntVar(value=50)
        self.duty_entry_vars[ch] = tk.StringVar(value="50")
        de = tk.Entry(drow, textvariable=self.duty_entry_vars[ch], width=4,
                      bg=C_CARD, fg=C_TEXT, insertbackground=C_TEXT,
                      relief="solid", bd=1, highlightbackground=C_BORDER,
                      highlightthickness=1, font=("Consolas", 9), justify="center")
        de.pack(side="right", padx=(0, 2))
        tk.Label(drow, text="%", bg=C_BG, fg=C_MUTED,
                 font=("Microsoft YaHei UI", 9)).pack(side="right")
        de.bind("<Return>", lambda e, c=ch: self.on_duty_entry(c))
        de.bind("<FocusOut>", lambda e, c=ch: self.on_duty_entry(c))

        ds = tk.Scale(drow, from_=0, to=100, orient="horizontal",
                      variable=self.duty_vars[ch],
                      command=lambda v, c=ch: self.on_duty_scale(c, v),
                      bg=C_BG, fg=C_TEXT, length=120, showvalue=False, resolution=1,
                      highlightthickness=0, troughcolor="#e5eaf2",
                      activebackground=C_ACCENT, bd=0)
        ds.pack(side="right", fill="x", expand=True, padx=(6, 4))

    def _build_log_panel(self):
        panel = tk.Frame(self.pwm_tab, bg=C_CARD, highlightbackground=C_BORDER, highlightthickness=1)
        panel.pack(fill="both", padx=6, pady=(0, 6))
        head = tk.Frame(panel, bg=C_CARD)
        head.pack(fill="x", padx=14, pady=(8, 2))
        tk.Label(head, text="PWM 日志", bg=C_CARD, fg=C_TEXT,
                 font=("Microsoft YaHei UI", 10, "bold")).pack(side="left")
        tk.Button(head, text="清空", command=self._clear_log, bg=C_BG, fg=C_TEXT,
                  relief="flat", activebackground="#e5eaf2",
                  font=("Microsoft YaHei UI", 9), cursor="hand2").pack(side="right")

        self.log = scrolledtext.ScrolledText(panel, bg="#0f172a", fg="#e2e8f0",
                                             insertbackground="#e2e8f0", height=6,
                                             font=("Consolas", 9), relief="flat",
                                             borderwidth=0, padx=10, pady=6)
        self.log.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.log.tag_configure("tx", foreground="#60a5fa")
        self.log.tag_configure("rx", foreground="#94a3b8")
        self.log.tag_configure("sys", foreground="#34d399")
        self.log.tag_configure("err", foreground="#f87171")
        self.log.configure(state="disabled")

        self.refresh_ports()

    def _clear_log(self):
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    def log_line(self, text, tag="sys"):
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n", tag)
        self.log.see("end")
        self.log.configure(state="disabled")

    def log_text(self, text, tag="rx"):
        self.log.configure(state="normal")
        self.log.insert("end", text, tag)
        self.log.see("end")
        self.log.configure(state="disabled")

    # ================= LA 标签页 =================
    def _build_la_tab(self):
        # 串口行
        frame = tk.Frame(self.la_tab, bg=C_BG)
        frame.pack(fill="x", padx=6, pady=(8, 2))

        tk.Label(frame, text="LA 流式示波器", bg=C_BG, fg=C_AMBER,
                 font=("Microsoft YaHei UI", 10, "bold")).pack(side="left")
        tk.Label(frame, text="连 CDC#1（GPIO8/9），1MHz", bg=C_BG, fg=C_MUTED,
                 font=("Microsoft YaHei UI", 9)).pack(side="left", padx=8)

        self.la_port_var = tk.StringVar()
        self.la_port_cb = ttk.Combobox(frame, textvariable=self.la_port_var, width=9, state="readonly")
        self.la_port_cb.pack(side="right", padx=(0, 4))

        self.la_refresh_btn = tk.Button(frame, text="↻", command=self.la_refresh_ports,
                                        bg=C_HEADER, fg="white", relief="flat",
                                        activebackground="#334155", activeforeground="white",
                                        font=("", 11), width=2, cursor="hand2")
        self.la_refresh_btn.pack(side="right", padx=(0, 6))

        self.la_btn = tk.Button(frame, text="开始", command=self.la_toggle,
                                bg=C_AMBER, fg="white", relief="flat",
                                activebackground="#b45309", activeforeground="white",
                                font=("Microsoft YaHei UI", 10, "bold"),
                                width=6, cursor="hand2")
        self.la_btn.pack(side="right", padx=(0, 6))

        self.la_status = tk.Label(frame, text="●", bg=C_BG, fg="#64748b", font=("", 13))
        self.la_status.pack(side="right", padx=(0, 4))

        # matplotlib 图
        fig = Figure(figsize=(10, 5), dpi=100, facecolor=C_CARD)
        self.la_fig = fig
        self.la_ax0 = fig.add_subplot(211)
        self.la_ax1 = fig.add_subplot(212)
        self.la_ax0.set_ylim(-0.15, 1.15)
        self.la_ax1.set_ylim(-0.15, 1.15)
        x = np.arange(LA_WIN_SAMPLES) / LA_SAMPLE_RATE * 1000.0
        self.la_x = x
        for ax in (self.la_ax0, self.la_ax1):
            ax.set_yticks([0, 1])
            ax.set_xlim(0, LA_WIN_SAMPLES / LA_SAMPLE_RATE * 1000.0)
            ax.grid(True, alpha=0.3)
        self.la_ax0.set_ylabel("CH0 (GPIO8)")
        self.la_ax1.set_ylabel("CH1 (GPIO9)")
        self.la_ax1.set_xlabel("时间 (ms)")
        self.la_line0, = self.la_ax0.plot(x, np.zeros(LA_WIN_SAMPLES), color="#2dd4bf", linewidth=1)
        self.la_line1, = self.la_ax1.plot(x, np.zeros(LA_WIN_SAMPLES), color="#f59e0b", linewidth=1)
        fig.tight_layout()

        canvas = FigureCanvasTkAgg(fig, master=self.la_tab)
        canvas.get_tk_widget().pack(fill="both", expand=True, padx=6, pady=6)
        self.la_canvas = canvas

        for _ in range(LA_WIN_SAMPLES):
            self.la_ch0.append(0)
            self.la_ch1.append(0)

        self.la_refresh_ports()

    def la_refresh_ports(self):
        ports = []
        if list_ports:
            ports = [p.device for p in list_ports.comports()]
        self.la_port_cb["values"] = ports
        if not self.la_port_var.get():
            # 自动探测 LA 口（CDC#1）
            p = self._find_la_port()
            if p:
                self.la_port_var.set(p)
            elif ports:
                self.la_port_var.set(ports[0])

    def _find_la_port(self):
        if not list_ports or not serial:
            return None
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

    def la_toggle(self):
        if self.la_running:
            self.la_stop()
        else:
            self.la_start()

    def la_start(self):
        if not serial:
            messagebox.showerror("错误", "未安装 pyserial")
            return
        port = self.la_port_var.get().strip()
        if not port:
            messagebox.showerror("错误", "请选择 LA 串口")
            return
        try:
            self.la_ser = serial.Serial(port, BAUD, timeout=0.1)
            self.la_ser.write(b"\x01")   # SUMP run
        except Exception as e:
            messagebox.showerror("打开 LA 串口失败", str(e))
            self.la_ser = None
            return
        self.la_running = True
        self.la_btn.configure(text="停止", bg=C_RED, activebackground="#b91c1c")
        self.la_status.configure(fg=C_GREEN)
        threading.Thread(target=self._la_reader, daemon=True).start()
        self._la_animate()

    def la_stop(self):
        self.la_running = False
        if self.la_ser and self.la_ser.is_open:
            try:
                self.la_ser.write(b"\x00")   # SUMP reset
                self.la_ser.close()
            except Exception:
                pass
        self.la_ser = None
        self.la_btn.configure(text="开始", bg=C_AMBER, activebackground="#b45309")
        self.la_status.configure(fg="#64748b")

    def _la_reader(self):
        while self.la_running and self.la_ser and self.la_ser.is_open:
            try:
                d = self.la_ser.read(4096)
            except Exception:
                break
            if not d:
                continue
            for b in d:
                self.la_ch0.append(b & 1)
                self.la_ch1.append((b >> 1) & 1)
                self.la_ch0.append((b >> 2) & 1)
                self.la_ch1.append((b >> 3) & 1)
                self.la_ch0.append((b >> 4) & 1)
                self.la_ch1.append((b >> 5) & 1)
                self.la_ch0.append((b >> 6) & 1)
                self.la_ch1.append((b >> 7) & 1)

    def _la_animate(self):
        if not self.la_running:
            return
        self.la_line0.set_ydata(list(self.la_ch0))
        self.la_line1.set_ydata(list(self.la_ch1))
        self.la_canvas.draw_idle()
        self.root.after(50, self._la_animate)

    # ================= PWM 串口 =================
    def refresh_ports(self):
        ports = []
        if list_ports:
            ports = [p.device for p in list_ports.comports()]
        self.port_cb["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _find_usb_port(self):
        if not list_ports or not serial:
            return None
        usb = []
        for p in list_ports.comports():
            try:
                if p.vid is not None:
                    usb.append(p.device)
            except Exception:
                pass
        if not usb:
            return None

        def com_num(dev):
            m = re.search(r"(\d+)\s*$", dev.upper())
            return int(m.group(1)) if m else 0
        usb.sort(key=com_num, reverse=True)

        for port in usb:
            try:
                s = serial.Serial(port, BAUD, timeout=0.3)
                s.dtr = True
                s.rts = True
                s.write(b"i\n")
                time.sleep(0.6)
                data = s.read(4096)
                s.close()
                if b"Ch | GPIO" in data:
                    return port
            except Exception:
                continue
        return usb[0]

    def auto_connect(self):
        if self.ser and self.ser.is_open:
            return
        self.refresh_ports()
        target = self._find_usb_port()
        if target:
            self.port_var.set(target)
            self.connect()

    def toggle_connect(self):
        if self.ser and self.ser.is_open:
            self.disconnect()
        else:
            self.connect()

    def connect(self):
        if not serial:
            messagebox.showerror("错误", "未安装 pyserial，请先 pip install pyserial")
            return
        port = self.port_var.get().strip()
        if not port:
            messagebox.showerror("错误", "请选择串口")
            return
        try:
            self.ser = serial.Serial(port, BAUD, timeout=0.3)
            self.ser.dtr = True
            self.ser.rts = True
        except Exception as e:
            messagebox.showerror("打开串口失败", str(e))
            self.ser = None
            return

        self.running = True
        self.connect_btn.configure(text="断开", bg=C_RED, activebackground="#dc2626")
        self.status_dot.configure(fg=C_GREEN)
        self.log_line(f"已连接 {port} @ {BAUD}", "sys")

        self.root.after(50, self._poll_reader)
        self.root.after(400, lambda: self.send_cmd("i"))

    def disconnect(self):
        self.running = False
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.connect_btn.configure(text="连接", bg=C_ACCENT, activebackground=C_ACCENT_D)
        self.status_dot.configure(fg="#64748b")
        self.log_line("已断开", "sys")

    def _port_present(self):
        port = self.port_var.get().strip()
        if not port:
            return False
        try:
            return any(p.device == port for p in list_ports.comports())
        except Exception:
            return False

    def _poll_reader(self):
        if not self.running or not self.ser or not self.ser.is_open:
            return
        try:
            if self.ser.in_waiting:
                data = self.ser.read(self.ser.in_waiting)
                text = data.decode("utf-8", errors="replace")
                self.log_text(text, tag="rx")
                self.rx_buffer += data
                self._parse_buffer()
        except Exception:
            self.log_line("串口读异常，尝试重连...", "err")
            self._schedule_reconnect()
            return

        self._poll_cnt = getattr(self, "_poll_cnt", 0) + 1
        if self._poll_cnt % 10 == 0 and not self._port_present():
            self.log_line("检测到设备断开，等待重连...", "err")
            self._schedule_reconnect()
            return

        self.root.after(50, self._poll_reader)

    def _schedule_reconnect(self):
        if not self.running:
            return
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.ser = None
        self.status_dot.configure(fg="#f59e0b")
        self.refresh_ports()
        port = self.port_var.get().strip() or self._find_usb_port()
        if port:
            self.port_var.set(port)
        if not port:
            self.root.after(400, self._schedule_reconnect)
            return
        try:
            self.ser = serial.Serial(port, BAUD, timeout=0.3)
            self.ser.dtr = True
            self.ser.rts = True
            self.status_dot.configure(fg=C_GREEN)
            self.log_line(f"已重连 {port}", "sys")
            self.root.after(150, lambda: self.send_cmd("i"))
            self.root.after(50, self._poll_reader)
        except Exception:
            self.ser = None
            self.root.after(400, self._schedule_reconnect)

    # ================= 固件更新 =================
    def _find_rp2_drive(self):
        for letter in string.ascii_uppercase:
            root = letter + ":\\"
            try:
                if os.path.exists(os.path.join(root, "INFO_UF2.TXT")) or \
                   os.path.exists(os.path.join(root, "INDEX.HTM")):
                    return root
            except Exception:
                pass
        return None

    def _update_firmware(self):
        path = filedialog.askopenfilename(
            title="选择要烧录的固件", filetypes=[("UF2 固件", "*.uf2"), ("所有文件", "*.*")])
        if not path:
            return
        if self.ser and self.ser.is_open:
            try:
                self.ser.write(b"bootsel\n")
                self.ser.flush()
                self.log_line("已发送 bootsel，板子重启进 RPI-RP2 烧录盘...", "sys")
            except Exception as e:
                self.log_line(f"发送 bootsel 失败: {e}", "err")
        else:
            self.log_line("未连接：请先连接（或手动按住 BOOTSEL 上电）", "err")
        self.disconnect()
        threading.Thread(target=self._copy_uf2_to_rp2, args=(path,), daemon=True).start()

    def _copy_uf2_to_rp2(self, path):
        drive = None
        for _ in range(20):
            drive = self._find_rp2_drive()
            if drive:
                break
            time.sleep(1)
        if not drive:
            self.log_line("未等到 RPI-RP2 盘，请手动 BOOTSEL", "err")
            return
        try:
            shutil.copyfile(path, os.path.join(drive, os.path.basename(path)))
            self.log_line(f"已写入 {os.path.basename(path)}，板子自动烧录并重启...", "sys")
            self.root.after(4000, self._reconnect_after_update)
        except Exception as e:
            self.log_line(f"复制 uf2 失败: {e}", "err")

    def _reconnect_after_update(self):
        self.running = True
        self._schedule_reconnect()

    # ================= PWM 状态解析 =================
    def _parse_buffer(self):
        buf = self.rx_buffer
        m = re.search(rb"Ch \| GPIO.*?^end\r?$", buf, re.S | re.M)
        if not m:
            return
        block = m.group(0).decode("utf-8", errors="replace")
        rows = {}
        for line in block.splitlines():
            mm = re.match(r"\s*(\d)\s*\|\s*\d+\s*\|\s*(\d+)\s*\|\s*([\d.]+)\s*\|\s*(ON|OFF)", line)
            if mm:
                ch = int(mm.group(1))
                rows[ch] = (int(mm.group(2)), float(mm.group(3)), mm.group(4) == "ON")
        self.root.after(0, lambda: self._apply_status(rows))
        self.rx_buffer = self.rx_buffer[m.end():]

    def _apply_status(self, rows):
        self._syncing = True
        try:
            for ch, (freq, duty, state) in rows.items():
                if ch in self.freq_vars:
                    self.freq_vars[ch].set(str(freq))
                if ch in self.duty_vars:
                    self.duty_vars[ch].set(int(round(duty)))
                    self.duty_entry_vars[ch].set(f"{duty:.1f}".rstrip("0").rstrip("."))
                if ch in self.on_vars:
                    self.on_vars[ch].set(state)
        finally:
            self._syncing = False

    # ================= PWM 发送 =================
    def send_cmd(self, cmd):
        if not self.ser or not self.ser.is_open:
            self.log_line("未连接，忽略命令", "err")
            return
        line = cmd.rstrip("\n") + "\n"
        try:
            self.ser.write(line.encode("utf-8"))
            self.log_line(f"> {cmd}", "tx")
        except Exception as e:
            self.log_line(f"发送失败: {e}", "err")

        if not self._syncing:
            self.root.after(150, lambda: self._refresh_if_needed(cmd))

    def _refresh_if_needed(self, cmd):
        if not self.ser or not self.ser.is_open:
            return
        if re.match(r"^(f\d|d\d|on\d|off\d)", cmd):
            try:
                self.ser.write(b"i\n")
            except Exception:
                pass

    def send_freq(self, ch):
        val = self.freq_vars[ch].get().strip()
        if not val:
            return
        self.send_cmd(f"f{ch}:{val}")

    def on_duty_scale(self, ch, val_str):
        if self._syncing:
            return
        self.duty_entry_vars[ch].set(val_str)
        now = time.time()
        last = self._last_duty_send.get(ch, 0)
        if now - last < 0.08:
            return
        self._last_duty_send[ch] = now
        self.send_duty(ch)

    def on_duty_entry(self, ch):
        if self._syncing:
            return
        v = self.duty_entry_vars[ch].get().strip()
        try:
            val = int(round(float(v)))
            val = max(0, min(100, val))
        except (ValueError, TypeError):
            val = self.duty_vars[ch].get()
        self.duty_vars[ch].set(val)
        self.duty_entry_vars[ch].set(str(val))
        self.send_duty(ch)

    def send_duty(self, ch):
        val = self.duty_vars[ch].get()
        self.send_cmd(f"d{ch}:{val}")

    def send_enable(self, ch):
        on = self.on_vars[ch].get()
        self.send_cmd(f"{'on' if on else 'off'}{ch}")

    # ================= 帮助 =================
    def _open_help(self):
        win = tk.Toplevel(self.root)
        win.title(f"帮助 - {APP_NAME}")
        win.configure(bg=C_BG)
        win.geometry("760x640")
        win.minsize(560, 460)
        win.transient(self.root)

        head = tk.Frame(win, bg=C_HEADER, height=72)
        head.pack(fill="x")
        head.pack_propagate(False)
        tk.Label(head, text=APP_NAME, bg=C_HEADER, fg=C_HEADER_T,
                 font=("Microsoft YaHei UI", 15, "bold")).pack(anchor="w", padx=18, pady=(12, 0))
        tk.Label(head, text=f"v{APP_VERSION}  ·  {APP_DATE}  ·  {APP_DESC}",
                 bg=C_HEADER, fg="#94a3b8",
                 font=("Microsoft YaHei UI", 9)).pack(anchor="w", padx=18)

        body = tk.Frame(win, bg=C_BG)
        body.pack(fill="both", expand=True, padx=18, pady=12)
        txt = scrolledtext.ScrolledText(body, bg=C_CARD, fg=C_TEXT, relief="flat",
                                        borderwidth=0, font=("Microsoft YaHei UI", 10),
                                        padx=14, pady=12)
        txt.pack(fill="both", expand=True)
        txt.tag_configure("h", font=("Microsoft YaHei UI", 12, "bold"), foreground=C_ACCENT)
        txt.tag_configure("b", font=("Microsoft YaHei UI", 10, "bold"))
        txt.tag_configure("c", font=("Consolas", 9), foreground="#0f766e")

        def H(s): txt.insert("end", s + "\n", "h")
        def B(s): txt.insert("end", s + "\n", "b")
        def C(s): txt.insert("end", s + "\n", "c")
        def T(s): txt.insert("end", s + "\n", "t")

        H("1. 三功能并行")
        T("设备枚举为 3 个 COM 口 + 1 个 CMSIS-DAP，三者同时在线、互不干扰：")
        C("CDC#0 = UART 桥（本工具不用）")
        C("CDC#1 = LA 流式示波器（本工具「LA 流式示波器」标签页）")
        C("CDC#2 = PWM 信号源（本工具「PWM 信号源」标签页）")
        C("IF0  = CMSIS-DAP v2 调试探针（Keil/pyOCD）")

        H("2. 引脚对照")
        C("DAPLink: SWCLK=GPIO2  SWDIO=GPIO3  UART_TX=GPIO4  UART_RX=GPIO5")
        C("PWM: CH0=GPIO6  CH1=GPIO7")
        C("LA : CH0=GPIO8  CH1=GPIO9")

        H("3. PWM 标签页")
        T("连 CDC#2，2 路独立方波，频率 1Hz-20MHz，占空比 0-100%（0.1 步进）。")
        C("f<ch>:<Hz>  d<ch>:<%>  on<ch>/off<ch>  i  save")
        T("参数记忆在板上 flash，掉电自动恢复。")

        H("4. LA 标签页")
        T("连 CDC#1，2 通道实时流式，1MHz 采样，滚动显示 CH0/CH1 波形。")
        T("点「开始」发 run 启动流式，点「停止」发 reset 结束。")
        T("数据格式：每字节 4 个采样，采样 N 在 bit[2N:2N+1]，bit0=CH0、bit1=CH1。")

        H("5. 更新固件")
        T("PWM 标签页连接后点「更新固件」，选 uf2 会自动重启进烧录盘并写入。")

        txt.configure(state="disabled")


def main():
    root = tk.Tk()
    ComboUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
