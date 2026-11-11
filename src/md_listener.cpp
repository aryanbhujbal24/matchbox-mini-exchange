// mx_md_listener: subscribe to the UDP feed, rebuild the L2 book, report
// top of book and feed health (sequence gaps) once a second.
#include <atomic>
#include <csignal>
#include <cstdio>
#include "mx/l2_book.hpp"
#include "mx/md.hpp"
#include "mx/util.hpp"
using namespace mx;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

struct Handler {
  L2Book book;
  uint64_t levels = 0, trades = 0, volume = 0;
  Price last_px = 0;
  void on_level(Side s, Price px, Qty q) { book.apply(s, px, q); ++levels; }
  void on_trade(Price px, Qty q, Side) { ++trades; volume += q; last_px = px; }
};

int main(int argc, char** argv) {
  const char* addr = arg_str(argc, argv, "--md-addr", "239.1.1.1");
  const uint16_t port = static_cast<uint16_t>(arg_int(argc, argv, "--md-port", 9001));
  const char* iface = arg_str(argc, argv, "--md-iface", "127.0.0.1");
  const long seconds = arg_int(argc, argv, "--seconds", 0);
  const bool quiet = arg_flag(argc, argv, "--quiet");
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  MdReceiver rx;
  if (!rx.open(addr, port, iface)) { std::perror("market data socket"); return 1; }
  Handler h{L2Book(arg_int(argc, argv, "--max-price", 20000))};

  const uint64_t start = now_ns();
  uint64_t next = start + 1'000'000'000ull;
  while (!g_stop.load()) {
    rx.poll(h, 50);
    const uint64_t t = now_ns();
    if (t >= next) {
      next += 1'000'000'000ull;
      if (!quiet) {
        const Price bb = h.book.best_bid(), ba = h.book.best_ask();
        std::printf("[md %3.0fs] bid %lld x %u | ask %lld x %u | last %lld | trades %llu | msgs %llu | lost %llu\n",
                    (t - start) / 1e9, (long long)bb, h.book.has_bid() ? h.book.qty(Side::Buy, bb) : 0,
                    (long long)ba, h.book.has_ask() ? h.book.qty(Side::Sell, ba) : 0, (long long)h.last_px,
                    (unsigned long long)h.trades, (unsigned long long)rx.msgs(), (unsigned long long)rx.lost());
        std::fflush(stdout);
      }
      if (seconds && t - start >= static_cast<uint64_t>(seconds) * 1'000'000'000ull) break;
    }
  }
  std::printf("md_listener: %llu datagrams, %llu msgs (%llu level, %llu trade), volume %llu, "
              "gap events %llu, msgs lost %llu, stale datagrams %llu\n",
              (unsigned long long)rx.packets(), (unsigned long long)rx.msgs(), (unsigned long long)h.levels,
              (unsigned long long)h.trades, (unsigned long long)h.volume, (unsigned long long)rx.gap_events(),
              (unsigned long long)rx.lost(), (unsigned long long)rx.stale());
  return 0;
}
