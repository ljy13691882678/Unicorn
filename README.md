# 手机传输数据 → 电脑 Unicorn 模拟

一个通用管道：手机把任意二进制数据（URL 编码的机器码/数据块）POST 到电脑，
电脑接收后交给 [Unicorn](https://www.unicorn-engine.org/) 模拟执行，并把寄存器快照
与结果内存返回给手机。

## 目录结构

```
phone/
  client.py        # 手机端发送客户端（仅标准库，Termux / Pythonista 可直接跑）
desktop/
  server.py        # 电脑端接收服务器（HTTP POST /upload）
  emulate.py       # Unicorn 模拟执行模块（ARM64 裸机代码段）
examples/
  build_blob.py    # 生成示例 ARM64 机器码 blob.bin
  blob.bin         # 生成的示例（20 字节）
```

## 依赖

电脑端（本机）：
```bash
pip3 install unicorn
```

手机端无需安装任何第三方库（只用 Python 标准库 `urllib`）。

## 使用

### 1. 电脑端启动服务器

```bash
python3 desktop/server.py --host 0.0.0.0 --port 8010
```

### 2. 生成示例数据（可选）

```bash
python3 examples/build_blob.py   # 生成 examples/blob.bin
```

### 3. 手机端发送并拿到模拟结果

```bash
python3 phone/client.py --file examples/blob.bin --host 192.168.1.20 --port 8010
```

`host` 填电脑局域网 IP（手机与电脑需同一网段、可互相访问，必要时放行防火墙端口）。
服务器会把文件存到 `desktop/inbox/`，同时返回模拟结果：

```json
{
  "saved": ".../desktop/inbox/1788012769_blob.bin",
  "bytes": 20,
  "emulation": {
    "registers": { "x0": "0x31", "x1": "0x10000", ... },
    "outbox@0x10000": "3100000000..."
  }
}
```

## 模拟约定

`emulate.py` 按"裸机/固件代码段"的方式执行：

| 项 | 地址 | 说明 |
|----|------|------|
| 代码基址 `CODE` | `0x0010_0000` | 收到的二进制从这里开始执行 |
| 出站缓冲区 `DATA` | `0x0001_0000` | 模拟代码把结果写到这里 |
| 栈 `STACK` | `0x0300_0000` | 预置好栈指针 |
| 停机点 `MAGIC` | `0x0011_0000` | 代码以 `ret` 结束，LR 指向此处触发停机 |

- 初始寄存器可通过 `run_arm64(blob, regs_in={"x0": 5})` 传入（命令行 `--x0 5`）。
- 示例 blob 等价汇编：`add x0,x0,#7` → `mul x0,x0,x0` → `str x0,[0x10000]` → `ret`。
  以 `x0=5` 为例结果 `144(0x90)`；`x0=0` 时结果 `49(0x31)`。

## 按需定制

- 换架构（x86/ARM）：在 `emulate.py` 中增加一个 `run_x86()`，改 `Uc` 的架构常量和寄存器映射即可。
- 改出站地址：`run_arm64(data, data_addr=...)`，并用 `--out` 指定 CLI 读取地址。
- 服务器只收不跑：`python3 desktop/server.py --no-emulate`。