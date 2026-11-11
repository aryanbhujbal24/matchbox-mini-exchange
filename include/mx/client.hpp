#pragma once
// Order-entry TCP client. Sends are buffered and go out on flush(), so a
// caller can batch several orders into one write(). TCP_NODELAY is set:
// with Nagle on, small order messages sit in the kernel waiting for an ACK.
#include <cerrno>
#include <cstring>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "protocol.hpp"

namespace mx {

class Client {
 public:
  ~Client() { if (fd_ >= 0) ::close(fd_); }

  bool connect(const char* host, uint16_t port, uint32_t client_id) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return false;
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) return false;
    auto m = make_msg<LoginMsg>(MsgType::Login);
    m.client_id = client_id;
    put(&m, sizeof(m));
    if (!flush()) return false;
    LoginAckMsg ack;
    size_t got = 0;
    while (got < sizeof(ack)) {
      ssize_t n = ::recv(fd_, reinterpret_cast<char*>(&ack) + got, sizeof(ack) - got, 0);
      if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
      got += static_cast<size_t>(n);
    }
    return ack.h.type == static_cast<uint8_t>(MsgType::LoginAck);
  }

  void new_order(uint64_t cl_ord_id, Side side, Price px, Qty qty,
                 Tif tif = Tif::GTC, OrdType type = OrdType::Limit) {
    auto m = make_msg<NewOrderMsg>(MsgType::NewOrder);
    m.cl_ord_id = cl_ord_id;
    m.price = px;
    m.qty = qty;
    m.side = static_cast<uint8_t>(side);
    m.tif = static_cast<uint8_t>(tif);
    m.ord_type = static_cast<uint8_t>(type);
    put(&m, sizeof(m));
  }

  void cancel(OrderId order_id) {
    auto m = make_msg<CancelMsg>(MsgType::Cancel);
    m.order_id = order_id;
    put(&m, sizeof(m));
  }

  bool flush() {
    size_t off = 0;
    while (off < out_.size()) {
      ssize_t n = ::send(fd_, out_.data() + off, out_.size() - off, MSG_NOSIGNAL);
      if (n < 0) { if (errno == EINTR) continue; return false; }
      off += static_cast<size_t>(n);
    }
    out_.clear();
    return true;
  }

  // Wait up to timeout_ms for data, then dispatch every complete message.
  // Handler: on_ack(cl_ord_id, order_id), on_reject(ref, RejectReason),
  //          on_fill(order_id, price, qty, leaves, side, is_maker),
  //          on_canceled(order_id, qty, CancelReason).
  // Returns messages handled, or -1 if the connection is gone.
  template <class H>
  int poll(H& h, int timeout_ms) {
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, timeout_ms) <= 0) return 0;
    ssize_t n = ::recv(fd_, in_ + in_len_, sizeof(in_) - in_len_, MSG_DONTWAIT);
    if (n == 0) return -1;
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    in_len_ += static_cast<size_t>(n);
    int handled = 0;
    size_t off = 0;
    while (in_len_ - off >= sizeof(MsgHeader)) {
      MsgHeader hd;
      std::memcpy(&hd, in_ + off, sizeof(hd));
      const uint16_t want = msg_size(hd.type);
      if (want == 0 || hd.len != want) return -1;
      if (in_len_ - off < want) break;
      dispatch(h, hd.type, in_ + off);
      off += want;
      ++handled;
    }
    std::memmove(in_, in_ + off, in_len_ - off);
    in_len_ -= off;
    return handled;
  }

  int fd() const { return fd_; }

 private:
  void put(const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    out_.insert(out_.end(), c, c + n);
  }

  template <class H>
  void dispatch(H& h, uint8_t type, const char* p) {
    switch (static_cast<MsgType>(type)) {
      case MsgType::Ack: {
        AckMsg m; std::memcpy(&m, p, sizeof(m));
        h.on_ack(m.cl_ord_id, m.order_id);
        break;
      }
      case MsgType::Reject: {
        RejectMsg m; std::memcpy(&m, p, sizeof(m));
        h.on_reject(m.ref, static_cast<RejectReason>(m.reason));
        break;
      }
      case MsgType::Fill: {
        FillMsg m; std::memcpy(&m, p, sizeof(m));
        h.on_fill(m.order_id, m.price, m.qty, m.leaves, static_cast<Side>(m.side), m.is_maker != 0);
        break;
      }
      case MsgType::Canceled: {
        CanceledMsg m; std::memcpy(&m, p, sizeof(m));
        h.on_canceled(m.order_id, m.qty, static_cast<CancelReason>(m.reason));
        break;
      }
      default: break;
    }
  }

  int fd_ = -1;
  std::vector<char> out_;
  char in_[1 << 16];
  size_t in_len_ = 0;
};

}  // namespace mx
