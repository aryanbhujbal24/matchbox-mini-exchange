// mx_exchange: the venue.
//
//   gateway thread (epoll)  --SPSC-->  engine thread  --SPSC-->  gateway (exec reports)
//                                           |  \--SPSC-->  market data thread (UDP)
//                                           \--> journal (write-ahead, group commit)
//
// The gateway owns every socket; the engine owns the book. They share
// nothing but lock-free queues, so the matching path takes no locks.
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include "mx/engine.hpp"
#include "mx/journal.hpp"
#include "mx/md.hpp"
#include "mx/spsc_queue.hpp"
#include "mx/util.hpp"
using namespace mx;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

// An execution report on its way from the engine to one client.
struct OutMsg {
  uint32_t client;
  uint32_t len;
  char data[kMaxMsgSize];
};

// Engine output -> wire messages on the outbound queues.
struct LiveSink {
  SpscQueue<OutMsg>& out;
  SpscQueue<MdMsg>& md;
  int efd;
  bool md_on;
  bool live = false;   // false while replaying the journal: rebuild state, emit nothing
  bool dirty = false;  // exec reports queued since the last gateway wake-up
  uint64_t n_acks = 0, n_rejects = 0, n_fills = 0, n_trades = 0, n_md = 0;
  // --trace: a text log of everything the engine does, for the visualization.
  // Off by default; when on, it costs a formatted write per event on the engine thread.
  FILE* trace = nullptr;
  uint64_t trace_t = 0;  // timestamp of the command being processed

  void wake() {
    uint64_t one = 1;
    ssize_t r = ::write(efd, &one, sizeof(one));
    (void)r;
  }
  template <class M>
  void send(uint32_t client, const M& m) {
    if (!live) return;
    OutMsg o;
    o.client = client;
    o.len = sizeof(M);
    std::memcpy(o.data, &m, sizeof(M));
    while (!out.push(o)) {  // gateway is behind: wake it and wait (backpressure)
      if (g_stop.load(std::memory_order_relaxed)) return;
      wake();
      cpu_relax();
    }
    dirty = true;
  }
  void publish(MdType t, Side s, Price px, Qty q) {
    if (!live || !md_on) return;
    MdMsg m{static_cast<uint8_t>(t), static_cast<uint8_t>(s), 0, q, px};
    while (!md.push(m)) {
      if (g_stop.load(std::memory_order_relaxed)) return;
      cpu_relax();
    }
    ++n_md;
  }

  void on_ack(uint32_t c, uint64_t cl, OrderId oid) {
    auto m = make_msg<AckMsg>(MsgType::Ack);
    m.cl_ord_id = cl; m.order_id = oid;
    send(c, m); ++n_acks;
    if (trace && live) std::fprintf(trace, "A,%llu,%u,%llu\n", (unsigned long long)trace_t, c, (unsigned long long)oid);
  }
  void on_reject(uint32_t c, uint64_t ref, RejectReason r) {
    auto m = make_msg<RejectMsg>(MsgType::Reject);
    m.ref = ref; m.reason = static_cast<uint8_t>(r);
    send(c, m); ++n_rejects;
  }
  void on_fill(uint32_t c, OrderId oid, Price px, Qty q, Qty leaves, Side s, bool maker) {
    auto m = make_msg<FillMsg>(MsgType::Fill);
    m.order_id = oid; m.price = px; m.qty = q; m.leaves = leaves;
    m.side = static_cast<uint8_t>(s); m.is_maker = maker;
    send(c, m); ++n_fills;
    if (trace && live)
      std::fprintf(trace, "F,%llu,%u,%llu,%lld,%u,%u,%d,%d\n", (unsigned long long)trace_t, c,
                   (unsigned long long)oid, (long long)px, q, leaves, static_cast<int>(s), maker ? 1 : 0);
  }
  void on_canceled(uint32_t c, OrderId oid, Qty q, CancelReason r) {
    auto m = make_msg<CanceledMsg>(MsgType::Canceled);
    m.order_id = oid; m.qty = q; m.reason = static_cast<uint8_t>(r);
    send(c, m);
    if (trace && live)
      std::fprintf(trace, "K,%llu,%u,%llu,%u,%d\n", (unsigned long long)trace_t, c, (unsigned long long)oid, q,
                   static_cast<int>(r));
  }
  void on_trade(Price px, Qty q, Side aggressor) {
    publish(MdType::Trade, aggressor, px, q);
    ++n_trades;
    if (trace && live)
      std::fprintf(trace, "T,%llu,%lld,%u,%d\n", (unsigned long long)trace_t, (long long)px, q,
                   static_cast<int>(aggressor));
  }
  void on_level(Side s, Price px, Qty q) { publish(MdType::Level, s, px, q); }
};

// ---------------------------------------------------------------------------
// Gateway: non-blocking sockets + epoll, written by hand.
// Level-triggered: simpler to get right than edge-triggered (no need to
// drain to EAGAIN on every event) at the cost of some extra wake-ups.
// ---------------------------------------------------------------------------
class Gateway {
 public:
  Gateway(SpscQueue<Command>& in, SpscQueue<OutMsg>& out, int efd) : in_(in), out_(out), efd_(efd) {}

  bool listen_on(const char* bind_addr, uint16_t port) {
    lfd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (lfd_ < 0) return false;
    int one = 1;
    setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) return false;
    if (::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) return false;
    if (::listen(lfd_, 128) < 0) return false;
    ep_ = epoll_create1(0);
    if (ep_ < 0) return false;
    return add(lfd_, EPOLLIN) && add(efd_, EPOLLIN);
  }

  void run() {
    epoll_event evs[256];
    while (!g_stop.load(std::memory_order_relaxed)) {
      int n = epoll_wait(ep_, evs, 256, 100);
      for (int i = 0; i < n; ++i) {
        const int fd = evs[i].data.fd;
        const uint32_t ev = evs[i].events;
        if (fd == lfd_) { accept_all(); continue; }
        if (fd == efd_) { uint64_t v; ssize_t r = ::read(efd_, &v, sizeof(v)); (void)r; continue; }
        auto it = conns_.find(fd);
        if (it == conns_.end()) continue;  // closed earlier in this batch
        Conn& c = *it->second;
        if (ev & (EPOLLHUP | EPOLLERR)) { close_conn(fd); continue; }
        if ((ev & EPOLLIN) && !on_read(c)) { close_conn(fd); continue; }
        if (ev & EPOLLOUT) mark_dirty(c);
      }
      drain_outbound();
      flush_dirty();
    }
  }

  uint64_t cmds_in() const { return cmds_in_; }
  uint64_t accepted() const { return accepted_; }

 private:
  struct Conn {
    int fd = -1;
    uint32_t client = 0;  // 0 until Login
    size_t in_len = 0;
    char in[1 << 16];
    std::vector<char> out;  // bytes not yet accepted by the kernel
    size_t out_off = 0;
    bool want_out = false, dirty = false;
  };

  bool add(int fd, uint32_t events) {
    epoll_event e{};
    e.events = events;
    e.data.fd = fd;
    return epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &e) == 0;
  }
  void mod(int fd, uint32_t events) {
    epoll_event e{};
    e.events = events;
    e.data.fd = fd;
    epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &e);
  }

  void accept_all() {
    for (;;) {
      int fd = ::accept4(lfd_, nullptr, nullptr, SOCK_NONBLOCK);
      if (fd < 0) return;  // EAGAIN: no more pending connections
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      auto c = std::make_unique<Conn>();
      c->fd = fd;
      if (!add(fd, EPOLLIN)) { ::close(fd); continue; }
      conns_[fd] = std::move(c);
      ++accepted_;
    }
  }

  void close_conn(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    const uint32_t client = it->second->client;
    if (client) {
      auto bc = by_client_.find(client);
      if (bc != by_client_.end() && bc->second == fd) by_client_.erase(bc);
    }
    epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    conns_.erase(it);
    // Resting orders stay in the book; the client can log in again and cancel them.
  }

  bool on_read(Conn& c) {
    ssize_t n = ::recv(c.fd, c.in + c.in_len, sizeof(c.in) - c.in_len, 0);
    if (n == 0) return false;
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    c.in_len += static_cast<size_t>(n);
    // TCP is a byte stream: one recv() may hold many messages or part of one.
    size_t off = 0;
    while (c.in_len - off >= sizeof(MsgHeader)) {
      MsgHeader h;
      std::memcpy(&h, c.in + off, sizeof(h));
      const uint16_t want = msg_size(h.type);
      if (want == 0 || h.len != want) return false;  // protocol violation: drop the session
      if (c.in_len - off < want) break;
      if (!handle(c, h.type, c.in + off)) return false;
      off += want;
    }
    std::memmove(c.in, c.in + off, c.in_len - off);
    c.in_len -= off;
    return true;
  }

  bool handle(Conn& c, uint8_t type, const char* p) {
    switch (static_cast<MsgType>(type)) {
      case MsgType::Login: {
        LoginMsg m;
        std::memcpy(&m, p, sizeof(m));
        if (m.client_id == 0 || c.client != 0 || by_client_.count(m.client_id)) return false;
        c.client = m.client_id;
        by_client_[m.client_id] = c.fd;
        auto ack = make_msg<LoginAckMsg>(MsgType::LoginAck);
        ack.client_id = m.client_id;
        queue_bytes(c, &ack, sizeof(ack));
        return true;
      }
      case MsgType::NewOrder: {
        NewOrderMsg m;
        std::memcpy(&m, p, sizeof(m));
        if (!c.client) return reject_local(c, m.cl_ord_id);
        Command cmd{};
        cmd.type = static_cast<uint8_t>(CmdType::New);
        cmd.side = m.side; cmd.tif = m.tif; cmd.ord_type = m.ord_type;
        cmd.client = c.client; cmd.ref = m.cl_ord_id; cmd.price = m.price; cmd.qty = m.qty;
        push_cmd(cmd);
        return true;
      }
      case MsgType::Cancel: {
        CancelMsg m;
        std::memcpy(&m, p, sizeof(m));
        if (!c.client) return reject_local(c, m.order_id);
        Command cmd{};
        cmd.type = static_cast<uint8_t>(CmdType::Cancel);
        cmd.client = c.client; cmd.ref = m.order_id;
        push_cmd(cmd);
        return true;
      }
      default: return false;  // clients must not send exchange->client types
    }
  }

  bool reject_local(Conn& c, uint64_t ref) {
    auto r = make_msg<RejectMsg>(MsgType::Reject);
    r.ref = ref;
    r.reason = static_cast<uint8_t>(RejectReason::NotLoggedIn);
    queue_bytes(c, &r, sizeof(r));
    return true;
  }

  void push_cmd(const Command& cmd) {
    // Inbound queue full: keep draining exec reports while we wait, otherwise
    // gateway and engine could each block on the other's full queue.
    while (!in_.push(cmd)) {
      if (g_stop.load(std::memory_order_relaxed)) return;
      drain_outbound();
      cpu_relax();
    }
    ++cmds_in_;
  }

  void queue_bytes(Conn& c, const void* p, size_t n) {
    const char* b = static_cast<const char*>(p);
    c.out.insert(c.out.end(), b, b + n);
    mark_dirty(c);
  }
  void mark_dirty(Conn& c) {
    if (!c.dirty) { c.dirty = true; dirty_.push_back(c.fd); }
  }

  void drain_outbound() {
    OutMsg m;
    while (out_.pop(m)) {
      auto bc = by_client_.find(m.client);
      if (bc == by_client_.end()) continue;  // client is offline: report dropped
      auto it = conns_.find(bc->second);
      if (it == conns_.end()) continue;
      queue_bytes(*it->second, m.data, m.len);
    }
  }

  // One send() per connection per loop iteration, however many reports were queued.
  void flush_dirty() {
    for (size_t i = 0; i < dirty_.size(); ++i) {
      auto it = conns_.find(dirty_[i]);
      if (it == conns_.end()) continue;
      Conn& c = *it->second;
      c.dirty = false;
      if (!flush(c)) close_conn(c.fd);
    }
    dirty_.clear();
  }

  bool flush(Conn& c) {
    while (c.out_off < c.out.size()) {
      ssize_t n = ::send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, MSG_NOSIGNAL);
      if (n > 0) { c.out_off += static_cast<size_t>(n); continue; }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Kernel send buffer is full: keep the rest and ask epoll to tell us when to resume.
        if (c.out.size() - c.out_off > (64u << 20)) return false;  // slow consumer: cut it off
        if (!c.want_out) { mod(c.fd, EPOLLIN | EPOLLOUT); c.want_out = true; }
        return true;
      }
      return false;
    }
    c.out.clear();
    c.out_off = 0;
    if (c.want_out) { mod(c.fd, EPOLLIN); c.want_out = false; }
    return true;
  }

  SpscQueue<Command>& in_;
  SpscQueue<OutMsg>& out_;
  int efd_, lfd_ = -1, ep_ = -1;
  std::unordered_map<int, std::unique_ptr<Conn>> conns_;
  std::unordered_map<uint32_t, int> by_client_;
  std::vector<int> dirty_;
  uint64_t cmds_in_ = 0, accepted_ = 0;
};

int main(int argc, char** argv) {
  const char* bind_addr = arg_str(argc, argv, "--bind", "127.0.0.1");
  const uint16_t port = static_cast<uint16_t>(arg_int(argc, argv, "--port", 9000));
  const char* md_addr = arg_str(argc, argv, "--md-addr", "239.1.1.1");
  const uint16_t md_port = static_cast<uint16_t>(arg_int(argc, argv, "--md-port", 9001));
  const char* md_iface = arg_str(argc, argv, "--md-iface", "127.0.0.1");
  const char* journal_path = arg_str(argc, argv, "--journal", nullptr);
  const Price max_price = arg_int(argc, argv, "--max-price", 20000);
  const uint32_t max_orders = static_cast<uint32_t>(arg_int(argc, argv, "--max-orders", 1 << 20));
  const bool md_on = !arg_flag(argc, argv, "--no-md");
  const bool spin = arg_flag(argc, argv, "--spin");
  const bool fsync = arg_flag(argc, argv, "--fsync");
  const char* trace_path = arg_str(argc, argv, "--trace", nullptr);
  const int engine_cpu = static_cast<int>(arg_int(argc, argv, "--engine-cpu", -1));
  const int gateway_cpu = static_cast<int>(arg_int(argc, argv, "--gateway-cpu", -1));

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  SpscQueue<Command> inq(1 << 16);
  SpscQueue<OutMsg> outq(1 << 16);
  SpscQueue<MdMsg> mdq(1 << 16);
  const int efd = eventfd(0, EFD_NONBLOCK);
  LiveSink sink{outq, mdq, efd, md_on};
  Engine<LiveSink> engine(max_price, max_orders, sink);

  Journal journal;
  if (journal_path) {
    const uint64_t t0 = now_ns();
    const size_t n = Journal::replay(journal_path, [&](const Command& c) { apply(engine, c); });
    if (n)
      std::printf("recovered %zu commands from %s in %.1f ms: %zu resting orders, best bid %lld, best ask %lld\n",
                  n, journal_path, (now_ns() - t0) / 1e6, engine.book().order_count(),
                  (long long)engine.book().best_bid(), (long long)engine.book().best_ask());
    if (!journal.open(journal_path, fsync)) { std::perror("journal"); return 1; }
  }
  sink.n_acks = sink.n_rejects = sink.n_fills = sink.n_trades = 0;
  sink.live = true;
  if (trace_path && !(sink.trace = std::fopen(trace_path, "w"))) { std::perror("trace"); return 1; }

  MdPublisher pub;
  if (md_on && !pub.open(md_addr, md_port, md_iface)) { std::perror("market data socket"); return 1; }

  Gateway gw(inq, outq, efd);
  if (!gw.listen_on(bind_addr, port)) { std::perror("listen"); return 1; }
  std::printf("mx_exchange: orders tcp://%s:%u  market data udp://%s:%u%s  journal=%s%s\n",
              bind_addr, port, md_addr, md_port, md_on ? "" : " (off)",
              journal_path ? journal_path : "off", spin ? "  busy-poll" : "");
  std::fflush(stdout);

  uint64_t batches = 0, cmds = 0;
  std::thread engine_thread([&] {
    name_thread("mx-engine");
    pin_cpu(engine_cpu);
    Backoff idle{spin};
    Command batch[256];
    while (!g_stop.load(std::memory_order_relaxed)) {
      size_t n = 0;
      while (n < 256 && inq.pop(batch[n])) ++n;
      if (n == 0) { idle.wait(); continue; }
      idle.reset();
      if (journal_path) journal.append(batch, n);  // write-ahead: log first, then act
      if (!sink.trace) {
        for (size_t i = 0; i < n; ++i) apply(engine, batch[i]);
      } else {
        for (size_t i = 0; i < n; ++i) {
          const Command& c = batch[i];
          sink.trace_t = now_ns();
          if (c.type == static_cast<uint8_t>(CmdType::New))
            std::fprintf(sink.trace, "N,%llu,%u,%d,%lld,%u,%d,%d\n", (unsigned long long)sink.trace_t, c.client,
                         c.side, (long long)c.price, c.qty, c.tif, c.ord_type);
          else
            std::fprintf(sink.trace, "X,%llu,%u,%llu\n", (unsigned long long)sink.trace_t, c.client,
                         (unsigned long long)c.ref);
          apply(engine, c);
        }
      }
      if (sink.dirty) { sink.wake(); sink.dirty = false; }
      ++batches;
      cmds += n;
    }
  });

  std::thread md_thread([&] {
    if (!md_on) return;
    name_thread("mx-md");
    Backoff idle{false};
    MdMsg buf[kMdMaxMsgs];
    while (!g_stop.load(std::memory_order_relaxed)) {
      size_t n = 0;
      while (n < kMdMaxMsgs && mdq.pop(buf[n])) ++n;
      if (n == 0) { idle.wait(); continue; }
      idle.reset();
      pub.send(buf, n);
    }
  });

  name_thread("mx-gateway");
  pin_cpu(gateway_cpu);
  gw.run();
  engine_thread.join();
  md_thread.join();
  if (sink.trace) std::fclose(sink.trace);

  std::printf("\nmx_exchange stopped. connections=%llu commands=%llu (avg batch %.1f) acks=%llu rejects=%llu "
              "fills=%llu trades=%llu\n",
              (unsigned long long)gw.accepted(), (unsigned long long)cmds,
              batches ? double(cmds) / double(batches) : 0.0, (unsigned long long)sink.n_acks,
              (unsigned long long)sink.n_rejects, (unsigned long long)sink.n_fills,
              (unsigned long long)sink.n_trades);
  std::printf("market data: %llu msgs in %llu datagrams (%llu send errors). resting orders: %zu\n",
              (unsigned long long)sink.n_md, (unsigned long long)pub.packets(),
              (unsigned long long)pub.send_errors(), engine.book().order_count());
  return 0;
}
