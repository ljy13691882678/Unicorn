// jni_bridge.cpp - 手机端 JNI 桥接层（Java 主界面 <-> C++ 底层）
// 通过 root 读取应用内存 + 用可靠 UDP 发送，复用 common/reliable_udp.cpp 核心。
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

// ---- 可靠 UDP 发送（全局单例，Java 后台线程驱动 pump）----
static std::mutex gSockMtx;
static rudp::Sender gSender;
static volatile bool gSending = false;

extern "C" {

// 读取进程内存：用 root 运行 dd if=/proc/PID/mem，可指定偏移/长度
JNIEXPORT jbyteArray JNICALL
Java_com_example_phoneudp_MainActivity_nativeRootReadMem(JNIEnv* env, jobject,
                                                         jint pid, jlong addr, jlong len) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "dd if=/proc/%d/mem bs=1 skip=%lld count=%lld status=none",
             (int)pid, (long long)addr, (long long)len);
    auto bytes = runRootBytes(cmd);
    jbyteArray arr = env->NewByteArray((jsize)bytes.size());
    if (arr && !bytes.empty())
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