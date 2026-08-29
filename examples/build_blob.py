#!/usr/bin/env python3
"""生成示例 ARM64 裸机器码 blob.bin，供手机发送 / 电脑模拟使用。

指令序列（等价汇编）:
    add  x0, x0, #7       0x91001c00
    mul  x0, x0, x0       0x9b007c00   (madd x0,x0,x0,xzr)
    movz x1, #1, lsl#16   0xd2a00021   -> x1 = 1 << 16 = 0x10000 (出站缓冲区)
    str  x0, [x1]         0xf9000020   -> 把结果写到 0x10000
    ret                   0xd65f03c0
"""

import os
import struct


def build(path):
    words = [0x91001C00, 0x9B007C00, 0xD2A00021, 0xF9000020, 0xD65F03C0]
    blob = b"".join(struct.pack("<I", w) for w in words)
    with open(path, "wb") as f:
        f.write(blob)
    print(f"写出 {len(blob)} 字节 -> {path}")
    print("等价汇编:")
    print("    add  x0, x0, #7")
    print("    mul  x0, x0, x0")
    print("    movz x1, #1, lsl#16")
    print("    str  x0, [x1]")
    print("    ret")


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    build(os.path.join(here, "blob.bin"))