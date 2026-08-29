#!/usr/bin/env python3
"""
电脑端接收服务器：
  手机把二进制 POST 到 /upload  ->  保存到 inbox/  ->  可选交给 Unicorn 模拟  ->  返回 JSON 结果。
"""

import argparse
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from emulate import load_blob, run_arm64

HERE = os.path.dirname(os.path.abspath(__file__))
INBOX = os.path.join(HERE, "inbox")


def save_blob(data: bytes, name: str) -> str:
    os.makedirs(INBOX, exist_ok=True)
    safe = os.path.basename(name) or "blob.bin"
    path = os.path.join(INBOX, f"{int(time.time())}_{safe}")
    with open(path, "wb") as f:
        f.write(data)
    return path


class Handler(BaseHTTPRequestHandler):
    emulate = True

    def _send(self, code: int, obj: dict):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path.rstrip("/") != "/upload":
            return self._send(404, {"error": "not found"})

        length = int(self.headers.get("Content-Length", 0))
        data = self.rfile.read(length) if length else b""
        name = self.headers.get("X-Filename", "blob.bin")
        path = save_blob(data, name)

        result = {"saved": path, "bytes": len(data)}
        if self.emulate and data:
            emu = run_arm64(data)
            result["emulation"] = emu
        self._send(200, result)

    def log_message(self, fmt, *args):
        print("[server]", fmt % args)


def main():
    ap = argparse.ArgumentParser(description="接收手机上传的二进制并交给 Unicorn 模拟")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8010)
    ap.add_argument("--no-emulate", action="store_true", help="只接收不模拟")
    args = ap.parse_args()

    Handler.emulate = not args.no_emulate
    print(f"监听 http://{args.host}:{args.port}/upload ，文件保存到 {INBOX}")
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()