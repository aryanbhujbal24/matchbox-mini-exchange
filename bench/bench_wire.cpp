// bench_wire: fixed-width binary vs protobuf vs gRPC on the same workload.
//
// Part 1, encoding only: encode + decode a NewOrder and an Ack in memory.
// Part 2, round trip over loopback TCP, one order in flight: the client
//   sends a NewOrder, a server thread feeds it to a real matching engine and
//   replies with the Ack. Four transports do exactly the same work:
//     raw      fixed-width structs straight on a TCP socket (this project's protocol)
//     pb/tcp   protobuf messages, 2-byte length prefix, on a TCP socket
//     grpc     gRPC unary call (HTTP/2, one RPC per order)
//     grpc-s   gRPC bidirectional stream (HTTP/2, one stream for all orders)
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <grpcpp/grpcpp.h>
#include "mx.grpc.pb.h"
#include "mx/engine.hpp"
#include "mx/util.hpp"
using namespace mx;

// ------------------------------------------------------------ shared work
struct AckSink {
  uint64_t cl = 0;
  OrderId oid = 0;
  void on_ack(uint32_t, uint64_t c, OrderId o) { cl = c; oid = o; }
  void on_reject(uint32_t, uint64_t c, RejectReason) { cl = c; oid = 0; }
  void on_fill(uint32_t, OrderId, Price, Qty, Qty, Side, bool) {}
  void on_canceled(uint32_t, OrderId, Qty, CancelReason) {}
  void on_trade(Price, Qty, Side) {}
  void on_level(Side, Price, Qty) {}
};
struct Venue {  // one engine per server, so every transport does the same matching work
  AckSink sink;
  Engine<AckSink> engine{20000, 1 << 20, sink};
  void submit(uint64_t cl, uint8_t side, Price px, Qty qty) {
    engine.new_order(1, cl, static_cast<Side>(side), px, qty, Tif::GTC, OrdType::Limit);
  }
};
// Orders that rest and never cross: a buy below and a sell above, alternating.
static inline Price px_for(uint64_t i) { return (i & 1) ? 10010 + static_cast<Price>(i % 7) : 9990 - static_cast<Price>(i % 7); }
static inline Qty qty_for(uint64_t i) { return 1 + static_cast<Qty>(i % 100); }

static bool read_all(int fd, void* p, size_t n) {
  char* c = static_cast<char*>(p);
  while (n > 0) {
    ssize_t r = ::recv(fd, c, n, 0);
    if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
    c += r; n -= static_cast<size_t>(r);
  }
  return true;
}
static bool write_all(int fd, const void* p, size_t n) {
  const char* c = static_cast<const char*>(p);
  while (n > 0) {
    ssize_t r = ::send(fd, c, n, MSG_NOSIGNAL);
    if (r < 0) { if (errno == EINTR) continue; return false; }
    c += r; n -= static_cast<size_t>(r);
  }
  return true;
}
static int listen_on(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 || ::listen(fd, 1) < 0) { std::perror("listen"); std::exit(1); }
  return fd;
}
static int accept_one(int lfd) {
  int fd = ::accept(lfd, nullptr, nullptr), one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}
static int connect_to(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) { std::perror("connect"); std::exit(1); }
  return fd;
}

static void report(const char* name, LatStats& lat, double secs, double bytes_per_rt) {
  std::sort(lat.ns.begin(), lat.ns.end());
  std::printf("%-7s %9.0f round trips/s   p50 %6.1f us   p99 %6.1f us   p99.9 %7.1f us   %5.1f app bytes per round trip\n",
              name, lat.ns.size() / secs, lat.pct(0.5) / 1e3, lat.pct(0.99) / 1e3, lat.pct(0.999) / 1e3, bytes_per_rt);
}

// ------------------------------------------------------------ part 1
static void bench_encoding(uint64_t n) {
  std::printf("Part 1: encode + decode in memory, %llu messages each\n", (unsigned long long)n);
  uint64_t check = 0, bytes = 0;
  char buf[64];
  // NewOrder
  { const uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < n; ++i) {
      auto m = make_msg<NewOrderMsg>(MsgType::NewOrder);
      m.cl_ord_id = i; m.price = px_for(i); m.qty = qty_for(i); m.side = i & 1;
      std::memcpy(buf, &m, sizeof(m));
      __asm__ __volatile__("" : : "r"(buf) : "memory");  // the bytes escape: no eliding the round trip
      NewOrderMsg d; std::memcpy(&d, buf, sizeof(d));
      check += d.cl_ord_id + static_cast<uint64_t>(d.price) + d.qty + d.side;
    }
    std::printf("  NewOrder  raw       %6.1f ns   %2zu bytes\n", double(now_ns() - t0) / n, sizeof(NewOrderMsg)); }
  { mxpb::NewOrder m, d;
    const uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < n; ++i) {
      m.set_cl_ord_id(i); m.set_price(px_for(i)); m.set_qty(qty_for(i)); m.set_side(i & 1);
      const size_t len = m.ByteSizeLong();
      m.SerializeToArray(buf, static_cast<int>(len));
      __asm__ __volatile__("" : : "r"(buf) : "memory");
      d.ParseFromArray(buf, static_cast<int>(len));
      check -= d.cl_ord_id() + static_cast<uint64_t>(d.price()) + d.qty() + d.side();
      bytes += len;
    }
    std::printf("  NewOrder  protobuf  %6.1f ns   %4.1f bytes average (varint: size depends on the values)\n",
                double(now_ns() - t0) / n, double(bytes) / n); }
  // Ack (order ids are large: generation in the high 32 bits)
  bytes = 0;
  { const uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < n; ++i) {
      auto m = make_msg<AckMsg>(MsgType::Ack);
      m.cl_ord_id = i; m.order_id = ((i + 1) << 32) | (i & 0xFFFFF);
      std::memcpy(buf, &m, sizeof(m));
      __asm__ __volatile__("" : : "r"(buf) : "memory");
      AckMsg d; std::memcpy(&d, buf, sizeof(d));
      check += d.cl_ord_id ^ d.order_id;
    }
    std::printf("  Ack       raw       %6.1f ns   %2zu bytes\n", double(now_ns() - t0) / n, sizeof(AckMsg)); }
  { mxpb::Ack m, d;
    const uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < n; ++i) {
      m.set_cl_ord_id(i); m.set_order_id(((i + 1) << 32) | (i & 0xFFFFF));
      const size_t len = m.ByteSizeLong();
      m.SerializeToArray(buf, static_cast<int>(len));
      __asm__ __volatile__("" : : "r"(buf) : "memory");
      d.ParseFromArray(buf, static_cast<int>(len));
      check -= d.cl_ord_id() ^ d.order_id();
      bytes += len;
    }
    std::printf("  Ack       protobuf  %6.1f ns   %4.1f bytes average\n", double(now_ns() - t0) / n, double(bytes) / n); }
  std::printf("  (decoded values agree: %s)\n\n", check == 0 ? "yes" : "NO");
}

// ------------------------------------------------------------ part 2
static void bench_raw(uint64_t n, uint64_t warm, uint16_t port) {
  int lfd = listen_on(port);
  std::thread srv([&] {
    Venue v;
    int fd = accept_one(lfd);
    NewOrderMsg m;
    while (read_all(fd, &m, sizeof(m))) {
      v.submit(m.cl_ord_id, m.side, m.price, m.qty);
      auto a = make_msg<AckMsg>(MsgType::Ack);
      a.cl_ord_id = v.sink.cl; a.order_id = v.sink.oid;
      if (!write_all(fd, &a, sizeof(a))) break;
    }
    ::close(fd);
  });
  int fd = connect_to(port);
  LatStats lat; lat.ns.reserve(n);
  uint64_t t0 = 0, bad = 0;
  for (uint64_t i = 0; i < n + warm; ++i) {
    if (i == warm) t0 = now_ns();
    const uint64_t a = now_ns();
    auto m = make_msg<NewOrderMsg>(MsgType::NewOrder);
    m.cl_ord_id = i; m.price = px_for(i); m.qty = qty_for(i); m.side = i & 1;
    AckMsg ack;
    if (!write_all(fd, &m, sizeof(m)) || !read_all(fd, &ack, sizeof(ack))) break;
    bad += (ack.cl_ord_id != i || ack.order_id == 0);
    if (i >= warm) lat.add(now_ns() - a);
  }
  const double secs = (now_ns() - t0) / 1e9;
  ::close(fd); srv.join(); ::close(lfd);
  if (bad) std::printf("raw: %llu bad acks\n", (unsigned long long)bad);
  report("raw", lat, secs, sizeof(NewOrderMsg) + sizeof(AckMsg));
}

static void bench_pb_tcp(uint64_t n, uint64_t warm, uint16_t port) {
  int lfd = listen_on(port);
  std::thread srv([&] {
    Venue v;
    int fd = accept_one(lfd);
    mxpb::NewOrder m; mxpb::Ack a;
    char buf[128]; uint16_t len;
    while (read_all(fd, &len, 2) && len <= sizeof(buf) && read_all(fd, buf, len)) {
      if (!m.ParseFromArray(buf, len)) break;
      v.submit(m.cl_ord_id(), static_cast<uint8_t>(m.side()), m.price(), m.qty());
      a.set_cl_ord_id(v.sink.cl); a.set_order_id(v.sink.oid);
      const uint16_t out = static_cast<uint16_t>(a.ByteSizeLong());
      std::memcpy(buf, &out, 2);
      a.SerializeToArray(buf + 2, out);
      if (!write_all(fd, buf, 2u + out)) break;  // prefix and body in one send()
    }
    ::close(fd);
  });
  int fd = connect_to(port);
  LatStats lat; lat.ns.reserve(n);
  mxpb::NewOrder m; mxpb::Ack ack;
  char buf[128];
  uint64_t t0 = 0, bad = 0, bytes = 0;
  for (uint64_t i = 0; i < n + warm; ++i) {
    if (i == warm) t0 = now_ns();
    const uint64_t a = now_ns();
    m.set_cl_ord_id(i); m.set_price(px_for(i)); m.set_qty(qty_for(i)); m.set_side(i & 1);
    const uint16_t out = static_cast<uint16_t>(m.ByteSizeLong());
    std::memcpy(buf, &out, 2);
    m.SerializeToArray(buf + 2, out);
    uint16_t len;
    if (!write_all(fd, buf, 2u + out) || !read_all(fd, &len, 2) || len > sizeof(buf) || !read_all(fd, buf, len)) break;
    ack.ParseFromArray(buf, len);
    bad += (ack.cl_ord_id() != i || ack.order_id() == 0);
    if (i >= warm) { lat.add(now_ns() - a); bytes += 4u + out + len; }
  }
  const double secs = (now_ns() - t0) / 1e9;
  ::close(fd); srv.join(); ::close(lfd);
  if (bad) std::printf("pb/tcp: %llu bad acks\n", (unsigned long long)bad);
  report("pb/tcp", lat, secs, double(bytes) / double(lat.ns.size()));
}

class OrderEntryImpl final : public mxpb::OrderEntry::Service {
 public:
  grpc::Status Submit(grpc::ServerContext*, const mxpb::NewOrder* m, mxpb::Ack* a) override {
    std::lock_guard<std::mutex> g(mu_);  // gRPC's sync server runs handlers on a thread pool
    v_.submit(m->cl_ord_id(), static_cast<uint8_t>(m->side()), m->price(), m->qty());
    a->set_cl_ord_id(v_.sink.cl); a->set_order_id(v_.sink.oid);
    return grpc::Status::OK;
  }
  grpc::Status Stream(grpc::ServerContext*, grpc::ServerReaderWriter<mxpb::Ack, mxpb::NewOrder>* s) override {
    mxpb::NewOrder m; mxpb::Ack a;
    while (s->Read(&m)) {
      std::lock_guard<std::mutex> g(mu_);
      v_.submit(m.cl_ord_id(), static_cast<uint8_t>(m.side()), m.price(), m.qty());
      a.set_cl_ord_id(v_.sink.cl); a.set_order_id(v_.sink.oid);
      if (!s->Write(a)) break;
    }
    return grpc::Status::OK;
  }
 private:
  std::mutex mu_;
  Venue v_;
};

static void bench_grpc(uint64_t n, uint64_t warm, uint16_t port, bool streaming) {
  const std::string addr = "127.0.0.1:" + std::to_string(port);
  OrderEntryImpl svc;
  grpc::ServerBuilder b;
  b.AddListeningPort(addr, grpc::InsecureServerCredentials());
  b.RegisterService(&svc);
  std::unique_ptr<grpc::Server> server = b.BuildAndStart();
  auto stub = mxpb::OrderEntry::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
  LatStats lat; lat.ns.reserve(n);
  mxpb::NewOrder m; mxpb::Ack ack;
  uint64_t t0 = 0, bad = 0, bytes = 0;
  std::unique_ptr<grpc::ClientContext> sctx;
  std::unique_ptr<grpc::ClientReaderWriter<mxpb::NewOrder, mxpb::Ack>> stream;
  if (streaming) { sctx = std::make_unique<grpc::ClientContext>(); stream = stub->Stream(sctx.get()); }
  for (uint64_t i = 0; i < n + warm; ++i) {
    if (i == warm) t0 = now_ns();
    const uint64_t a = now_ns();
    m.set_cl_ord_id(i); m.set_price(px_for(i)); m.set_qty(qty_for(i)); m.set_side(i & 1);
    if (streaming) {
      if (!stream->Write(m) || !stream->Read(&ack)) { std::puts("grpc stream broke"); break; }
    } else {
      grpc::ClientContext ctx;
      if (!stub->Submit(&ctx, m, &ack).ok()) { std::puts("grpc call failed"); break; }
    }
    bad += (ack.cl_ord_id() != i || ack.order_id() == 0);
    if (i >= warm) { lat.add(now_ns() - a); bytes += m.ByteSizeLong() + ack.ByteSizeLong(); }
  }
  const double secs = (now_ns() - t0) / 1e9;
  if (streaming) { stream->WritesDone(); stream->Finish(); }
  server->Shutdown();
  if (bad) std::printf("grpc: %llu bad acks\n", (unsigned long long)bad);
  report(streaming ? "grpc-s" : "grpc", lat, secs, double(bytes) / double(lat.ns.size()));
}

int main(int argc, char** argv) {
  const uint64_t n_enc = static_cast<uint64_t>(arg_int(argc, argv, "--n-encode", 5'000'000));
  const uint64_t n = static_cast<uint64_t>(arg_int(argc, argv, "--n", 100'000));
  const uint64_t warm = n / 10;
  const uint16_t port = static_cast<uint16_t>(arg_int(argc, argv, "--port", 9100));
  bench_encoding(n_enc);
  std::printf("Part 2: order -> engine -> ack over loopback, one in flight, %llu round trips each\n", (unsigned long long)n);
  std::printf("(app bytes = message payloads only; gRPC adds HTTP/2 frames and headers on top)\n");
  bench_raw(n, warm, port);
  bench_pb_tcp(n, warm, static_cast<uint16_t>(port + 1));
  bench_grpc(n, warm, static_cast<uint16_t>(port + 2), false);
  bench_grpc(n, warm, static_cast<uint16_t>(port + 3), true);
  google::protobuf::ShutdownProtobufLibrary();
  return 0;
}
