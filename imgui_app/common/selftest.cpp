// selftest.cpp - 无界面自测：可靠 UDP 收发 + 完整性与（可选）Unicorn 模拟。
// 构建：g++ common/*.cpp selftest.cpp -std=c++17 -o selftest
// 丢包：RUDP_DROP=0.3  ./selftest
#include "reliable_udp.hpp"
#include "emu.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <chrono>
#include <thread>
#include <vector>

using namespace rudp;

static std::vector<uint8_t> randomBytes(size_t n) {
    std::vector<uint8_t> v(n);
    std::mt19937 rng(12345);
    for (auto& b : v) b = (uint8_t)(rng() & 0xFF);
    return v;
}

int main(int argc, char** argv) {
    size_t size = argc > 1 ? (size_t)atoi(argv[1]) : 200000;

    // 1) 可靠 UDP 端到端（回环）
    Receiver recv;
    uint16_t port = 29000 + (int)(std::random_device{}() % 1000);
    if (!recv.start(port)) { std::fprintf(stderr, "recv.start failed\n"); return 1; }

    std::vector<uint8_t> blob = randomBytes(size);
    Sender send;
    send.start("127.0.0.1", port, blob);

    std::fprintf(stderr, "sending %zu bytes on udp %u (drop=gDrop)\n", size, port);
    auto t0 = std::chrono::steady_clock::now();
    bool done = false;
    std::vector<uint8_t> got;
    // 墙钟驱动：给重传留出真实时间
    for (int i = 0; i < 200000; ++i) {
        std::string from;
        if (recv.poll(got, from)) break;
        if (send.pump()) done = true;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > 15.0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto t1 = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(t1 - t0).count();

    if (got.empty()) { size_t guard=0; while (got.empty() && guard++<200000) { std::string f; recv.poll(got, f); } }
    std::fprintf(stderr, "[dbg] recv.boundary=%u/%u  sender.acked=%llu/%llu retx=%d\n",
                 recv.boundary(), recv.totalChunks(),
                 (unsigned long long)send.ackedBytes(), (unsigned long long)send.totalBytes(),
                 send.retransmits());
    if (got == blob) {
        std::printf("UDPLOOPBACK OK  bytes=%zu retx=%d dt=%.3fs\n",
                    size, send.retransmits(), dt);
    } else {
        std::printf("UDPLOOPBACK FAILED got=%zu want=%zu\n", got.size(), blob.size());
        return 1;
    }

#ifdef USE_UNICORN
    // 2) Unicorn 模拟示例：add/mul 后写 0x10000
    std::vector<uint8_t> code = {0x00,0x1c,0x00,0x91, 0x00,0x7c,0x00,0x9b,
                                 0x21,0x00,0xa0,0xd2, 0x20,0x00,0x00,0xf9,
                                 0xc0,0x03,0x5f,0xd6};
    auto r = emu::runArm64(code);
    if (r.ok && r.x0 == 0x31 && r.outbox[0] == 0x31)
        std::printf("EMU ARM64 OK  x0=0x%llx out[0]=0x%02x\n", r.x0, r.outbox[0]);
    else { std::printf("EMU FAILED err=%s  (x0=%llx out[0]=%02x)\n",
                       r.error.c_str(), r.x0, r.outbox.empty()?0:r.outbox[0]); return 1; }
#else
    std::puts("(Unicorn 未启用，跳过模拟自检)");
#endif
    return 0;
}