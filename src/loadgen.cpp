// mx_loadgen: order-entry load generator and latency probe.
//
// Sends `window` requests, flushes, waits for all their responses, repeats.
//   window = 1   -> clean round-trip latency (send -> ack), one order in flight
//   window = 64+ -> throughput, with batching on both sides
// Every request gets exactly one primary response (Ack/Reject for a new
// order, Canceled/Reject for a cancel) and one session's responses come back
// in request order, so latencies are matched with a simple FIFO.
#include <cstdio>
#include <random>
#include <vector>
#include "mx/client.hpp"
#include "mx/util.hpp"
using namespace mx;

struct Handler {
  std::vector<uint64_t>& t_send;
  LatStats& lat;
  std::vector<OrderId> live;
  uint64_t responses = 0, acks = 0, rejects = 0, fills = 0, cancels = 0;
  void primary() { lat.add(now_ns() - t_send[responses]); ++responses; }
  void on_ack(uint64_t, OrderId oid) { ++acks; live.push_back(oid); primary(); }
  void on_reject(uint64_t, RejectReason) { ++rejects; primary(); }
  void on_fill(OrderId, Price, Qty, Qty, Side, bool) { ++fills; }
  void on_canceled(OrderId, Qty, CancelReason r) {
    if (r == CancelReason::User) { ++cancels; primary(); }
  }
};

int main(int argc, char** argv) {
  const char* host = arg_str(argc, argv, "--host", "127.0.0.1");
  const uint16_t port = static_cast<uint16_t>(arg_int(argc, argv, "--port", 9000));
  const uint32_t client_id = static_cast<uint32_t>(arg_int(argc, argv, "--client", 100));
  const uint64_t n = static_cast<uint64_t>(arg_int(argc, argv, "--n", 200000));
  const uint64_t window = static_cast<uint64_t>(arg_int(argc, argv, "--window", 1));
  const unsigned cancel_pct = static_cast<unsigned>(arg_int(argc, argv, "--cancel-pct", 30));
  const Price mid = arg_int(argc, argv, "--mid", 10000);
  const uint64_t warmup = std::min<uint64_t>(n / 10, 10000);
  pin_cpu(static_cast<int>(arg_int(argc, argv, "--cpu", -1)));

  Client c;
  if (!c.connect(host, port, client_id)) { std::perror("connect"); return 1; }

  std::vector<uint64_t> t_send(n);
  LatStats lat;
  lat.ns.reserve(n);
  Handler h{t_send, lat, {}};
  std::mt19937_64 rng(client_id);

  uint64_t sent = 0, t0 = 0;
  while (sent < n) {
    if (!t0 && sent >= warmup) { t0 = now_ns(); lat.ns.clear(); }
    for (uint64_t w = 0; w < window && sent < n; ++w, ++sent) {
      t_send[sent] = now_ns();
      if (!h.live.empty() && rng() % 100 < cancel_pct) {
        const size_t i = rng() % h.live.size();
        c.cancel(h.live[i]);
        h.live[i] = h.live.back();
        h.live.pop_back();
      } else {
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        const Price px = mid + static_cast<Price>(rng() % 200) - 100;
        c.new_order(sent, side, px, 1 + static_cast<Qty>(rng() % 100));
      }
    }
    if (!c.flush()) { std::puts("connection lost"); return 1; }
    while (h.responses < sent)
      if (c.poll(h, 1000) < 0) { std::puts("connection lost"); return 1; }
  }
  const double secs = (now_ns() - t0) / 1e9;
  std::printf("requests=%llu window=%llu  acks=%llu cancels=%llu rejects=%llu fills=%llu\n",
              (unsigned long long)n, (unsigned long long)window, (unsigned long long)h.acks,
              (unsigned long long)h.cancels, (unsigned long long)h.rejects, (unsigned long long)h.fills);
  std::printf("throughput: %.0f requests/s (after %llu warm-up requests)\n",
              double(lat.ns.size()) / secs, (unsigned long long)warmup);
  lat.print("round-trip latency");
  return 0;
}
