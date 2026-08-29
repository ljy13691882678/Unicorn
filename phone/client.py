#!/usr/bin/env python3
"""
手机端发送客户端：
  把本机一个二进制文件 POST 到电脑服务器 /upload，服务器会用 Unicorn 模拟并返回结果。
  仅用标准库 urllib，可在 Android Termux / iOS Pythonista 直接运行。

用法:
  python3 client.py --file /path/to/blob.bin --host 192.168.1.20 --port 8010
"""

import argparse
import json
import urllib.request


def send(file_path: str, host: str, port: int, name: str = None) -> dict:
    with open(file_path, "rb") as f:
        data = f.read()

    url = f"http://{host}:{port}/upload"
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    req.add_header("X-Filename", name or file_path.rsplit("/", 1)[-1])

    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", required=True, help="要发送的二进制文件路径")
    ap.add_argument("--host", required=True, help="电脑 IP，如 192.168.1.20")
    ap.add_argument("--port", type=int, default=8010)
    ap.add_argument("--name", default=None, help="保存的文件名（可选）")
    args = ap.parse_args()

    result = send(args.file, args.host, args.port, args.name)
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()