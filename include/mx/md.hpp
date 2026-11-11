#pragma once
// Market data feed over UDP (multicast, or unicast for a single listener).
//
// Datagram = MdPacketHdr + count * MdMsg. first_seq is the sequence number
// of the first message in the datagram; receivers detect loss by comparing
// it with the next sequence number they expect. UDP gives no delivery or
// ordering guarantee, which is the point: the publisher never blocks on a
// slow consumer, and consumers must cope with gaps.
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "types.hpp"

namespace mx {

enum class MdType : uint8_t { Level = 1, Trade = 2 };

#pragma pack(push, 1)
struct MdMsg {
  uint8_t  type;   // MdType
  uint8_t  side;   // Level: book side.  Trade: aggressor side.
  uint16_t pad;
  uint32_t qty;    // Level: total resting qty at price (0 = level gone).  Trade: traded qty.
  int64_t  price;
};
struct MdPacketHdr {
  uint64_t first_seq;
  uint16_t count;
  uint16_t pad;
  uint32_t pad2;
};
#pragma pack(pop)
static_assert(sizeof(MdMsg) == 16);
static_assert(sizeof(MdPacketHdr) == 16);

constexpr size_t kMdMaxMsgs = 64;  // 16 + 64*16 = 1040 bytes, under a 1500-byte MTU

inline bool is_multicast(in_addr a) { return (ntohl(a.s_addr) >> 28) == 0xE; }

class MdPublisher {
 public:
  ~MdPublisher() { if (fd_ >= 0) ::close(fd_); }

  bool open(const char* addr, uint16_t port, const char* iface = "127.0.0.1") {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    std::memset(&dst_, 0, sizeof(dst_));
    dst_.sin_family = AF_INET;
    dst_.sin_port = htons(port);
    if (inet_pton(AF_INET, addr, &dst_.sin_addr) != 1) return false;
    if (is_multicast(dst_.sin_addr)) {
      in_addr ifa{};
      inet_pton(AF_INET, iface, &ifa);
      unsigned char loop = 1, ttl = 1;
      if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa)) < 0) return false;
      setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
      setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }
    return true;
  }

  // Send up to kMdMaxMsgs messages as one datagram.
  void send(const MdMsg* m, size_t n) {
    char pkt[sizeof(MdPacketHdr) + kMdMaxMsgs * sizeof(MdMsg)];
    MdPacketHdr h{};
    h.first_seq = seq_;
    h.count = static_cast<uint16_t>(n);
    std::memcpy(pkt, &h, sizeof(h));
    std::memcpy(pkt + sizeof(h), m, n * sizeof(MdMsg));
    ssize_t r = ::sendto(fd_, pkt, sizeof(h) + n * sizeof(MdMsg), 0,
                         reinterpret_cast<sockaddr*>(&dst_), sizeof(dst_));
    if (r < 0) ++send_errors_;
    seq_ += n;
    ++packets_;
  }

  uint64_t seq() const { return seq_; }
  uint64_t packets() const { return packets_; }
  uint64_t send_errors() const { return send_errors_; }

 private:
  int fd_ = -1;
  sockaddr_in dst_{};
  uint64_t seq_ = 1, packets_ = 0, send_errors_ = 0;
};

class MdReceiver {
 public:
  ~MdReceiver() { if (fd_ >= 0) ::close(fd_); }

  bool open(const char* addr, uint16_t port, const char* iface = "127.0.0.1") {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    int one = 1, rcvbuf = 8 << 20;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    in_addr grp{};
    if (inet_pton(AF_INET, addr, &grp) != 1) return false;
    // Multicast: bind to the group so we only see this feed. Unicast: bind to the address.
    a.sin_addr = grp;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) return false;
    if (is_multicast(grp)) {
      ip_mreq mreq{};
      mreq.imr_multiaddr = grp;
      inet_pton(AF_INET, iface, &mreq.imr_interface);
      if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) return false;
    }
    return true;
  }

  // Wait up to timeout_ms, then drain whatever is queued.
  // Handler: on_level(Side, Price, Qty), on_trade(Price, Qty, Side aggressor).
  // Returns the number of messages delivered.
  template <class H>
  int poll(H& h, int timeout_ms) {
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, timeout_ms) <= 0) return 0;
    int delivered = 0;
    char pkt[2048];
    for (int i = 0; i < 256; ++i) {
      ssize_t n = ::recv(fd_, pkt, sizeof(pkt), MSG_DONTWAIT);
      if (n < static_cast<ssize_t>(sizeof(MdPacketHdr))) break;
      MdPacketHdr hd;
      std::memcpy(&hd, pkt, sizeof(hd));
      if (static_cast<size_t>(n) != sizeof(hd) + hd.count * sizeof(MdMsg)) { ++malformed_; continue; }
      ++packets_;
      if (expected_ != 0) {
        if (hd.first_seq > expected_) { ++gap_events_; lost_ += hd.first_seq - expected_; }
        else if (hd.first_seq < expected_) { ++stale_; continue; }  // duplicate or reordered
      }
      expected_ = hd.first_seq + hd.count;
      for (uint16_t k = 0; k < hd.count; ++k) {
        MdMsg m;
        std::memcpy(&m, pkt + sizeof(hd) + k * sizeof(MdMsg), sizeof(m));
        if (m.type == static_cast<uint8_t>(MdType::Level))
          h.on_level(static_cast<Side>(m.side), m.price, m.qty);
        else if (m.type == static_cast<uint8_t>(MdType::Trade))
          h.on_trade(m.price, m.qty, static_cast<Side>(m.side));
        ++delivered;
      }
    }
    msgs_ += static_cast<uint64_t>(delivered);
    return delivered;
  }

  int fd() const { return fd_; }
  uint64_t packets() const { return packets_; }
  uint64_t msgs() const { return msgs_; }
  uint64_t gap_events() const { return gap_events_; }
  uint64_t lost() const { return lost_; }
  uint64_t stale() const { return stale_; }

 private:
  int fd_ = -1;
  uint64_t expected_ = 0, packets_ = 0, msgs_ = 0;
  uint64_t gap_events_ = 0, lost_ = 0, stale_ = 0, malformed_ = 0;
};

}  // namespace mx
