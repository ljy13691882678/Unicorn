// reliable_udp.hpp - 跨平台可靠 UDP（手机=发送端 / 电脑=接收端共用核心）。
// 只依赖标准库 + 原始 socket，编译期按平台选择 Winsock 或 POSIX。
// 应用每帧调用 pump()/poll() 驱动收发（ImGui 主循环友好，无阻塞）。
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rudp {

constexpr uint32_t kMaxChunk = 1200;   // 每块负载字节数（避免 IP 分片）
constexpr uint32_t kWindow   = 128;    // 发送窗口（在途块数）
constexpr double   kRto      = 0.15;   // 重传超时（秒）

constexpr uint8_t  kTypeData = 1;
constexpr uint8_t  kTypeFin  = 2;
constexpr uint8_t  kTypeAck  = 3;

// ---- 平台 socket 抽象（Winsock / POSIX） ----
struct SocketImpl;

struct Endpoint {
    std::string ip;
    uint16_t port = 0;
};

class Socket {
public:
    Socket();
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // 返回 false 表示失败（WSAStartup/socket/bind）。
    bool openReceiver(uint16_t bindPort);
    bool openSender();
    // 非阻塞读：有数据返回 true 并填充对端 + payload。
    bool recvFrom(Endpoint& peer, std::string& payload);
    bool sendTo(const std::string& peerHost, uint16_t peerPort,
                const uint8_t* data, size_t len);
    void close();

private:
    SocketImpl* impl_;
};

// ---- 数据包编解码（与 Python 版一致的线路格式） ----
struct Packet {
    uint8_t  type = 0;
    uint64_t total = 0;
    uint32_t seq = 0;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> packData(uint64_t total, uint32_t seq, const uint8_t* p, size_t n);
std::vector<uint8_t> packAck(uint64_t total, uint32_t boundary);
std::vector<uint8_t> packFin(uint64_t total, uint32_t nchunks);
bool unpack(const uint8_t* buf, size_t len, Packet& out);

// ---- 发送端（手机） ----
class Sender {
public:
    // 设定目标与待发数据（拷贝入内存）。
    void start(const std::string& host, uint16_t port, const std::vector<uint8_t>& data);

    // 每帧调用。返回 true 表示全部转发确认（可结束）。
    bool pump();

    // 取消/复位发送（关闭 socket 并清空状态；下次 start() 可重新使用）。
    void reset();

    // 统计
    uint64_t ackedBytes() const;
    uint64_t totalBytes() const;
    int      retransmits() const;
    double   bytesPerSec() const;
    bool     started() const { return nchunks_ > 0; }

private:
    Socket sock_;
    Endpoint target_;
    std::vector<uint8_t> data_;
    std::vector<std::vector<uint8_t>> chunks_;
    uint32_t nchunks_ = 0;
    uint32_t boundary_ = 0;               // 已确认连续块数
    std::vector<double> sentAt_;          // 每块上次发送时间（-1=未发,-2=已确认）
    int64_t  retx_ = 0;
    double   startTime_ = 0;
    bool     finSent_ = false;
    bool     finAcked_ = false;
    double   finTime_ = 0;
    void pumpAcks();
};

// ---- 接收端（电脑） ----
class Receiver {
public:
    // 绑定端口开始监听。
    bool start(uint16_t bindPort);

    // 关闭监听（关闭 socket 并复位状态）。
    void stop();

    // 每帧调用，驱动收包/重组。返回 true 表示收到完整数据。
    bool poll(std::vector<uint8_t>& outData, std::string& fromPeer);

    uint64_t receivedBytes() const { return boundary_ * kMaxChunk; }
    uint32_t boundary() const { return boundary_; }
    uint32_t totalChunks() const { return nchunks_; }
    uint64_t totalBytes() const { return totalBytes_; }
    bool listening() const { return listening_; }

private:
    Socket sock_;
    bool listening_ = false;
    Endpoint peer_;
    uint64_t totalBytes_ = 0;
    uint32_t nchunks_ = 0;
    uint32_t boundary_ = 0;
    std::vector<std::vector<uint8_t>> buf_;   // seq -> payload
    void advance(Endpoint peer);
};

} // namespace rudp