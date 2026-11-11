// mx_flow: simulated market participants for the live demo, the same three
// populations as the backtest, each on its own exchange session:
//   LP        slow liquidity provider re-centring a quote ladder on V
//   noise     random market orders
//   informed  IOC orders priced off the true value V: they only execute
//             when a resting quote is wrong by more than `edge`
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <random>
#include <vector>
#include "mx/client.hpp"
#include "mx/util.hpp"
using namespace mx;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

struct LpHandler {
  std::vector<OrderId> live;
  void on_ack(uint64_t, OrderId oid) { live.push_back(oid); }
  void on_reject(uint64_t, RejectReason) {}
  void on_fill(OrderId, Price, Qty, Qty, Side, bool) {}
  void on_canceled(OrderId, Qty, CancelReason) {}
};
struct CountHandler {
  uint64_t fills = 0, volume = 0;
  void on_ack(uint64_t, OrderId) {}
  void on_reject(uint64_t, RejectReason) {}
  void on_fill(OrderId, Price, Qty q, Qty, Side, bool) { ++fills; volume += q; }
  void on_canceled(OrderId, Qty, CancelReason) {}
};

int main(int argc, char** argv) {
  const char* host = arg_str(argc, argv, "--host", "127.0.0.1");
  const uint16_t port = static_cast<uint16_t>(arg_int(argc, argv, "--port", 9000));
  const long seconds = arg_int(argc, argv, "--seconds", 0);
  const double sigma = arg_dbl(argc, argv, "--sigma", 2.0);
  const double noise_rate = arg_dbl(argc, argv, "--noise-rate", 2.0);
  const double inf_rate = arg_dbl(argc, argv, "--inf-rate", 1.0);
  const double inf_edge = arg_dbl(argc, argv, "--inf-edge", 1.0);
  const int lp_half = static_cast<int>(arg_int(argc, argv, "--lp-half", 5));
  const uint32_t base_id = static_cast<uint32_t>(arg_int(argc, argv, "--client", 2));
  const char* events_path = arg_str(argc, argv, "--events", nullptr);  // value path for the visualization
  FILE* events = events_path ? std::fopen(events_path, "w") : nullptr;
  const double dt = 0.02;  // 20 ms tick
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  Client lp, noise, inf;
  if (!lp.connect(host, port, base_id) || !noise.connect(host, port, base_id + 1) ||
      !inf.connect(host, port, base_id + 2)) { std::perror("connect"); return 1; }

  std::mt19937_64 rng(static_cast<uint64_t>(arg_int(argc, argv, "--seed", 1)));
  std::normal_distribution<double> norm(0.0, 1.0);
  std::poisson_distribution<int> n_noise(noise_rate * dt), n_inf(inf_rate * dt);
  LpHandler lph;
  CountHandler nh, ih;
  double V = static_cast<double>(arg_int(argc, argv, "--start", 10000));
  uint64_t cl = 0, step = 0;
  const uint64_t start = now_ns();
  const uint64_t tick_ns = static_cast<uint64_t>(dt * 1e9);

  while (!g_stop.load()) {
    V += sigma * std::sqrt(dt) * norm(rng);
    if (events && step % 5 == 0) std::fprintf(events, "V,%llu,%.2f\n", (unsigned long long)now_ns(), V);

    if (step % 50 == 0) {  // LP re-centres once a second
      for (OrderId o : lph.live) lp.cancel(o);
      lph.live.clear();
      const Price ctr = std::llround(V);
      for (int l = 0; l < 3; ++l) {
        lp.new_order(++cl, Side::Buy, ctr - lp_half - l, 50);
        lp.new_order(++cl, Side::Sell, ctr + lp_half + l, 50);
      }
      if (!lp.flush()) break;
    }
    for (int i = n_inf(rng); i > 0; --i) {
      inf.new_order(++cl, Side::Buy, static_cast<Price>(std::floor(V - inf_edge)), 5, Tif::IOC);
      inf.new_order(++cl, Side::Sell, static_cast<Price>(std::ceil(V + inf_edge)), 5, Tif::IOC);
    }
    for (int i = n_noise(rng); i > 0; --i)
      noise.new_order(++cl, (rng() & 1) ? Side::Buy : Side::Sell, 0, 1 + static_cast<Qty>(rng() % 5),
                      Tif::IOC, OrdType::Market);
    if (!inf.flush() || !noise.flush()) break;

    // Sleep out the rest of the tick while reading responses.
    ++step;
    const uint64_t deadline = start + step * tick_ns;
    for (;;) {
      if (lp.poll(lph, 0) < 0 || noise.poll(nh, 0) < 0 || inf.poll(ih, 0) < 0) { g_stop.store(true); break; }
      const uint64_t t = now_ns();
      if (t >= deadline) break;
      usleep(static_cast<useconds_t>(std::min<uint64_t>((deadline - t) / 1000, 2000)));
    }
    if (seconds && now_ns() - start >= static_cast<uint64_t>(seconds) * 1'000'000'000ull) break;
  }
  if (events) std::fclose(events);
  std::printf("flow done: value %.1f, noise %llu fills / %llu units, informed %llu fills / %llu units\n", V,
              (unsigned long long)nh.fills, (unsigned long long)nh.volume, (unsigned long long)ih.fills,
              (unsigned long long)ih.volume);
  return 0;
}
