#!/usr/bin/env python3
"""
手机端发送 GUI（可视化）：
  选择本地文件，通过可靠 UDP 高速发送到电脑（不丢包），实时显示
  进度、已确认字节、速率、重传次数。

基于 tkinter，跨平台：桌面直接 `python3 phone/sender_gui.py` 运行；
Android 可在 Termux + X11（`export DISPLAY=:0`）下运行，iOS Pythonista 亦可。
"""

import os
import sys
import threading
import time

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if BASE not in sys.path:
    sys.path.insert(0, BASE)

import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext

from rudp import RELUDPSender


class SenderApp:
    def __init__(self, root):
        self.root = root
        self.sender = None
        self.data = b""
        self.path = ""

        root.title("手机发送端 · 可靠UDP")
        self._build(root)

    def _build(self, root):
        f0 = ttk.LabelFrame(root, text="电脑服务器", padding=8)
        f0.pack(fill="x", padx=8, pady=4)
        ttk.Label(f0, text="目标IP:").grid(row=0, column=0, sticky="e")
        self.host_var = tk.StringVar(value="192.168.1.20")
        ttk.Entry(f0, textvariable=self.host_var, width=16).grid(row=0, column=1)
        ttk.Label(f0, text="端口:").grid(row=0, column=2, sticky="e", padx=(12, 0))
        self.port_var = tk.StringVar(value="8010")
        ttk.Entry(f0, textvariable=self.port_var, width=7).grid(row=0, column=3)
        f0.columnconfigure(1, weight=1)

        f1 = ttk.LabelFrame(root, text="文件", padding=8)
        f1.pack(fill="x", padx=8, pady=4)
        self.file_var = tk.StringVar(value="未选择文件")
        ttk.Entry(f1, textvariable=self.file_var, state="readonly").pack(side="left", fill="x", expand=True)
        ttk.Button(f1, text="选择文件", command=self.choose_file).pack(side="left", padx=6)
        self.btn_send = ttk.Button(f1, text="发送", command=self.send, state="disabled")
        self.btn_send.pack(side="left")

        prog = ttk.LabelFrame(root, text="传输", padding=8)
        prog.pack(fill="x", padx=8, pady=4)
        self.pbar = ttk.Progressbar(prog, maximum=100)
        self.pbar.pack(fill="x")
        self.stat_var = tk.StringVar(value="已确认 0 / 0 字节   0 B/s   重传 0")
        ttk.Label(prog, textvariable=self.stat_var).pack(anchor="w", pady=(4, 0))

        self.log = scrolledtext.ScrolledText(root, height=10)
        self.log.pack(fill="both", expand=True, padx=8, pady=(4, 8))

    def log_msg(self, msg):
        self.log.insert("end", f"[{time.strftime('%H:%M:%S')}] {msg}\n")
        self.log.see("end")

    def choose_file(self):
        path = filedialog.askopenfilename(title="选择要发送的文件")
        if not path:
            return
        with open(path, "rb") as f:
            self.data = f.read()
        self.path = path
        self.file_var.set(f"{path}（{len(self.data):,} 字节）")
        self.btn_send.config(state="normal")
        self.log_msg(f"已选择 {os.path.basename(path)}，{len(self.data)} 字节")

    def send(self):
        if not self.data:
            return
        host = self.host_var.get().strip()
        port = int(self.port_var.get().strip())
        self.btn_send.config(state="disabled")
        self.pbar["value"] = 0
        self.log_msg(f"开始发送到 {host}:{port}（可靠UDP，不丢包）…")
        self.sender = RELUDPSender((host, port), self.data)
        self.sender.on_progress = lambda a, t, bps, retx: self.root.after(
            0, lambda: self.on_progress(a, t, bps, retx))
        threading.Thread(target=self._send_worker, daemon=True).start()

    def _send_worker(self):
        self.sender.start()
        ok = self.sender.wait(timeout=120)
        def done():
            self.pbar["value"] = 100
            self.btn_send.config(state="normal")
            self.log_msg("发送完成 ✓（电脑端已验证接收完整）" if ok else "发送超时/未完成")
        self.root.after(0, done)

    def on_progress(self, acked, total, bps, retx):
        pct = (acked / total * 100) if total else 0
        self.pbar["value"] = pct
        self.stat_var.set(f"已确认 {acked:,} / {total:,} 字节   {bps:,} B/s   重传 {retx}")


def main():
    root = tk.Tk()
    SenderApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()