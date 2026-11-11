// mx_mm: the market maker as a live client of the exchange.
//
// Order entry over TCP, market data over UDP: the same split a real trading
// system has. Quotes come from the same as_quote() the backtest uses.
// Everything is asynchronous here, so each quote runs a small state machine
// (None -> PendingNew -> Live -> PendingCancel -> None) and we only act on
// a side when the exchange has confirmed its previous action.
#include <atomic>
#include <csignal>
#include <cstdio>
#include "mx/client.hpp"
#include "mx/l2_book.hpp"
#include "mx/md.hpp"
#include "mx/strategy.hpp"
#include "mx/util.hpp"
using namespace mx;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

struct QuoteState {
  enum St { None, PendingNew, Live, PendingCancel } st = None;
  Price px = 0;
  OrderId oid = 0;
  uint64_t cl = 0;
  Qty leaves = 0;
};

struct Maker {
  explicit Maker(Price max_price) : book(max_price) {}
  L2Book book;
  QuoteState q[2];  // [Buy, Sell]
  int64_t inv = 0;
  double cash = 0;
  uint64_t fills = 0, volume = 0, trades_seen = 0, orders_sent = 0;
  Price last_trade = 0;

  // --- market data ---
  void on_level(Side s, Price px, Qty qty) { book.apply(s, px, qty); }
  void on_trade(Price px, Qty, Side) { last_trade = px; ++trades_seen; }

  // --- execution reports ---
  void on_ack(uint64_t cl, OrderId oid) {
    for (auto& s : q)
      if (s.st == QuoteState::PendingNew && s.cl == cl) { s.oid = oid; s.st = QuoteState::Live; }
  }
  void on_reject(uint64_t ref, RejectReason) {
    for (auto& s : q) {
      if (s.st == QuoteState::PendingNew && s.cl == ref) s.st = QuoteState::None;
      else if (s.st == QuoteState::PendingCancel && s.oid == ref) s.st = QuoteState::None;  // already filled
    }
  }
  void on_fill(OrderId oid, Price px, Qty qty, Qty leaves, Side side, bool) {
    const int sg = (side == Side::Buy) ? 1 : -1;
    inv += sg * static_cast<int64_t>(qty);
    cash -= sg * static_cast<double>(px) * qty;
    ++fills;
    volume += qty;
    for (auto& s : q)
      if (s.st != QuoteState::None && s.oid == oid) {
        s.leaves = leaves;
        if (leaves == 0) s.st = QuoteState::None;
      }
  }
  void on_canceled(OrderId oid, Qty, CancelReason) {
    for (auto& s : q)
      if (s.st == QuoteState::PendingCancel && s.oid == oid) s.st = QuoteState::None;
  }

  // Our own resting quantity as the feed would show it.
  Qty own(int side) const {
    return (q[side].st == QuoteState::Live || q[side].st == QuoteState::PendingCancel) ? q[side].leaves : 0;
  }
};

int main(int argc, char** argv) {
  const char* host = arg_str(argc, argv, "--host", "127.0.0.1");
  const uint16_t port = static_cast<uint16_t>(arg_int(argc, argv, "--port", 9000));
  const char* md_addr = arg_str(argc, argv, "--md-addr", "239.1.1.1");
  const uint16_t md_port = static_cast<uint16_t>(arg_int(argc, argv, "--md-port", 9001));
  const char* md_iface = arg_str(argc, argv, "--md-iface", "127.0.0.1");
  const long seconds = arg_int(argc, argv, "--seconds", 0);
  const uint64_t interval_ns = static_cast<uint64_t>(arg_int(argc, argv, "--interval-ms", 100)) * 1'000'000ull;
  const char* csv_path = arg_str(argc, argv, "--csv", nullptr);
  const char* events_path = arg_str(argc, argv, "--events", nullptr);  // per-quote log for the visualization
  AsParams p;
  p.gamma = arg_dbl(argc, argv, "--gamma", 0.05);
  p.k = arg_dbl(argc, argv, "--k", p.k);
  p.tau = arg_dbl(argc, argv, "--tau", p.tau);
  p.size = static_cast<Qty>(arg_int(argc, argv, "--size", p.size));
  p.max_inv = arg_int(argc, argv, "--max-inv", p.max_inv);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  MdReceiver rx;
  if (!rx.open(md_addr, md_port, md_iface)) { std::perror("market data socket"); return 1; }
  Client c;
  if (!c.connect(host, port, static_cast<uint32_t>(arg_int(argc, argv, "--client", 1)))) { std::perror("connect"); return 1; }

  Maker m(arg_int(argc, argv, "--max-price", 20000));
  VolEstimator vol(1.0, 30.0);
  FILE* csv = csv_path ? std::fopen(csv_path, "w") : nullptr;
  if (csv) std::fprintf(csv, "t,mid,bid,ask,inventory,pnl\n");
  FILE* events = events_path ? std::fopen(events_path, "w") : nullptr;
  uint64_t next_cl = 1;
  const uint64_t start = now_ns();
  uint64_t next_quote = start, next_print = start + 1'000'000'000ull;
  double mid = 0;
  Quote want;

  while (!g_stop.load()) {
    rx.poll(m, 1);
    if (c.poll(m, 0) < 0) { std::puts("mm: exchange connection lost"); break; }
    const uint64_t t = now_ns();

    if (t >= next_quote) {
      next_quote = t + interval_ns;
      // The market without our own quotes in it.
      const Price bb = m.book.best_bid_excl(m.q[0].px, m.own(0));
      const Price ba = m.book.best_ask_excl(m.q[1].px, m.own(1));
      if (bb >= 0 && ba < m.book.max_price()) {
        mid = 0.5 * static_cast<double>(bb + ba);
        vol.update(mid, (t - start) / 1e9);
        want = as_quote(p, mid, m.inv, vol.var_per_sec(), bb, ba);
        if (events)
          std::fprintf(events, "Q,%llu,%.1f,%lld,%lld,%lld,%.1f,%.2f\n", (unsigned long long)t, mid,
                       (long long)want.bid, (long long)want.ask, (long long)m.inv,
                       m.cash + static_cast<double>(m.inv) * mid, want.reservation);
        const Price px[2] = {want.bid, want.ask};
        const bool on[2] = {want.bid_on, want.ask_on};
        for (int s = 0; s < 2; ++s) {
          QuoteState& qs = m.q[s];
          if (qs.st == QuoteState::Live && (!on[s] || qs.px != px[s])) {
            c.cancel(qs.oid);
            qs.st = QuoteState::PendingCancel;
            ++m.orders_sent;
          } else if (qs.st == QuoteState::None && on[s]) {
            qs.px = px[s];
            qs.cl = next_cl++;
            qs.leaves = p.size;
            qs.oid = 0;
            qs.st = QuoteState::PendingNew;
            c.new_order(qs.cl, s == 0 ? Side::Buy : Side::Sell, px[s], p.size);
            ++m.orders_sent;
          }
        }
        if (!c.flush()) break;
      }
    }

    if (t >= next_print) {
      next_print += 1'000'000'000ull;
      const double pnl = m.cash + static_cast<double>(m.inv) * mid;
      std::printf("[mm %3.0fs] mid %.1f  quote %lld / %lld  inv %+lld  pnl %+.1f  fills %llu  vol %.2f  md lost %llu\n",
                  (t - start) / 1e9, mid, (long long)m.q[0].px, (long long)m.q[1].px, (long long)m.inv, pnl,
                  (unsigned long long)m.fills, std::sqrt(vol.var_per_sec()), (unsigned long long)rx.lost());
      std::fflush(stdout);
      if (csv) std::fprintf(csv, "%.1f,%.1f,%lld,%lld,%lld,%.1f\n", (t - start) / 1e9, mid, (long long)m.q[0].px,
                            (long long)m.q[1].px, (long long)m.inv, pnl);
      if (seconds && t - start >= static_cast<uint64_t>(seconds) * 1'000'000'000ull) break;
    }
  }

  // Pull our quotes on the way out.
  for (auto& s : m.q)
    if (s.st == QuoteState::Live) c.cancel(s.oid);
  c.flush();
  for (int i = 0; i < 20; ++i) c.poll(m, 10);
  if (csv) std::fclose(csv);
  if (events) std::fclose(events);
  std::printf("mm done: inventory %+lld, pnl %+.1f (marked to mid %.1f), %llu fills, %llu units, %llu order messages\n",
              (long long)m.inv, m.cash + static_cast<double>(m.inv) * mid, mid, (unsigned long long)m.fills,
              (unsigned long long)m.volume, (unsigned long long)m.orders_sent);
  return 0;
}
