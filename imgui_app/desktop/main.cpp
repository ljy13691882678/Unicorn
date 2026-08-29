// desktop/main.cpp - 电脑端 ImGui 接收程序（CMake -> Windows EXE / 任意桌面）
// 核心工作：可靠 UDP 接收手机数据 -> 保存 -> Unicorn 模拟出寄存器快照 + 结果内存。
// 依赖：Dear ImGui + GLFW + OpenGL3（imgui 自带 gl3w loader，无需 GLEW）。
#include "reliable_udp.hpp"
#include "emu.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <ctime>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

// ===================== 辅助 =====================
static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::string timeStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32]; std::snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d",
        1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

static std::string hexDump(const std::vector<uint8_t>& d, size_t maxRows = 16) {
    std::ostringstream o;
    for (size_t i = 0; i < d.size() && i / 16 < maxRows; i += 16) {
        char a[17]; size_t k = 0;
        o << "0x" << std::hex << std::setw(8) << std::setfill('0') << i << ":  ";
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < d.size()) {
                o << std::setw(2) << std::setfill('0') << (int)d[i + j] << " ";
                a[k++] = (d[i + j] >= 32 && d[i + j] < 127) ? (char)d[i + j] : '.';
            } else o << "   ";
            if (j == 7) o << "  ";
        }
        a[k] = 0;
        o << std::dec << " |" << a << "|\n";
    }
    return o.str();
}

// ===================== 应用状态 =====================
struct App {
    // 接收端
    rudp::Receiver recv;
    bool  listening = false;
    bool  gotData = false;
    std::vector<uint8_t> data;
    std::string peerIp;
    char portBuf[16] = "8010";
    double rxLastBytes = 0, rxLastT = 0, rxRateBps = 0;

    // 模拟结果
    unsigned long long rex[4] = {0, 0, 0, 0};
    std::string emuOutbox;
    bool emuRan = false;
    bool emuEnabled = false;

    // 发送端（本机调试用：无需手机即可端到端自测）
    char  sendIp[64] = "127.0.0.1";
    char  sendPortBuf[16] = "8010";
    char  sendSizeBuf[16] = "200000";
    rudp::Sender sender;
    bool  sending = false, sendDone = false;
    int   sendRetx = 0; uint64_t sendTotal = 0;

    std::string log;
    void logAppend(const std::string& s) {
        std::time_t t = std::time(nullptr); std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char b[32]; std::snprintf(b, sizeof(b), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        log += "["; log += b; log += "] "; log += s; log += "\n";
        if (log.size() > 8000) log.erase(0, log.size() - 6000);
    }
};

static void saveToInbox(App& app, const std::vector<uint8_t>& d) {
    std::string dir = "inbox";
    FILE* f = std::fopen((dir + "/" + timeStamp() + ".bin").c_str(), "wb");
    if (!f) { app.logAppend("保存失败（检查 inbox/ 目录是否存在）"); return; }
    std::fwrite(d.data(), 1, d.size(), f);
    std::fclose(f);
    app.logAppend("已保存 " + std::to_string(d.size()) + " 字节 -> inbox/");
}

static void runEmulation(App& app) {
#ifdef USE_UNICORN
    app.emuEnabled = true;
    auto r = emu::runArm64(app.data);
    app.rex[0] = r.x0; app.rex[1] = r.x1; app.rex[2] = r.x2; app.rex[3] = r.x3;
    app.emuOutbox = hexDump(r.outbox);
    if (r.ok) { app.emuRan = true; app.logAppend("Unicorn 模拟完成"); }
    else if (!r.error.empty()) app.logAppend("Unicorn 失败: " + r.error);
#else
    app.emuEnabled = false;
    app.logAppend("Unicorn 未编译（用 -DUSE_UNICORN=ON 重新构建可显示模拟结果）");
#endif
}

// ===================== 面板 =====================
static void panelReceiver(App& app) {
    ImGui::Begin("电脑接收端 · Unicorn 模拟");
    app.listening = app.recv.listening();
    ImGui::Text("监听端口:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputText("##port", app.portBuf, sizeof(app.portBuf));

    if (!app.listening) {
        if (ImGui::Button("启动监听")) {
            unsigned p = (unsigned)std::atoi(app.portBuf);
            if (p > 0 && app.recv.start((uint16_t)p)) {
                app.gotData = false; app.data.clear(); app.emuRan = false;
                app.rxLastBytes = 0; app.rxLastT = 0; app.rxRateBps = 0;
                app.logAppend("开始监听 UDP :" + std::string(app.portBuf));
            } else app.logAppend("监听失败（端口被占用?）");
        }
        ImGui::Text("状态：未监听");
    } else {
        if (ImGui::Button("停止监听")) {
            app.recv.stop();
            app.logAppend("已停止");
        }
        ImGui::Text("状态：正在监听 UDP :%s", app.portBuf);
    }

    // 接收进度
    ImGui::Separator();
    if (app.listening) {
        uint64_t recv = app.recv.boundary() * rudp::kMaxChunk;
        uint64_t total = app.recv.totalBytes();
        double t = nowSec();
        if (app.rxLastT == 0) { app.rxLastT = t; app.rxLastBytes = (double)recv; }
        double dt = t - app.rxLastT;
        if (dt > 0.2) { app.rxRateBps = (recv - app.rxLastBytes) / dt; app.rxLastT = t; app.rxLastBytes = (double)recv; }
        ImGui::Text("对端: %s", app.peerIp.empty() ? "-" : app.peerIp.c_str());
        ImGui::Text("已收: %llu / %llu 字节",
                    (unsigned long long)recv, (unsigned long long)total);
        ImGui::Text("速率: %.1f KB/s", app.rxRateBps / 1024.0);
        if (total > 0) {
            float f = (float)((double)recv / (double)total);
            if (f > 1.0f) f = 1.0f;
            const char* label = f < 1.0f ? "正在接收..." : "接收完成，正在模拟";
            ImGui::ProgressBar(f, ImVec2(-1, 0), label);
        }
    }

    // 每帧驱动收包
    if (app.listening) {
        std::vector<uint8_t> got; std::string from;
        if (app.recv.poll(got, from)) {
            app.data = std::move(got);
            app.peerIp = from;
            app.gotData = true;
            app.logAppend("收到完整数据 " + std::to_string(app.data.size()) + " 字节，来自 " + from);
            saveToInbox(app, app.data);
            runEmulation(app);
        }
    }

    // 模拟结果
    ImGui::Separator();
    ImGui::TextUnformatted("寄存器快照:");
    if (ImGui::BeginTable("regs", 4)) {
        ImGui::TableSetupColumn("x0"); ImGui::TableSetupColumn("x1");
        ImGui::TableSetupColumn("x2"); ImGui::TableSetupColumn("x3");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (int c = 0; c < 4; ++c) {
            ImGui::TableNextColumn();
            if (app.emuEnabled)
                ImGui::Text("0x%016llx", (unsigned long long)app.rex[c]);
            else
                ImGui::TextUnformatted("-");
        }
        ImGui::EndTable();
    }
    ImGui::TextUnformatted("结果内存 (outbox @0x10000):");
    ImGui::SetNextItemWidth(-1);
    std::vector<char> obuf(app.emuOutbox.begin(), app.emuOutbox.end());
    obuf.push_back('\0');
    ImGui::InputTextMultiline("##out", obuf.data(), obuf.size(), ImVec2(0, 190),
                              ImGuiInputTextFlags_ReadOnly);

    ImGui::Separator();
    ImGui::TextUnformatted("日志:");
    ImGui::SetNextItemWidth(-1);
    std::vector<char> lbuf(app.log.begin(), app.log.end());
    lbuf.push_back('\0');
    ImGui::InputTextMultiline("##log", lbuf.data(), lbuf.size(), ImVec2(0, 130),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::End();
}

// 发送端面板（本机自测用；正式场景由手机 App 发送）
static void panelSender(App& app) {
    ImGui::Begin("本机发送端·自测");
    ImGui::TextWrapped("随机数据发给上面的监听端口，验证整条链路。");
    ImGui::SetNextItemWidth(150); ImGui::InputText("目标 IP", app.sendIp, sizeof(app.sendIp));
    ImGui::SetNextItemWidth(110); ImGui::InputText("端口", app.sendPortBuf, sizeof(app.sendPortBuf));
    ImGui::SetNextItemWidth(110); ImGui::InputText("字节数", app.sendSizeBuf, sizeof(app.sendSizeBuf));
    if (!app.sending && ImGui::Button("发送")) {
        size_t n = (size_t)std::atoi(app.sendSizeBuf);
        std::vector<uint8_t> blob(n);
        std::mt19937 rng(7); for (auto& b : blob) b = (uint8_t)(rng() & 0xFF);
        unsigned p = (unsigned)std::atoi(app.sendPortBuf);
        app.sender.start(app.sendIp, (uint16_t)p, blob);
        app.sendTotal = blob.size(); app.sending = true; app.sendDone = false; app.sendRetx = 0;
        app.logAppend("自测发送 " + std::to_string(n) + " 字节 -> " +
                      std::string(app.sendIp) + ":" + std::string(app.sendPortBuf));
    }
    if (app.sending) {
        app.sendDone = app.sender.pump();
        app.sendRetx = app.sender.retransmits();
        if (app.sendDone) { app.sending = false; app.logAppend("发送确认完成，重传 " + std::to_string(app.sendRetx) + " 次"); }
        uint64_t acked = app.sender.ackedBytes();
        if (app.sendTotal > 0) {
            float f = (float)((double)acked / app.sendTotal);
            if (f > 1.0f) f = 1.0f;
            ImGui::ProgressBar(f, ImVec2(-1, 0), f < 1.0f ? "发送中..." : "完成");
        }
        ImGui::Text("已确认 %llu / %llu   重传 %d",
                    (unsigned long long)acked, (unsigned long long)app.sendTotal, app.sendRetx);
        if (ImGui::Button("取消")) { app.sender.reset(); app.sending = false; }
    }
    ImGui::End();
}

// ===================== 主入口 =====================
int main() {
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* win = glfwCreateWindow(1000, 740, "手机->电脑 Unicorn 模拟接收端", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "window create failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 150");
    ImGui::StyleColorsDark();

    App app;
    app.logAppend("程序启动。让手机发送端指向本机 IP，可先发 examples/blob.bin 试运行。");

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(600, 470), ImGuiCond_FirstUseEver);
        panelReceiver(app);

        ImGui::SetNextWindowPos(ImVec2(630, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(340, 230), ImGuiCond_FirstUseEver);
        panelSender(app);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}