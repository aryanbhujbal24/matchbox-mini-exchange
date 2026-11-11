// Order book benchmark.
//   bench_order_book [--impl flat|naive] [--n N] [--spread TICKS] [--latency]
//
// The operation stream is generated up front, so the timed loop contains
// only book operations. Default mode measures throughput with no per-op
// timing. --latency wraps every operation in two clock reads and reports
// percentiles (the clock itself costs roughly 40 ns of each sample).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "mx/order_book.hpp"
#include "mx/util.hpp"
#include "naive_book.hpp"
using namespace mx;
using clk = std::chrono::steady_clock;

struct Op { OrderId id; Price px; Qty qty; uint8_t side; uint8_t cancel; };

template <class Book>
static void run(const char* name, const std::vector<Op>& ops, Price max_price, bool latency) {
  Book book(max_price, ops.size() + 16);
  uint64_t sink = 0;
  auto cb = [&](const Fill& f) { sink += f.qty; };
  std::vector<uint32_t> lat;
  if (latency) lat.reserve(ops.size());

  const auto t0 = clk::now();
  if (!latency) {
    for (const Op& o : ops) {
      if (o.cancel) book.cancel(o.id);
      else book.add_limit(o.id, static_cast<Side>(o.side), o.px, o.qty, cb);
    }
  } else {
    for (const Op& o : ops) {
      const auto a = clk::now();
      if (o.cancel) book.cancel(o.id);
      else book.add_limit(o.id, static_cast<Side>(o.side), o.px, o.qty, cb);
      const auto b = clk::now();
      lat.push_back(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
    }
  }
  const double secs = std::chrono::duration<double>(clk::now() - t0).count();
  std::printf("%-5s ops=%zu  %.2f M ops/s  %.1f ns/op  (checksum %llu)\n", name, ops.size(),
              ops.size() / secs / 1e6, secs * 1e9 / ops.size(), (unsigned long long)sink);
  if (latency) {
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat[static_cast<size_t>(p * (lat.size() - 1))]; };
    std::printf("      latency ns  p50=%u  p99=%u  p99.9=%u  max=%u\n", pct(0.5), pct(0.99), pct(0.999), lat.back());
  }
}

int main(int argc, char** argv) {
  const size_t n = static_cast<size_t>(arg_int(argc, argv, "--n", 2'000'000));
  const Price spread = arg_int(argc, argv, "--spread", 200);
  const char* impl = arg_str(argc, argv, "--impl", "flat");
  const bool latency = arg_flag(argc, argv, "--latency");
  const Price max_price = 100'000, mid = 50'000;

  // 70% new limit orders within spread/2 ticks of the mid, 30% cancels of a random earlier id.
  std::mt19937_64 rng(42);
  std::vector<Op> ops(n);
  for (size_t i = 0; i < n; ++i) {
    const OrderId id = i + 1;
    Op& o = ops[i];
    if (id > 10 && rng() % 10 < 3) o = {1 + rng() % (id - 1), 0, 0, 0, 1};
    else o = {id, mid + static_cast<Price>(rng() % static_cast<uint64_t>(spread)) - spread / 2,
              1 + static_cast<Qty>(rng() % 100), static_cast<uint8_t>(rng() & 1), 0};
  }
  if (!std::strcmp(impl, "naive")) run<NaiveBook>("naive", ops, max_price, latency);
  else run<OrderBook>("flat", ops, max_price, latency);
  return 0;
}
