// reliable_udp.cpp - 实现见 reliable_udp.hpp
#include "reliable_udp.hpp"

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <random>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <io.h>
  #pragma comment(lib, "ws2_32.lib")
  using ssize_t = int;
#else
  #include <arpa/inet.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <cstring>
  typedef int SOCKET;
  constexpr SOCKET INVALID_SOCKET = -1;
  #define SOCKET_ERROR (-1)
  #define closesocket(fd) ::close(fd)
#endif

namespace rudp {

namespace {
double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct WsaOnce { WsaOnce() {
#ifdef _WIN32
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
#endif
} };
WsaOnce g_wsa;

// 测试用：RUDP_DROP=0.3 表示 30% 收包丢包（验证可靠性）。默认关闭。
double gDrop = []() {
    const char* e = std::getenv("RUDP_DROP");
    return e ? std::atof(e) : 0.0;
}();

void setNonBlocking(SOCKET s) {
#ifdef _WIN32
    u_long mode = 1; ioctlsocket(s, FIONBIO, &mode);
#else
    int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}
} // namespace

// ===================== Socket =====================
struct SocketImpl { SOCKET fd = INVALID_SOCKET; };

Socket::Socket() : impl_(new SocketImpl()) {}
Socket::~Socket() { close(); }

bool Socket::openReceiver(uint16_t bindPort) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return false;
    int rbuf = 8 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rbuf, sizeof(rbuf));
    sockaddr_in a; std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(bindPort); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) { closesocket(s); return false; }
    setNonBlocking(s);
    impl_->fd = s;
    return true;
}

bool Socket::openSender() {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return false;
    int b = 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&b, sizeof(b));
    setNonBlocking(s);
    impl_->fd = s;
    return true;
}

void Socket::close() {
    if (impl_->fd != INVALID_SOCKET) { closesocket(impl_->fd); impl_->fd = INVALID_SOCKET; }
}

bool Socket::recvFrom(Endpoint& peer, std::string& payload) {
    char cb[65536];
    sockaddr_in an; socklen_t alen = sizeof(an);
#ifdef _WIN32
    int alenIn = alen;
    int r = ::recvfrom(impl_->fd, cb, (int)sizeof(cb), 0, (sockaddr*)&an, &alenIn);
#else
    ssize_t r = ::recvfrom(impl_->fd, cb, sizeof(cb), 0, (sockaddr*)&an, &alen);
#endif
    if (r <= 0) return false;
    if (gDrop > 0.0) {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> d(0, 1);
        if (d(rng) < gDrop) return false; // 模拟丢包
    }
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &an.sin_addr, ip, sizeof(ip));
    peer.ip = ip;
    peer.port = ntohs(an.sin_port);
    payload.assign(cb, (size_t)r);
    return true;
}

bool Socket::sendTo(const std::string& peerHost, uint16_t peerPort,
                    const uint8_t* data, size_t len) {
    sockaddr_in a; std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(peerPort);
    inet_pton(AF_INET, peerHost.c_str(), &a.sin_addr);
    int r = ::sendto(impl_->fd, (const char*)data, (int)len, 0, (sockaddr*)&a, sizeof(a));
    return r == (int)len;
}

// ===================== 包编解码（与 Python 版一致） =====================
namespace {
void wr32(std::vector<uint8_t>& o, uint32_t v) {
    o.insert(o.end(), {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
                       (uint8_t)(v >> 8), (uint8_t)v});
}
void wr64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 7; i >= 0; --i) o.push_back((uint8_t)(v >> (i * 8)));
}
uint64_t rd64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p[i]; return v; }
uint32_t rd32(const uint8_t* p) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v = (v << 8) | p[i]; return v; }
} // namespace

std::vector<uint8_t> packData(uint64_t total, uint32_t seq, const uint8_t* p, size_t n) {
    std::vector<uint8_t> o; o.reserve(19 + n);
    o.push_back('R'); o.push_back('U'); o.push_back(kTypeData);
    wr64(o, total); wr32(o, seq); wr32(o, (uint32_t)n);
    o.insert(o.end(), p, p + n);
    return o;
}
std::vector<uint8_t> packAck(uint64_t total, uint32_t boundary) {
    std::vector<uint8_t> o = {'R', 'U', kTypeAck};
    wr64(o, total); wr32(o, boundary); wr32(o, 0);
    return o;
}
std::vector<uint8_t> packFin(uint64_t total, uint32_t nchunks) {
    std::vector<uint8_t> o = {'R', 'U', kTypeFin};
    wr64(o, total); wr32(o, nchunks); wr32(o, 0);
    return o;
}
bool unpack(const uint8_t* buf, size_t len, Packet& out) {
    if (len < 19 || buf[0] != 'R' || buf[1] != 'U') return false;
    out.type = buf[2];
    out.total = rd64(buf + 3);
    out.seq = rd32(buf + 11);
    uint32_t dlen = rd32(buf + 15);
    if (19 + dlen > len) dlen = (uint32_t)(len - 19);
    out.payload.assign(buf + 19, buf + 19 + dlen);
    return true;
}

// ===================== Sender =====================
void Sender::reset() {
    sock_.close();
    chunks_.clear();
    data_.clear();
    sentAt_.clear();
    nchunks_ = 0; boundary_ = 0; retx_ = 0;
    finSent_ = false; finAcked_ = false; finTime_ = 0;
}

void Sender::start(const std::string& host, uint16_t port, const std::vector<uint8_t>& data) {
    sock_.openSender();
    target_ = {host, port};
    data_ = data;
    chunks_.clear();
    for (size_t i = 0; i < data_.size(); i += kMaxChunk) {
        size_t n = std::min<size_t>(kMaxChunk, data_.size() - i);
        chunks_.emplace_back(data_.begin() + (long)i, data_.begin() + (long)i + n);
    }
    nchunks_ = (uint32_t)chunks_.size();
    sentAt_.assign(nchunks_, -1.0);
    boundary_ = 0; retx_ = 0; finSent_ = false; finAcked_ = false;
    startTime_ = nowSec();
}

bool Sender::pump() {
    if (nchunks_ == 0) return true;
    double now = nowSec();
    uint32_t wEnd = std::min<uint32_t>(boundary_ + kWindow, nchunks_);
    for (uint32_t seq = boundary_; seq < wEnd; ++seq) {
        bool needSend = sentAt_[seq] < 0 || (now - sentAt_[seq]) >= kRto;
        if (needSend) {
            auto pk = packData(data_.size(), seq, chunks_[seq].data(), chunks_[seq].size());
            sock_.sendTo(target_.ip, target_.port, pk.data(), pk.size());
            if (sentAt_[seq] >= 0) ++retx_;
            sentAt_[seq] = now;
        }
    }
    pumpAcks();
    if (finAcked_) return true;
    if (boundary_ >= nchunks_) {
        if (!finSent_) {
            auto fin = packFin(data_.size(), nchunks_);
            sock_.sendTo(target_.ip, target_.port, fin.data(), fin.size());
            finSent_ = true; finTime_ = now;
        } else if (now - finTime_ >= kRto * 4) {
            return true; // 收尾握手尽力而为
        }
    }
    return false;
}

void Sender::pumpAcks() {
    Endpoint peer; std::string payload; Packet p;
    while (sock_.recvFrom(peer, payload)) {
        if (!unpack((const uint8_t*)payload.data(), payload.size(), p)) continue;
        if (p.type == kTypeAck) {
            if (p.seq == 0xFFFFFFFFu) {
                finAcked_ = true;               // 完成握手确认
            } else if (p.seq > boundary_ && p.seq <= nchunks_) {
                for (uint32_t s = boundary_; s < p.seq && s < (uint32_t)sentAt_.size(); ++s) sentAt_[s] = -2;
                boundary_ = p.seq;
            }
        }
    }
}

uint64_t Sender::ackedBytes() const { return (uint64_t)boundary_ * kMaxChunk; }
uint64_t Sender::totalBytes() const { return data_.size(); }
int Sender::retransmits() const { return retx_; }
double Sender::bytesPerSec() const {
    double el = nowSec() - startTime_;
    return el > 0 ? (double)ackedBytes() / el : 0.0;
}

// ===================== Receiver =====================
bool Receiver::start(uint16_t bindPort) {
    if (!sock_.openReceiver(bindPort)) return false;
    listening_ = true; peer_ = {}; totalBytes_ = 0; nchunks_ = 0; boundary_ = 0; buf_.clear();
    return true;
}

void Receiver::stop() {
    sock_.close();
    listening_ = false; peer_ = {}; totalBytes_ = 0; nchunks_ = 0; boundary_ = 0; buf_.clear();
}

bool Receiver::poll(std::vector<uint8_t>& outData, std::string& fromPeer) {
    Endpoint peer; std::string payload; Packet p;
    while (sock_.recvFrom(peer, payload)) {
        if (!unpack((const uint8_t*)payload.data(), payload.size(), p)) continue;
        if (p.type == kTypeData) {
            if (totalBytes_ == 0) {
                totalBytes_ = p.total;
                nchunks_ = (uint32_t)((totalBytes_ + kMaxChunk - 1) / kMaxChunk);
                buf_.assign(nchunks_, {});
            }
            if (p.seq < nchunks_ && buf_[p.seq].empty())
                buf_[p.seq] = std::move(p.payload);
            advance(peer);
            if (nchunks_ > 0 && boundary_ >= nchunks_) {
                std::vector<uint8_t> all;
                all.reserve(totalBytes_);
                for (uint32_t i = 0; i < nchunks_; ++i)
                    all.insert(all.end(), buf_[i].begin(), buf_[i].end());
                all.resize(totalBytes_);
                outData = std::move(all);
                fromPeer = peer_.ip.empty() ? peer.ip : peer_.ip;
                peer_ = peer;
                listening_ = true;
                return true;
            }
        } else if (p.type == kTypeFin) {
            // 回确认，通知发送端可结束
            auto ack = packAck(0, 0xFFFFFFFFu);
            sock_.sendTo(peer.ip, peer.port, ack.data(), ack.size());
        }
    }
    return false;
}

void Receiver::advance(Endpoint peer) {
    while (boundary_ < nchunks_ && !buf_[boundary_].empty()) {
        ++boundary_;
        auto ack = packAck(totalBytes_, boundary_);
        sock_.sendTo(peer.ip, peer.port, ack.data(), ack.size());
    }
}
} // namespace rudp