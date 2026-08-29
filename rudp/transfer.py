#!/usr/bin/env python3
"""
可靠 UDP（Reliable UDP）传输库。

在 UDP 之上叠加，保证局域网内高速且不丢包：
  - 分片：大文件按 MAX_CHUNK 切成数据块，包头带 total / seq。
  - 序号 + 累计 ACK：接收方按序重组，把连续边界回传给发送方。
  - 重传：发送方对超出 RTO 仍未确认的块重发（选择重传）。
  - 完成握手：全部确认后发 FIN，接收方回确认才结束。

用法（同机演示 / 手机=发送 电脑=接收）：
    recv = RELUDPReceiver(bind_port)
    recv.on_complete = lambda data,...: ...
    recv.start()
    send = RELUDPSender(('host', bind_port), data)
    send.on_progress = lambda acked,total,bps,retx: ...
    send.start(); send.wait()
"""

import os
import socket
import struct
import threading
import time

MAGIC = b"RU"
T_DATA = 1
T_FIN = 2
T_ACK = 3

MAX_CHUNK = 1200          # 每个数据块负载字节数（避免 IP 分片）
WINDOW = 128              # 发送窗口（在途块数）
RTO = 0.15                # 重传超时（秒）
SEND_INTERVAL = 0.002     # 发送循环节拍
ACK_DONE = 0xFFFFFFFF     # 完成确认标记

DROP_PROB = 0.0           # 测试用：丢包率（0=不丢）

_HEADER = struct.Struct(">2sBQI H")


def _pack(msg_type, total, seq, payload=b""):
    return _HEADER.pack(MAGIC, msg_type, total, seq, len(payload)) + payload


def _unpack(pkt):
    if len(pkt) < _HEADER.size:
        return None
    magic, mtype, total, seq, dlen = _HEADER.unpack(pkt[:_HEADER.size])
    if magic != MAGIC:
        return None
    return mtype, total, seq, pkt[_HEADER.size:_HEADER.size + dlen]


def _maybe_drop():
    if DROP_PROB > 0 and os.environ.get("RUDP_NO_DROP") != "1":
        import random
        return random.random() < DROP_PROB
    return False


class RELUDPReceiver:
    """接收端：绑定 UDP 端口，重组数据，完成时回调。"""

    def __init__(self, bind_port, buf_size=4 * 1024 * 1024):
        self.port = bind_port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, buf_size)
        self.sock.bind(("0.0.0.0", bind_port))
        self.sock.settimeout(0.05)
        self._peer = None
        self._chunks = {}       # seq -> bytes
        self._total = 0
        self._boundary = 0      # 已连续收到的块数
        self._stop = threading.Event()
        self._th = None
        self.on_complete = None  # fn(data: bytes)
        self.on_progress = None  # fn(boundary, total_chunks)
        self.on_peer = None      # fn(peer_addr)

    def start(self):
        self._th = threading.Thread(target=self._run, daemon=True)
        self._th.start()
        return self

    def stop(self):
        self._stop.set()

    def join(self, timeout=None):
        if self._th:
            self._th.join(timeout)

    def _run(self):
        while not self._stop.is_set():
            try:
                pkt, addr = self.sock.recvfrom(65535)
            except socket.timeout:
                if self._stop.is_set():
                    break
                continue
            if _maybe_drop():
                continue
            if self._peer is None:
                self._peer = addr
                if self.on_peer:
                    self.on_peer(addr)
            self._handle(pkt, addr)

    def _handle(self, pkt, addr):
        msg = _unpack(pkt)
        if msg is None:
            return
        mtype, total, seq, payload = msg

        if mtype == T_DATA:
            if self._total == 0:
                self._total = total
            if self._total:
                nchunks = (self._total + MAX_CHUNK - 1) // MAX_CHUNK
                if seq < nchunks:
                    self._chunks[seq] = payload
                    self._advance()
        elif mtype == T_FIN:
            # 发送结束确认
            self.sock.sendto(_pack(T_ACK, 0, ACK_DONE), addr)
        elif mtype == T_ACK:
            pass

    def _advance(self):
        nchunks = (self._total + MAX_CHUNK - 1) // MAX_CHUNK
        while self._boundary in self._chunks:
            self._boundary += 1
        # 累计 ACK
        if self._peer and self._boundary > 0:
            try:
                self.sock.sendto(_pack(T_ACK, self._total, self._boundary), self._peer)
            except OSError:
                pass
        if self.on_progress:
            self.on_progress(self._boundary, nchunks)
        # 全部收齐，组装
        if self._boundary >= nchunks and self._total:
            data = b"".join(self._chunks[i] for i in range(nchunks))
            if self.on_complete:
                cb = self.on_complete
                self.on_complete = None  # 只一次性完成
                cb(data)


class RELUDPSender:
    """发送端：分片 + 滑动窗口 + 重传 + FIN 握手。"""

    def __init__(self, target, data, window=WINDOW, rto=RTO):
        self.target = target
        self.data = data
        self.total = len(data)
        self.nchunks = (self.total + MAX_CHUNK - 1) // MAX_CHUNK if self.total else 0
        self.chunks = [data[i:i + MAX_CHUNK] for i in range(0, self.total, MAX_CHUNK)]
        self.window = window
        self.rto = rto
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.settimeout(0.05)
        self._ack_boundary = 0   # 已确认的连续块数
        self._sent = {}          # seq -> last_sent_time
        self._done = threading.Event()
        self._retx = 0
        self._start_ts = time.monotonic()
        self._th = None
        self.on_progress = None  # fn(acked_bytes, total_bytes, bps, retx)

    def start(self):
        self._th = threading.Thread(target=self._run, daemon=True)
        self._th.start()
        return self

    def wait(self, timeout=30):
        return self._done.wait(timeout)

    @property
    def acked(self):
        return self._ack_boundary * MAX_CHUNK

    def _run(self):
        send_ts = 0.0
        fin_ts = 0.0
        fin_done = False
        while not self._done.is_set():
            now = time.monotonic()
            # 1) 发送窗口内的未确认块（含超时重传）
            for seq in range(self._ack_boundary, min(self._ack_boundary + self.window, self.nchunks)):
                if self._sent.get(seq, 0.0) == 0.0:
                    self.sock.sendto(self._pack_data(seq), self.target)
                    self._sent[seq] = now
                elif now - self._sent[seq] >= self.rto:
                    self.sock.sendto(self._pack_data(seq), self.target)
                    self._sent[seq] = now
                    self._retx += 1
                if self._done.is_set():
                    break
            # 2) 处理 ACK
            self._recv_acks()

            # 3) 进度回调
            if self.on_progress and self.nchunks:
                self.on_progress(self.acked, self.total, self.bps(), self._retx)

            # 4) 全部确认 -> 发 FIN
            if self._ack_boundary >= self.nchunks and not fin_done and self.nchunks:
                if now - fin_ts >= self.rto or fin_ts == 0.0:
                    self.sock.sendto(_pack(T_FIN, self.total, self.nchunks), self.target)
                    fin_ts = now
                    fin_done = True
            if fin_done and now - fin_ts >= self.rto * 4:
                # 超时仍无确认则结束（尽力而为）
                self._done.set()
            time.sleep(SEND_INTERVAL)
        self.sock.close()

    def _recv_acks(self):
        while True:
            try:
                pkt, _ = self.sock.recvfrom(65535)
            except socket.timeout:
                return
            msg = _unpack(pkt)
            if msg is None:
                continue
            mtype, _total, seq, _ = msg
            if mtype == T_ACK and seq == ACK_DONE:
                self._done.set()
                return
            if mtype == T_ACK:
                if seq > self._ack_boundary:
                    self._ack_boundary = seq
                    # 已确认的在途块从计时表清理
                    for s in list(self._sent):
                        if s < seq:
                            self._sent.pop(s, None)

    def _pack_data(self, seq):
        return _pack(T_DATA, self.total, seq, self.chunks[seq])

    def bps(self):
        el = time.monotonic() - self._start_ts
        return int(self.acked / el) if el > 0 else 0


# 快速一键传输助手（供 GUI /测试 使用）
def fast_send(host, port, data, on_progress=None, timeout=60):
    """常规命令里调用：建立发送端并等待完成。返回 (ok, acked, bytes, retx)。"""
    s = RELUDPSender((host, port), data)
    if on_progress:
        s.on_progress = on_progress
    s.start()
    ok = s.wait(timeout)
    return ok, s.acked, s.total, s._retx


if __name__ == "__main__":
    # 自检：本机回环 + 强制丢包，验证完整性
    import hashlib, sys, random, argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=300000)
    ap.add_argument("--drop", type=float, default=0.15)
    args = ap.parse_args()

    DROP_PROB = args.drop
    blob = os.urandom(args.size)
    ref = hashlib.sha256(blob).hexdigest()

    recv_port = random.randint(20000, 40000)
    got = {}

    recv = RELUDPReceiver(recv_port)
    recv.on_complete = lambda data, got=got: got.update(data=data)
    recv.start()
    time.sleep(0.2)

    st = time.monotonic()
    ok, acked, total, retx = fast_send("127.0.0.1", recv_port, blob, timeout=30)
    recv.join(1)

    recv.stop()
    if ok:
        print(f"丢包率={args.drop} 大小={args.size}B 重传={retx} 速率={int(acked/(time.monotonic()-st))} B/s")
        if got.get("data") == blob:
            print("完整性 OK  sha256:", ref)
        else:
            sys.exit("完整性 FAILED")
    else:
        sys.exit("传输未在超时内完成")