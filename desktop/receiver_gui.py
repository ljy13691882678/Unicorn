#!/usr/bin/env python3
"""
电脑端接收 GUI（可视化）：
  启动后监听一个 UDP 端口，接收手机传来的数据（可靠 UDP，不丢包），
  显示接收进度 / 速率 / 包数量，自动保存并交给 Unicorn 模拟，展示寄存器与结果内存。

运行：python3 desktop/receiver_gui.py
"""

import os
import sys
import threading
import time

# 让脚本可以直接从仓库根目录调用
BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for p in (BASE, os.path.join(BASE, "desktop")):
    if p not in sys.path:
        sys.path.insert(0, p)

import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext

from rudp import RELUDPReceiver, MAX_CHUNK
from emulate import run_arm64

INBOX = os.path.join(BASE, "desktop", "inbox")


class ReceiverApp:
    def __init__(self, root):
        self.root = root
        self.recv = None
        self.last_data = b""

        root.title("电脑接收端 · 可靠UDP → Unicorn 模拟")
        self._build(root)

    def _build(self, root):
        top = ttk.Frame(root, padding=8)
        top.pack(fill="x")
        ttk.Label(top, text="监听端口:").pack(side="left")
        self.port_var = tk.StringVar(value="8010")
        ttk.Entry(top, textvariable=self.port_var, width=8).pack(side="left", padx=4)
        self.btn_start = ttk.Button(top, text="启动监听", command=self.start)
        self.btn_start.pack(side="left", padx=4)
        self.btn_stop = ttk.Button(top, text="停止", command=self.stop, state="disabled")
        self.btn_stop.pack(side="left")
        ttk.Button(top, text="选择文件并模拟", command=self.choose_file).pack(side="left", padx=12)

        mid = ttk.LabelFrame(root, text="传输", padding=8)
        mid.pack(fill="x", padx=8)
        self.peer_var = tk.StringVar(value="未连接")
        self.stat_var = tk.StringVar(value="0 / 0 字节  0 B/s  ·  0 块接收")
        ttk.Label(mid, textvariable=self.peer_var).pack(anchor="w")
        self.pbar = ttk.Progressbar(mid, maximum=100)
        self.pbar.pack(fill="x", pady=4)
        ttk.Label(mid, textvariable=self.stat_var).pack(anchor="w")

        # 结果区
        self.result = scrolledtext.ScrolledText(root, height=8)
        self.result.pack(fill="both", expand=True, padx=8, pady=4)
        self.result.insert("end", "模拟结果会显示在这里…\n")

        self.log = scrolledtext.ScrolledText(root, height=8)
        self.log.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        self.log.insert("end", "就绪。请先在手机端启动发送。\n")

    # ---------- 事件 ----------
    def log_msg(self, msg):
        self.log.insert("end", f"[{time.strftime('%H:%M:%S')}] {msg}\n")
        self.log.see("end")

    def start(self):
        port = int(self.port_var.get())
        self.recv = RELUDPReceiver(port)
        self.recv.on_peer = lambda addr: self.root.after(0, lambda a=addr: self.peer_var.set(f"对端: {a[0]}:{a[1]}"))
        self.recv.on_progress = lambda b, n: self.root.after(0, lambda: self.on_progress(b, n))
        self.recv.on_complete = lambda data: self.root.after(0, lambda: self.on_complete(data))
        self.recv.start()
        self.btn_start.config(state="disabled")
        self.btn_stop.config(state="normal")
        self.pbar["value"] = 0
        self.log_msg(f"开始监听端口 {port}")

    def stop(self):
        if self.recv:
            self.recv.stop()
            self.recv = None
        self.btn_start.config(state="normal")
        self.btn_stop.config(state="disabled")
        self.log_msg("已停止监听")

    def on_progress(self, boundary, total_chunks):
        pct = (boundary / total_chunks * 100) if total_chunks else 0
        self.pbar["value"] = pct
        self.stat_var.set(f"{boundary * MAX_CHUNK} / {total_chunks * MAX_CHUNK} 字节  ·  已收 {boundary}/{total_chunks} 块")

    def on_complete(self, data):
        self.last_data = data
        self.pbar["value"] = 100
        self.log_msg(f"接收完成：{len(data)} 字节")
        self._save(data)
        self._run_emulate(data)

    def _save(self, data):
        os.makedirs(INBOX, exist_ok=True)
        path = os.path.join(INBOX, f"{int(time.time())}_recv.bin")
        with open(path, "wb") as f:
            f.write(data)
        self.log_msg(f"已保存：{path}")

    def _run_emulate(self, data):
        self.result.delete("1.0", "end")
        self.result.insert("end", "正在 Unicorn 模拟…\n")
        threading.Thread(target=self._emulate_worker, args=(data,), daemon=True).start()

    def _emulate_worker(self, data):
        try:
            r = run_arm64(data)
            def show():
                self.result.delete("1.0", "end")
                self.result.insert("end", "=== Unicorn 执行结果 ===\n")
                for k, v in r.items():
                    if isinstance(v, dict):
                        self.result.insert("end", f"\n{k}:\n  " + "\n  ".join(f"{rk} = {rv}" for rk, rv in v.items()) + "\n")
                    else:
                        self.result.insert("end", f"{k}: {v}\n")
            self.root.after(0, show)
        except Exception as e:  # noqa: BLE001
            self.log_msg(f"模拟失败：{e}")

    def choose_file(self):
        path = filedialog.askopenfilename(title="选择二进制文件")
        if not path:
            return
        with open(path, "rb") as f:
            data = f.read()
        self.log_msg(f"手动选择：{path}（{len(data)} 字节）")
        self._run_emulate(data)


def main():
    root = tk.Tk()
    ReceiverApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()