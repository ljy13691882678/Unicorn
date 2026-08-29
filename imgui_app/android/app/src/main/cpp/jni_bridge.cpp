// jni_bridge.cpp - 手机端 JNI 桥接层（Java 主界面 <-> C++ 底层）
// 通过内核驱动 TimeDriver 读取应用内存（比 root 直接读更隐蔽，不易被反调试发现），
// 再用可靠 UDP 发送，复用 common/reliable_udp.cpp 核心。
#include <jni.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <sstream>

#include "reliable_udp.hpp"
#include "time_driver.h"

// ---- root 执行辅助：popen 走 su -c，读取完整输出 ----
static std::vector<uint8_t> runRootBytes(const std::string& cmd) {
    std::vector<uint8_t> out;
    std::string full = "su -c \"" + cmd + "\" 2>/dev/null";
    FILE* fp = popen(full.c_str(), "r");
    if (!fp) return out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        out.insert(out.end(), buf, buf + n);
    pclose(fp);
    return out;
}

static std::string runRootText(const std::string& cmd) {
    auto b = runRootBytes(cmd);
    return std::string(b.begin(), b.end());
}

// ---- 内核驱动 TimeDriver：懒初始化 + 访问守卫 ----
// TIME_Driver 为驱动库提供的全局指针；Init() 会加载/连接内核驱动。
// 读取失败返回 false，避免把失败误当"读到了0字节"。
static std::mutex gDrvMtx;
static bool gDrvInit = false;
static bool gDrvOk   = false;

static bool ensureDriver() {
    std::lock_guard<std::mutex> lk(gDrvMtx);
    if (gDrvInit) return gDrvOk;
    gDrvInit = true;
    if (!TIME_Driver) { gDrvOk = false; return false; }
    gDrvOk = TIME_Driver->Init();
    return gDrvOk;
}

// 通过内核驱动读取进程内存。需驱动已加载（手机已 root 且安装内核模块）。
static std::vector<uint8_t> driverReadMem(pid_t pid, uintptr_t addr, size_t size) {
    std::vector<uint8_t> out;
    if (!ensureDriver()) return out;                 // 驱动不可用
    out.resize(size ? size : 1);
    if (!TIME_Driver->Read_Memory(pid, addr, out.data(), size)) {
        out.clear();
        return out;
    }
    if (size == 0) out.clear();
    return out;
}

// ---- 可靠 UDP 发送（全局单例，Java 后台线程驱动 pump）----
static std::mutex gSockMtx;
static rudp::Sender gSender;
static volatile bool gSending = false;

extern "C" {

// 内核驱动链接状态：返回 true = Init 成功（驱动已连接）；false = 链接失败。
// Java 侧用于启动时提醒"驱动链接失败 / 成功"。
JNIEXPORT jboolean JNICALL
Java_com_example_phoneudp_MainActivity_nativeDriverLink(JNIEnv* env, jobject) {
    return ensureDriver() ? JNI_TRUE : JNI_FALSE;
}

// 读取进程内存：始终只走内核驱动 TimeDriver（单一内核读取模式）。
// 驱动未链接或读取失败时返回 null，Java 侧提示链接/读取失败，不回退 root 读取。
JNIEXPORT jbyteArray JNICALL
Java_com_example_phoneudp_MainActivity_nativeRootReadMem(JNIEnv* env, jobject,
                                                         jint pid, jlong addr, jlong len) {
    if (!ensureDriver() || len <= 0)
        return nullptr;                              // 驱动未链接，直接失败
    std::vector<uint8_t> bytes = driverReadMem((pid_t)pid, (uintptr_t)addr, (size_t)len);
    if (bytes.empty())
        return nullptr;                              // 读取失败
    jbyteArray arr = env->NewByteArray((jsize)bytes.size());
    if (arr)
        env->SetByteArrayRegion(arr, 0, (jsize)bytes.size(), (const jbyte*)bytes.data());
    return arr;
}

// 执行任意 root 命令，返回输出文本
JNIEXPORT jstring JNICALL
Java_com_example_phoneudp_MainActivity_nativeRootExec(JNIEnv* env, jobject,
                                                      jstring jcmd) {
    const char* c = env->GetStringUTFChars(jcmd, nullptr);
    std::string cmd = c ? c : "";
    if (c) env->ReleaseStringUTFChars(jcmd, c);
    std::string out = runRootText(cmd);
    return env->NewStringUTF(out.c_str());
}

// 启动可靠 UDP 发送（非阻塞，需循环调用 pump）
JNIEXPORT jboolean JNICALL
Java_com_example_phoneudp_MainActivity_nativeSendStart(JNIEnv* env, jobject,
                                                       jstring jip, jint port, jbyteArray data) {
    const char* c = env->GetStringUTFChars(jip, nullptr);
    std::string ip = c ? c : "";
    if (c) env->ReleaseStringUTFChars(jip, c);
    jsize sz = env->GetArrayLength(data);
    std::vector<uint8_t> d((size_t)sz);
    if (sz > 0) env->GetByteArrayRegion(data, 0, sz, (jbyte*)d.data());

    std::lock_guard<std::mutex> lk(gSockMtx);
    gSender.reset();
    gSender.start(ip, (uint16_t)port, d);
    gSending = true;
    return JNI_TRUE;
}

// 驱动发送，返回是否完成
JNIEXPORT jboolean JNICALL
Java_com_example_phoneudp_MainActivity_nativeSendPump(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lk(gSockMtx);
    if (!gSending) return JNI_FALSE;
    bool done = gSender.pump();
    if (done) gSending = false;
    return done ? JNI_TRUE : JNI_FALSE;
}

// 已确认字节数
JNIEXPORT jlong JNICALL
Java_com_example_phoneudp_MainActivity_nativeSendAcked(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lk(gSockMtx);
    return (jlong)gSender.ackedBytes();
}

} // extern "C"