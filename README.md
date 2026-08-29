# 手机传输数据 → 电脑 Unicorn 模拟

一个通用管道：手机把二进制数据高速、不丢包地（**可靠 UDP**）传到电脑，
电脑端可视化界面接收后交给 [Unicorn](https://www.unicorn-engine.org/) 模拟执行，
并把寄存器快照与结果内存可视化展示。手机与电脑端都带 GUI。

## 目录结构

```
phone/
  sender_gui.py   # 手机发送端 GUI：选文件 + 实时进度/速率/重传（tkinter，跨平台）
  client.py       # 备选：HTTP POST 发送（无 GUI 的简单场景）
desktop/
  receiver_gui.py # 电脑接收端 GUI：进度 + Unicorn 结果可视化（tkinter）
  server.py       # 备选：HTTP 接收服务器
  emulate.py      # Unicorn 模拟执行模块（ARM64 裸机代码段）
rudp/
  transfer.py     # 可靠 UDP 传输库：分片/序号/ACK/重传/重组
  __init__.py
examples/
  build_blob.py   # 生成示例 ARM64 机器码 blob.bin
  blob.bin
```

## 依赖

电脑端 / 运行 GUI 的机器：
```bash
pip3 install unicorn
```
GUI 用 Python 自带 `tkinter`（无第三方依赖）。手机端可在 Android Termux + X11（`export DISPLAY=:0`）或 iOS Pythonista 下直接运行同一套 tkinter 发送端。

## 使用（推荐：可靠 UDP + GUI）

设备和电脑需在同一局域网、可互相访问，必要时放行所用端口。

### 1. 电脑端：启动接收 GUI

```bash
python3 desktop/receiver_gui.py
```
点「启动监听」（默认端口 8010）。界面会显示对端地址、接收进度、速率，并在完成后自动保存到 `desktop/inbox/` 且调用 Unicorn 模拟，把寄存器与结果内存展示在结果区。

### 2. 手机端：启动发送 GUI

```bash
python3 phone/sender_gui.py      # 桌面直接运行；Termux / Pythonista 亦可用
```
填入电脑的局域网 IP 和端口 → 「选择文件」（建议先用 `examples/blob.bin`）→ 「发送」。界面实时显示已确认字节、速率、重传次数；可靠的 UDP 传输保证不丢包，最后自动完成握手。

### 3. 生成示例数据（可选）

```bash
python3 examples/build_blob.py   # 生成 examples/blob.bin
```

## 可靠 UDP 协议（`rudp/transfer.py`）

在 UDP 之上叠加可靠性，保证局域网内高速且不丢包：

- **分片**：大文件按 `MAX_CHUNK=1200` 字节切片，包头带 `total`/`seq`，避免 IP 分片。
- **序号 + 累计 ACK**：接收方按序重组，把连续边界回传。
- **滑动窗口 + 选择重传**：超过 `RTO` 未确认或丢失的块会被重发（默认窗口 128、RTO 0.15s）。
- **完成握手**：全部确认后发 `FIN`，接收方回确认才结束，双端确实落盘才算完成。

命令行可独立测试完整性（第二行是 40% 丢包环境）：
```bash
python3 rudp/transfer.py --drop 0.15 --size 300000
python3 rudp/transfer.py --drop 0.40 --size 2000000   # sha256 一致，不丢包
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

## 备选：HTTP 简单模式（无 GUI）

```bash
# 电脑
python3 desktop/server.py --host 0.0.0.0 --port 8010
# 手机
python3 phone/client.py --file examples/blob.bin --host 电脑IP --port 8010
```
服务器把文件存到 `desktop/inbox/`，返回 JSON（含 `emulation`）。

## 按需定制

- 换架构（x86/ARM）：在 `emulate.py` 中增加一个 `run_x86()`，改 `Uc` 的架构常量和寄存器映射即可。
- 改出站地址：`run_arm64(data, data_addr=...)`，并用 `--out` 指定 CLI 读取地址。
- 调吞吐：增大 `rudp/transfer.py` 里 `WINDOW`、调小 `RTO`，或调大 `SO_SNDBUF/SO_RCVBUF`。
- 只收不跑：接收 GUI 的「选择文件并模拟」可单独对本地文件做可视化模拟。