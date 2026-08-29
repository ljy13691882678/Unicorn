# 手机->电脑 Unicorn 模拟（ImGui 版）

手机（**ImGui APK**）用**可靠 UDP**把二进制数据不丢包地发给电脑；电脑（**ImGui EXE**）接收、
保存，再丢给 **Unicorn** 模拟执行 ARM64 裸机代码，把寄存器快照 + 结果内存可视化显示。

```
imgui_app/
├── common/               # 跨平台核心（手机/电脑共用）
│   ├── reliable_udp.{hpp,cpp}  # 可靠 UDP：分片/序号/累计ACK/滑动窗口/选择重传/FIN握手
│   ├── emu.{hpp,cpp}            # Unicorn ARM64 模拟封装（可选编译）
│   └── selftest.cpp             # 无界面自测（g++ 一行即可跑）
├── desktop/              # 电脑端 → Windows EXE（CMake + ImGui + GLFW + OpenGL3）
│   ├── main.cpp
│   └── CMakeLists.txt
└── android/              # 手机端 → APK（NDK NativeActivity + Gradle + ImGui）
    ├── third_party/imgui/        # vendored Dear ImGui（已内置）
    ├── app/src/main/cpp/          # android_main.cpp + app CMake（可靠UDP直接复用 common/）
    └── app/build.gradle 等
```

---

## 0. 先自跑核心自测（可选，Linux/macOS）

```bash
cd imgui_app/common
g++ -std=c++17 -O2 selftest.cpp reliable_udp.cpp emu.cpp -lpthread -ldl -lm -o selftest
RUDP_DROP=0.4 ./selftest 800000     # 40% 丢包下仍完整，验证可靠性
# 想验证 Unicorn 模拟：
#   pip install unicorn
#   UNI=$(python3 -c "import unicorn,os;print(os.path.dirname(unicorn.__file__))")
#   g++ -std=c++17 -O2 -DUSE_UNICORN -I"$UNI/include" selftest.cpp reliable_udp.cpp emu.cpp "$UNI/lib/libunicorn.a" -lpthread -ldl -lm -o selftest
#   RUDP_DROP=0.15 ./selftest 300000
```

---

## 1. 电脑端：生成 Windows EXE

### 1.1 安装工具链（任选其一）
- **MSVC**：装 Visual Studio 2019+（含 C++ 桌面开发 + CMake）。
- **MinGW-w64**：装 `x86_64-w64-mingw32-g++`（如 msys2 或 zig），需与 CMake 兼容。
- **Unicorn（可选，要显示模拟结果建议装）**：`vcpkg install unicorn:x64-windows`。
  - 不装也能出 EXE，只是界面会提示「Unicorn 未编译」，跳过模拟展示。
  - 装完把 unicorn 的 include/lib 所在目录记为 `UNICORN_ROOT`。

### 1.2 配置并编译
在 `imgui_app/desktop` 打开终端：

```bat
:: 首次联网会自动拉取 Dear ImGui + GLFW
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release          :: 不带 Unicorn，最快跑通
rem cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUSE_UNICORN=ON -DUNICORN_ROOT=C:\path\to\unicorn
cmake --build build --config Release
```

产物：`build/desktop.exe`（MinGW 会生成 `build/desktop`）。

> 生成器示例：MSVC 用默认 `-G "Visual Studio 17 2022"`；MinGW 用
> `-G "MinGW Makefiles"`。vcpkg 建议 `-DCMAKE_TOOLCHAIN_FILE=` 指向 vcpkg 的
> `scripts/buildsystems/vcpkg.cmake`，Unicorn 依赖会更好解析。

### 1.3 运行
双击 `desktop.exe` → 「启动监听」（默认端口 8010，注意放行防火墙）→ 让手机发数。
收到数据后自动：保存到 `imgui_app/desktop/inbox/` → 运行 Unicorn → 显示 x0..x3 寄存器 +
结果内存 hex dump。
窗口右下角还有一个「本机发送端·自测」，无需手机即可在电脑上回环验证整条链路。

---

## 2. 手机端：生成 APK

### 2.1 准备
- 安装 **Android Studio**（含 Android SDK + NDK，`r23+`，建议 NDK 一个稳定版）。
- 本工程已把 Dear ImGui 内置在 `android/third_party/imgui`，无需另下。

### 2.2 用 Android Studio 构建
1. `File → Open` 选择 `imgui_app/android`。
2. 等 Gradle Sync 完成（首次会下载 Gradle/AGP，需网络）。
3. `Build → Build Bundle(s)/APK(s) → Build APK(s)`。
产物：`android/app/build/outputs/apk/debug/app-debug.apk`。

> 没有 Android Studio 时，也可用命令行：
> ```bash
> cd android && gradle wrapper && ./gradlew assembleDebug
> ```
>（首次会自动下载 AGP 与 NDK；或用 `sdkmanager` 装好 NDK 后设置 `ANDROID_NDK_HOME`。）

### 2.3 使用
1. 手机与电脑连同一 WiFi（同一局域网）。
2. 手机安装 APK 并打开：填电脑的局域网 IP 与接收端口（8010）→ 点「发送示例」。
3. 发的是内置 ARM64 示例 Blob（`add x0,#7; mul x0,x0; str; ret`）；电脑收到后会自动模拟。
4. 界面实时显示已确认字节、进度、重传次数。

---

## 可靠 UDP 关键点（common/reliable_udp）

- 按 `kMaxChunk=1200` 分片，包头带 `total / seq / len`（大端，与早期 Python 版一致）。
- 接收方按序重组并回**累计 ACK**（连续边界 `boundary`）。
- 发送方用**滑动窗口**（`kWindow=128`）+ 超过 `kRto=0.15s` **选择重传**。
- 全部确认后发 `FIN`，接收方回 FIN-ACK，双端才认为传输结束（不丢包）。
- 全是非阻塞 socket，UI 每帧调 `pump()/poll()`，天然适合 ImGui 主循环。

## Unicorn 约定（common/emu）

裸机代码段直接以其 PE 无关的裸字节加载执行：

| 项 | 地址 |
|----|------|
| 代码基址 CODE | `0x0010_0000` |
| 出站缓冲 DATA  | `0x0001_0000` |
| 栈 STACK       | `0x0300_0000` |
| 停机点 MAGIC（ret 后 LR 所指）| `0x0011_0000` |

模拟代码把结果写到 DATA，读回即得到结果内存；同时取 x0..x3 作为寄存器快照。

## 数据流向

```
手机 APK (Sender, ImGui) --可靠UDP(chunks/ACK/retx/FIN)--> 电脑 EXE (Receiver, ImGui)
                                                            │ 保存 inbox/*.bin
                                                            v
                                                     Unicorn 模拟 ARM64
                                                            │
                                         寄存器快照 x0..x3 + 结果内存 hex → 界面展示
```