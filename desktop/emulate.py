#!/usr/bin/env python3
"""
Unicorn 模拟执行模块。

约定（裸机/固件段）：
  - 代码块加载到 CODE 地址，作为纯二进制直接执行。
  - 在 DATA 地址预留一块"出站缓冲区"，模拟代码把结果写到这里供外部读取。
  - 代码以 `RET` 结束，LR 指向 MAGIC 地址；引擎在 MAGIC 处 hook 停止，视作执行完成。
"""

import argparse
import struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UcError
from unicorn.arm64_const import (
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
    UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC,
)

# 内存布局（可被调用方覆盖）
CODE   = 0x0010_0000   # 代码基址
DATA   = 0x0001_0000   # 出站缓冲区
STACK  = 0x0300_0000   # 栈顶
MAGIC  = 0x0011_0000   # RET 之后 LR 所指的停机地址
PAGE   = 0x1000

XREGS = [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3]


def _to_bytes(word):
    return struct.pack("<I", word)


def run_arm64(blob: bytes, regs_in: dict = None,
              data_addr: int = DATA, data_len: int = 64) -> dict:
    """
    在 ARM64 模式下执行一段裸机器码。
    regs_in: 例如 {"x0": 5, "x1": 0} 传入初始寄存器。
    返回寄存器快照 + 出站缓冲区内容。
    """
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)

    # 映射代码区（含代码段上方的 MAGIC 停机点）
    mu.mem_map(CODE - PAGE, 2 * PAGE)          # 覆盖代码区
    mu.mem_map((MAGIC // PAGE) * PAGE, PAGE)   # MAGIC 停机页（RET 跳回这里触发停止）
    mu.mem_write(CODE, blob)

    # 数据区 / 栈
    mu.mem_map(((data_addr // PAGE) * PAGE), PAGE)
    mu.mem_map(((STACK - PAGE) // PAGE) * PAGE, 2 * PAGE)

    # 初始寄存器
    reg_map = {
        "x0": UC_ARM64_REG_X0, "x1": UC_ARM64_REG_X1,
        "x2": UC_ARM64_REG_X2, "x3": UC_ARM64_REG_X3,
    }
    for name, val in (regs_in or {}).items():
        if name in reg_map:
            mu.reg_write(reg_map[name], val)
    mu.reg_write(UC_ARM64_REG_SP, (STACK // PAGE) * PAGE + PAGE)
    mu.reg_write(UC_ARM64_REG_LR, MAGIC)

    # 在 MAGIC 处停机（代表代码正常返回）
    mu.hook_add(UC_HOOK_CODE, _stop_on_magic, begin=MAGIC, end=MAGIC)

    try:
        mu.emu_start(CODE, 0)
    except UcError as e:
        return {"error": f"UcError: {e}", "pc": hex(mu.reg_read(UC_ARM64_REG_PC))}

    regs_out = {f"x{i}": hex(mu.reg_read(XREGS[i])) for i in range(4)}
    result = mu.mem_read(data_addr, data_len)
    return {"registers": regs_out, f"outbox@0x{data_addr:x}": result.hex()}


def _stop_on_magic(uc, address, size, user_data):
    if address == MAGIC:
        uc.emu_stop()


def load_blob(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description="用 Unicorn 模拟执行一段 ARM64 裸机器码")
    ap.add_argument("blob", help="待模拟的二进制文件")
    ap.add_argument("--x0", type=int, default=5, help="初始 x0")
    ap.add_argument("--out", type=lambda x: int(x, 0), default=DATA, help="读取结果的内存地址")
    args = ap.parse_args()

    code = load_blob(args.blob)
    print(f"加载 {len(code)} 字节 -> 0x{CODE:x}")
    r = run_arm64(code, regs_in={"x0": args.x0})
    for k, v in r.items():
        print(f"{k}: {v}")


if __name__ == "__main__":
    main()