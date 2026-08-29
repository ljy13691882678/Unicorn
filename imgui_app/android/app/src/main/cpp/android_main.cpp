// android_main.cpp - 手机端 ImGui 发送程序（NDK NativeActivity -> APK）
// 核心工作：输入电脑局域网 IP/端口，用可靠 UDP 把 ARM64 机器码（blob）不丢包地发给电脑。
// 参考 Dear ImGui 的 example_android_opengl3。
#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/log.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>

#include <imgui.h>
#include <imgui_impl_android.h>
#include <imgui_impl_opengl3.h>

#include "reliable_udp.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <chrono>
#include <signal.h>
#include <unwind.h>
#include <unistd.h>
#include <fcntl.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "phone-udp", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "phone-udp", __VA_ARGS__)

// ---- 进程内崩溃兜底 ----
// 捕获 SIGSEGV/SIGABRT/SIGBUS，把调用栈打到 logcat（tag=phone-udp）并写入
// /data/data/<包名>/files/crash.txt。有 root 时可直接 cat 该文件，或用其定位。
static const char* gCrashFilePath = "/data/data/com.example.phoneudp/files/crash.txt";

// Android bionic 没有 backtrace()，用 NDK 的 _Unwind_Backtrace（32/64 位均可用）。
static _Unwind_Reason_Code trace_cb(struct _Unwind_Context* ctx, void* arg) {
    void* ip = reinterpret_cast<void*>(_Unwind_GetIP(ctx));
    if (ip) { reinterpret_cast<std::vector<void*>*>(arg)->push_back(ip); }
    return _URC_NO_REASON;
}

static void crash_handler(int sig) {
    std::vector<void*> frames;
    _Unwind_Backtrace(trace_cb, &frames);
    __android_log_print(ANDROID_LOG_FATAL, "phone-udp",
                        "== 崩溃 signal=%d (%s), frames=%zu ==", sig,
                        sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT" : "SIGBUS", frames.size());
    char line[64];
    int fd = open(gCrashFilePath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    for (size_t i = 0; i < frames.size(); ++i) {
        snprintf(line, sizeof(line), "  #%02zu %p\n", i, frames[i]);
        __android_log_print(ANDROID_LOG_FATAL, "phone-udp", "%s", line);
        if (fd >= 0) { write(fd, line, strlen(line)); }
    }
    __android_log_print(ANDROID_LOG_FATAL, "phone-udp", "== 请将以上 #00..#%zu 栈发回 ==", frames.size() - 1);
    if (fd >= 0) close(fd);
    _exit(128 + sig);
}

// 示例 ARM64 机器码：add x0,#7 ; mul x0,x0 ; movz x1,#1<<16 ; str x0,[x1] ; ret
static const uint8_t kSampleBlob[] = {
    0x00,0x1c,0x00,0x91, 0x00,0x7c,0x00,0x9b,
    0x21,0x00,0xa0,0xd2, 0x20,0x00,0x00,0xf9,
    0xc0,0x03,0x5f,0xd6
};
static const size_t kSampleSize = sizeof(kSampleBlob);

// ---- 应用状态（全局，原生回调需要）----
static struct android_app* gApp = nullptr;

static struct {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig  config = 0;
    bool configured = false;
    bool imguiAndroidReady = false;
    int width = 0, height = 0;
    int orientation = 0;

    char ip[64] = "192.168.1.100";
    char portBuf[16] = "8010";

    rudp::Sender sender;
    bool sending = false, sendDone = false;
    int  sendRetx = 0;
    uint64_t sendTotal = 0;
    const uint8_t* payload = &kSampleBlob[0];
    size_t payloadSize = kSampleSize;

    std::string log;
    char logbuf[512];
} g;

static void logAppend(const char* fmt, ...) {
    va_list va; va_start(va, fmt);
    vsnprintf(g.logbuf, sizeof(g.logbuf), fmt, va);
    va_end(va);
    g.log += g.logbuf; g.log += "\n";
    if (g.log.size() > 6000) g.log.erase(0, g.log.size() - 5000);
    LOGI("%s", g.logbuf);
}

// ---- EGL：首次创建上下文；每次窗口重建时(重)建 surface ----
static bool init_gl() {
    if (g.display == EGL_NO_DISPLAY)
        g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g.display, nullptr, nullptr)) { LOGE("eglInitialize"); return false; }

    if (g.context == EGL_NO_CONTEXT) {
        const EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8,
            EGL_DEPTH_SIZE, 0,
            EGL_NONE
        };
        EGLint n;
        if (!eglChooseConfig(g.display, attribs, &g.config, 1, &n) || n < 1) { LOGE("eglChooseConfig"); return false; }
        EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        g.context = eglCreateContext(g.display, g.config, EGL_NO_CONTEXT, ctxAttrs);
        if (g.context == EGL_NO_CONTEXT) return false;
    }

    if (g.surface == EGL_NO_SURFACE) {
        g.surface = eglCreateWindowSurface(g.display, g.config, gApp->window, nullptr);
        if (g.surface == EGL_NO_SURFACE) { LOGE("eglCreateWindowSurface"); return false; }
    }
    if (!eglMakeCurrent(g.display, g.surface, g.surface, g.context)) { LOGE("eglMakeCurrent"); return false; }
    g.width = ANativeWindow_getWidth(gApp->window);
    g.height = ANativeWindow_getHeight(gApp->window);
    return true;
}

static void render() {
    if (!g.configured) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(8, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2((float)g.width - 16.f, (float)g.height - 24.f), ImGuiCond_FirstUseEver);
    ImGui::Begin("手机发送端 · 可靠 UDP");

    ImGui::TextUnformatted("电脑局域网 IP:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##ip", g.ip, sizeof(g.ip));
    ImGui::TextUnformatted("电脑接收端口:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##port", g.portBuf, sizeof(g.portBuf));
    ImGui::Text("发送内容：内置 ARM64 示例 Blob（%d 字节）", (int)g.payloadSize);
    if (ImGui::Button("发送示例", ImVec2(-1, 44))) {
        unsigned p = (unsigned)std::atoi(g.portBuf);
        if (p > 0) {
            std::vector<uint8_t> blob(g.payload, g.payload + g.payloadSize);
            g.sender.start(g.ip, (uint16_t)p, blob);
            g.sendTotal = blob.size(); g.sending = true; g.sendDone = false; g.sendRetx = 0;
            logAppend("开始发送 %d 字节 -> %s:%u", (int)blob.size(), g.ip, p);
        } else logAppend("端口无效");
    }

    ImGui::Separator();
    if (g.sending) {
        g.sendDone = g.sender.pump();
        g.sendRetx = g.sender.retransmits();
        if (g.sendDone) { g.sending = false; logAppend("发送完成，重传 %d 次", g.sendRetx); }
        uint64_t acked = g.sender.ackedBytes();
        float f = g.sendTotal ? (float)((double)acked / g.sendTotal) : 0.f;
        if (f > 1.f) f = 1.f;
        ImGui::ProgressBar(f, ImVec2(-1, 0), f < 1.f ? "发送中..." : "完成");
        ImGui::Text("已确认 %llu / %llu   重传 %d",
                    (unsigned long long)acked, (unsigned long long)g.sendTotal, g.sendRetx);
        if (ImGui::Button("取消", ImVec2(-1, 40))) { g.sender.reset(); g.sending = false; }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("日志:");
    ImGui::SetNextItemWidth(-1);
    std::vector<char> lbuf(g.log.begin(), g.log.end());
    lbuf.push_back('\0');
    ImGui::InputTextMultiline("##log", lbuf.data(), lbuf.size(),
                              ImVec2(0, ImGui::GetContentRegionAvail().y),
                              ImGuiInputTextFlags_ReadOnly);

    ImGui::End();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (g.surface != EGL_NO_SURFACE) eglSwapBuffers(g.display, g.surface);
}

// ---- 生命周期 / 输入 ----
static void handleAppCmd(android_app* app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window) {
            init_gl();
            if (!g.imguiAndroidReady) {
                ImGui_ImplAndroid_Init(app->window);
                g.imguiAndroidReady = true;
            }
            g.configured = true;
        }
        break;
    case APP_CMD_TERM_WINDOW:
        g.configured = false;
        if (g.display != EGL_NO_DISPLAY) eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g.surface != EGL_NO_SURFACE) { eglDestroySurface(g.display, g.surface); g.surface = EGL_NO_SURFACE; }
        break;
    case APP_CMD_DESTROY:
        if (g.context != EGL_NO_CONTEXT) { eglDestroyContext(g.display, g.context); g.context = EGL_NO_CONTEXT; }
        if (g.display != EGL_NO_DISPLAY) { eglTerminate(g.display); g.display = EGL_NO_DISPLAY; }
        break;
    default: break;
    }
}

static int32_t handleInputEvent(android_app* app, AInputEvent* event) {
    return ImGui_ImplAndroid_HandleInputEvent(event);
}

void android_main(struct android_app* app) {
    // 关键：保持 native_app_glue 被链接。它的入口 ANativeActivity_onCreate 只被
    // Android 运行时按符号调用，代码里无人引用，若不加 app_dummy() 链接器会裁剪该
    // 静态库成员，导致 NativeActivity 缺少入口，App 一打开就闪退。
    app_dummy();

    // 装崩溃兜底（越早越好）：崩溃时把调用栈打到 logcat + crash.txt
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS,  crash_handler);

    app->activity->vm->AttachCurrentThread(&app->activity->env, nullptr);
    gApp = app;
    app->onAppCmd = handleAppCmd;
    app->onInputEvent = handleInputEvent;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplOpenGL3_Init("#version 300 es");

    logAppend("已就绪。请先运行电脑接收端并启动监听，输入电脑 IP 与端口后点「发送」。");

    while (true) {
        int events; android_poll_source* pSource;
        while (ALooper_pollAll(0, nullptr, &events, (void**)&pSource) >= 0) {
            if (pSource) pSource->process(app, pSource);
            if (app->destroyRequested) return;
        }
        if (g.configured) {
            if (!init_gl()) continue;
            render();
        }
    }
}