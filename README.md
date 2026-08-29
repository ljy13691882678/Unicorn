# Unicorn

手机（ImGui APK）用**可靠 UDP** 把二进制数据不丢包地发给电脑；电脑（ImGui EXE）接收后交给
**Unicorn** 模拟执行 ARM64 裸机代码，展示寄存器快照与结果内存。

详细说明见 [imgui_app/README.md](<imgui_app/README.md>)。

## 云端 CI 产物

GitHub Actions 自动构建（`.github/workflows/build.yml`），产物在每次构建后作为
**Artifacts** 上传到 Actions 页面下方下载：

| 触发方式 | 说明 |
|------|------|
| push 到 `main` | 自动构建 |
| 推送 `v*` tag（如 `v1.0`） | 自动构建（可用于发 Release） |
| Actions 页手动 “Run workflow” | 随时手动构建 |

### 产物清单

- **电脑端 Windows EXE**：`desktop-windows-exe` 下的 `desktop.exe`
- **手机端 Android APK**：`android-apk` 下的 `app-debug.apk`（Debug 签名，可直接安装）

下载：仓库 → **Actions** → 最上面的构建 → 底部 **Artifacts** → 点每个名字下载。

## 本地手动构建

```bash
# 电脑端 EXE（Windows 上需 CMake + VS/MinGW）
cmake -S imgui_app/desktop -B imgui_app/desktop/build -DCMAKE_BUILD_TYPE=Release
cmake --build imgui_app/desktop/build --config Release

# 手机端 APK（需 Android SDK + NDK r25）
cd imgui_app/android
./gradlew assembleDebug        # 产物 app/build/outputs/apk/debug/app-debug.apk
```