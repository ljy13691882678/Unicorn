// desktop/main.cpp - 电脑端标准控件接收程序（原生 Win32，去 ImGui）
// 核心工作：可靠 UDP 接收手机数据 -> 显示/保存 -> 可选 Unicorn 模拟出寄存器+结果内存。
// 控件：复选框（自动保存/自动模拟）、输入框（端口/IP/大小）、显示框（日志/结果）。
// 仅 Windows。CI（windows-latest + MSVC/mingw）可直接编译，无第三方 GUI 依赖。

#ifndef _WIN32
#error "该程序使用原生 Win32 控件，仅支持 Windows。"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include "reliable_udp.hpp"
#include "emu.hpp"

#include <cstdio>
#include <cstdint>
#include <cctype>
#include <cwchar>
#include <string>
#include <vector>
#include <ctime>
#include <chrono>

// ===================== 控件 ID =====================
enum {
    IDC_BTN_START  = 1001,
    IDC_BTN_STOP   = 1002,
    IDC_BTN_SAVE   = 1003,
    IDC_BTN_EMU    = 1004,
    IDC_EDT_PORT   = 1101,
    IDC_EDT_SIP    = 1102,
    IDC_EDT_SPORT  = 1103,
    IDC_EDT_SSIZE  = 1104,
    IDC_CHK_SAVE   = 1201,
    IDC_CHK_EMU    = 1202,
    IDC_CHK_SEND   = 1203,
    IDC_MEM_VIEW   = 1301,
    IDC_LOG_VIEW   = 1302,
    IDC_BTN_SEND   = 1303,
    IDC_STAT_RX    = 1401,
};

// ===================== 全局状态 =====================
struct App {
    HWND hwnd = nullptr;
    HWND statRx = nullptr, memView = nullptr, logView = nullptr;
    HWND edtPort = nullptr, edtByte = nullptr;
    HWND chkSave = nullptr, chkEmu = nullptr;

    rudp::Receiver recv;
    bool  listening = false;
    std::vector<uint8_t> data;
    std::string peerIp;
    bool autoSave = false;
    bool autoEmu  = false;

    // 本机发送自测
    rudp::Sender sender;
    bool sending = false;

    unsigned long long rex[4] = {0,0,0,0};
    std::string emuOutbox;
    bool emuRan = false;
    int  emuErr = 0;

    std::string log;
    std::string mem;
} app;

static std::string nowTime() {
    std::time_t t = std::time(nullptr); std::tm tm{};
    localtime_s(&tm, &t);
    char b[40]; std::snprintf(b, sizeof(b), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}
static std::string nowStamp() {
    std::time_t t = std::time(nullptr); std::tm tm{};
    localtime_s(&tm, &t);
    char b[40]; std::snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d",
        1900+tm.tm_year, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

static std::wstring toW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string hexDump(const std::vector<uint8_t>& d, size_t maxRows = 24) {
    std::string o;
    char line[128];
    for (size_t i = 0; i < d.size() && i / 16 < maxRows; i += 16) {
        char a[17]; size_t k = 0;
        std::snprintf(line, sizeof(line), "0x%08zx:  ", i); o += line;
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < d.size()) {
                std::snprintf(line, sizeof(line), "%02x ", (int)d[i+j]); o += line;
                a[k++] = (d[i+j]>=32 && d[i+j]<127) ? (char)d[i+j] : '.';
            } else { o += "   "; }
            if (j == 7) o += " ";
        }
        a[k] = 0; o += " |"; o += a; o += "|\r\n";
    }
    return o;
}

static void setW(HWND c, const std::wstring& s) {
    if (c) SetWindowTextW(c, s.c_str());
}
static std::wstring getW(HWND c) {
    wchar_t buf[512]; buf[0] = 0;
    if (c) GetWindowTextW(c, buf, 511);
    return buf;
}
static std::string toW2s(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 追加日志（只读显示框）
static void logAdd(const std::string& s) {
    app.log += "[" + nowTime() + "] " + s + "\r\n";
    if (app.log.size() > 30000) app.log.erase(0, app.log.size() - 24000);
    setW(app.logView, toW(app.log));
    if (app.logView) SendMessageW(app.logView, EM_SETSEL, (WPARAM)app.log.size(), (LPARAM)app.log.size());
}

static void refreshMem() {
    SetWindowTextW(app.memView, toW(app.mem).c_str());
}

static void saveData(const std::vector<uint8_t>& d) {
    // 确保目录存在
    CreateDirectoryW(L"inbox", nullptr);
    std::string path = "inbox/" + nowStamp() + ".bin";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { logAdd("保存失败: " + path); return; }
    std::fwrite(d.data(), 1, d.size(), f);
    std::fclose(f);
    logAdd("已保存 " + std::to_string(d.size()) + " 字节 -> " + path);
}

static void doEmulation() {
#ifdef USE_UNICORN
    if (app.data.empty()) { logAdd("没有数据可模拟。"); return; }
    auto r = emu::runArm64(app.data);
    app.rex[0]=r.x0; app.rex[1]=r.x1; app.rex[2]=r.x2; app.rex[3]=r.x3;
    if (r.ok) {
        app.emuRan = true; app.emuErr = 0;
        char b[256];
        std::snprintf(b, sizeof(b),
            "== Unicorn 模拟结果 (ARM64) ==\r\nx0 = 0x%016llx\r\nx1 = 0x%016llx\r\nx2 = 0x%016llx\r\nx3 = 0x%016llx\r\n\r\n结果内存(0x10000):\r\n%s\r\n",
            r.x0, r.x1, r.x2, r.x3, hexDump(r.outbox).c_str());
        app.mem = b;
        logAdd("Unicorn 模拟完成: x0=" + std::to_string(r.x0));
    } else {
        app.emuErr = 1;
        app.mem = "Unicorn 模拟失败: " + r.error + "\r\n";
        logAdd("Unicorn 模拟失败: " + r.error);
    }
    refreshMem();
#else
    app.mem = "未以 -DUSE_UNICORN=ON 编译，Unicorn 模拟不可用。\r\n";
    refreshMem();
    logAdd("本版未启用 Unicorn（需 -DUSE_UNICORN=ON 重新构建）。");
#endif
}

// 收包完成后的统一处理
static void onDataComplete() {
    if (app.autoSave) saveData(app.data);
    if (app.autoEmu)  doEmulation();
}

// ===================== 窗口过程 =====================
static void doStartListen() {
    if (app.listening) return;
    std::wstring w = getW(app.edtPort);
    uint16_t port = (uint16_t)std::wcstoul(w.c_str(), nullptr, 10);
    if (port == 0) { logAdd("请输入有效监听端口。"); return; }
    if (!app.recv.start(port)) {
        logAdd("监听 " + std::to_string(port) + " 失败（端口被占用？）。");
        return;
    }
    app.listening = true;
    logAdd("已开始监听 UDP " + std::to_string(port) + "，等待手机发送...");
}

static void doStopListen() {
    if (!app.listening) return;
    app.recv.stop();
    app.listening = false;
    logAdd("已停止监听。");
}

// 把输入框里空格分隔的 hex 字符串解析成字节
static std::vector<uint8_t> parseHex(const std::string& s) {
    std::vector<uint8_t> out;
    std::string hex;
    for (char c : s) if (!std::isspace((unsigned char)c)) hex += c;
    if (hex.size() % 2) return out;
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            char c = hex[i + k]; v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return {};
        }
        out.push_back((uint8_t)v);
    }
    return out;
}

// 本机自测：把输入框里的 hex 字节发给本机监听端口（可端到端验证）。
static void doSendSelf() {
    if (app.sending) { logAdd("正在发送，请稍候。"); return; }
    std::string hex = toW2s(getW(app.edtByte));
    std::vector<uint8_t> d = parseHex(hex);
    if (d.empty()) { logAdd("输入框不是合法的 hex 字节串。"); return; }
    std::wstring wp = getW(app.edtPort);
    uint16_t port = (uint16_t)std::wcstoul(wp.c_str(), nullptr, 10);
    if (port == 0) port = 8010;
    app.sender.reset();
    app.sender.start("127.0.0.1", port, d);
    app.sending = true;
    logAdd("本机自测：向 127.0.0.1:" + std::to_string(port) + " 发送 " + std::to_string(d.size()) + " 字节");
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        int id = LOWORD(w);
        if (id == IDC_BTN_START) { doStartListen(); return 0; }
        if (id == IDC_BTN_STOP)  { doStopListen();  return 0; }
        if (id == IDC_BTN_SEND)  { doSendSelf();    return 0; }
        // 自动保存 / 自动模拟 勾选框
        if (id == IDC_CHK_SAVE) {
            app.autoSave = (SendMessageW(app.chkSave, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        }
        if (id == IDC_CHK_EMU) {
            app.autoEmu = (SendMessageW(app.chkEmu, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (app.autoEmu) doEmulation();
            return 0;
        }
    }
    if (m == WM_TIMER && w == 1) {
        // 驱动接收
        if (app.listening) {
            std::vector<uint8_t> out; std::string peer;
            if (app.recv.poll(out, peer)) {
                app.data = out; app.peerIp = peer;
                app.mem = "== 收到 " + std::to_string(out.size()) + " 字节 (来自 " + peer + ") ==\r\n" + hexDump(out);
                refreshMem();
                logAdd("收到完整数据 " + std::to_string(out.size()) + " 字节（" + peer + "）。");
                onDataComplete();
            }
        }
        // 驱动发送自测
        if (app.sending) {
            bool done = app.sender.pump();
            if (done) { app.sending = false; logAdd("自测发送完成。"); }
        }
        // 更新状态栏
        if (app.statRx) {
            std::string s = app.listening
                ? ("监听中 | 已收 " + std::to_string(app.recv.receivedBytes()) + " 字节")
                : "未监听 | 可靠UDP电脑端";
            setW(app.statRx, toW(s));
        }
        return 0;
    }
    if (m == WM_CLOSE) {
        if (app.sending) app.sender.reset();
        if (app.listening) app.recv.stop();
        DestroyWindow(h);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

// ===================== 创建控件 =====================
static HWND mk(HWND parent, LPCWSTR cls, const wchar_t* text, int id,
               int x, int y, int wdt, int ht, DWORD style) {
    return CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                         x, y, wdt, ht, parent, (HMENU)(INT_PTR)id,
                         GetModuleHandleW(nullptr), nullptr);
}

static void buildUI(HWND h) {
    int y = 12;
    mk(h, L"STATIC", L"监听端口(UDP)：", 0, 12, y, 110, 20, SS_LEFT);
    app.edtPort = mk(h, L"EDIT", L"8010", IDC_EDT_PORT, 130, y-2, 90, 22, ES_AUTOHSCROLL);
    HWND btnStart = mk(h, L"BUTTON", L"开始监听", IDC_BTN_START, 230, y-2, 90, 26, BS_PUSHBUTTON);
    HWND btnStop  = mk(h, L"BUTTON", L"停止",    IDC_BTN_STOP,  325, y-2, 60, 26, BS_PUSHBUTTON);
    (void)btnStart; (void)btnStop;

    y += 34;
    app.chkSave = mk(h, L"BUTTON", L"自动保存到 inbox/", IDC_CHK_SAVE, 12, y, 150, 24, BS_AUTOCHECKBOX);
    app.chkEmu  = mk(h, L"BUTTON", L"自动 Unicorn 模拟", IDC_CHK_EMU,  170, y, 160, 24, BS_AUTOCHECKBOX);

    y += 30;
    mk(h, L"STATIC", L"数据字节(自测,hex)：", 0, 12, y, 130, 20, SS_LEFT);
    app.edtByte = mk(h, L"EDIT", L"1c0091007c009b2100a0d2200000f9c0035fd6", IDC_EDT_SSIZE, 150, y-2, 300, 22, ES_AUTOHSCROLL);
    HWND btnSend = mk(h, L"BUTTON", L"本机自测发送", IDC_BTN_SEND, 460, y-2, 110, 26, BS_PUSHBUTTON);
    (void)btnSend;

    y += 30;
    mk(h, L"STATIC", L"内存 / 模拟结果显示框：", 0, 12, y, 200, 20, SS_LEFT);
    y += 20;
    app.memView = mk(h, L"EDIT", L"", IDC_MEM_VIEW, 12, y, 560, 180,
                     ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | ES_AUTOHSCROLL);

    y += 190;
    mk(h, L"STATIC", L"日志显示框：", 0, 12, y, 200, 20, SS_LEFT);
    y += 20;
    app.logView = mk(h, L"EDIT", L"", IDC_LOG_VIEW, 12, y, 560, 150,
                     ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);

    y += 160;
    app.statRx = mk(h, L"STATIC", L"未监听", IDC_STAT_RX, 12, y, 560, 18, SS_LEFT);
}

static void initFonts(HWND h) {
    // 让显示框用等宽字体
    HFONT monof = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (app.memView) SendMessageW(app.memView, WM_SETFONT, (WPARAM)monof, TRUE);
    if (app.logView) SendMessageW(app.logView, WM_SETFONT, (WPARAM)monof, TRUE);
}

// ===================== 入口 =====================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    InitCommonControls();

    const wchar_t* cls = L"ReliableUdpDesk";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    HWND h = CreateWindowW(cls, L"可靠UDP 电脑端（接收 + Unicorn 模拟）",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 600, 500,
                           nullptr, nullptr, hInst, nullptr);
    if (!h) return 1;

    app.hwnd = h;
    buildUI(h);
    initFonts(h);

    // 固定可编辑字体大小一致
    HFONT uif = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(app.edtPort, WM_SETFONT, (WPARAM)uif, TRUE);
    SendMessageW(app.edtByte, WM_SETFONT, (WPARAM)uif, TRUE);

    ShowWindow(h, nShow);
    UpdateWindow(h);

    SetTimer(h, 1, 60, nullptr);   // 驱动收发

    logAdd("就绪。手机端输入本机 IP + 端口(默认8010) 即可发送。");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}