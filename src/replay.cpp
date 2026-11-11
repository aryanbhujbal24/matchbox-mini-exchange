// mx_replay: replay a LOBSTER day (https://lobsterdata.com) through the
// matching engine, optionally with the market maker quoting into it.
//
//   mx_replay --messages <..._message_N.csv> --orderbook <..._orderbook_N.csv>
//             [--mm] [--out DIR] [--gamma G --spread S] ...
//
// LOBSTER gives two files per stock-day: `message` (every event inside the
// top N price levels) and `orderbook` (the top N levels after each event).
//
// How history is replayed:
//   new limit order     -> a GTC limit order in our book
//   partial cancel      -> reduce in place (queue position kept)
//   delete              -> cancel
//   visible execution   -> if the executed order is the one our book would
//                          match next (front of the queue at the best
//                          price), an IOC order is sent and our matching
//                          logic executes it. Otherwise it is removed
//                          directly. The share handled by real matching is
//                          reported as "priority agreement".
//   hidden execution    -> no book change (a trade print only)
// The message file only covers events inside the top N levels, so orders
// that sat deeper when the day started, or were placed or cancelled deeper,
// are unknown. The first time a price level enters the N-level window it is
// reconciled against the orderbook file. After every event our top N levels
// are compared with the orderbook file and the match rate is reported.
//
// Market maker (--mm): no-market-impact assumption. History plays out
// exactly as it did; the maker's quotes are an overlay. The maker is filled
// only when a historical execution (visible or hidden) trades at a price its
// quote would have captured first: its quote is at the front of the queue at
// the best price and at least as good as the historical trade price. That
// is optimistic in one way: the historical trade still happens in full, so
// the aggressor is assumed to have had extra size for us.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/stat.h>
#include "mx/engine.hpp"
#include "mx/strategy.hpp"
#include "mx/util.hpp"
using namespace mx;

enum : uint32_t { MM = 1, HIST = 10, AGG = 11 };
constexpr int64_t kRawPerTick = 100;  // LOBSTER price = dollars * 10000; one tick = 1 cent

struct Msg { double t; int type; uint64_t id; Qty size; int64_t raw_px; int dir; };
struct Lvl { Price px; Qty qty; };  // px < 0: empty slot

struct Data {
  std::vector<Msg> msgs;
  std::vector<Lvl> book;  // rows * 2 * levels, per row: asks[0..L) then bids[0..L)
  int levels = 0;
  Price max_px = 0;
  const Lvl* asks(size_t row) const { return &book[row * 2 * levels]; }
  const Lvl* bids(size_t row) const { return &book[row * 2 * levels + levels]; }
};

static bool load(const char* mpath, const char* opath, Data& d) {
  FILE* f = std::fopen(mpath, "r");
  if (!f) { std::perror(mpath); return false; }
  char line[4096];
  while (std::fgets(line, sizeof(line), f)) {
    Msg m{};
    char* p = line;
    m.t = std::strtod(p, &p); ++p;
    m.type = static_cast<int>(std::strtol(p, &p, 10)); ++p;
    m.id = std::strtoull(p, &p, 10); ++p;
    m.size = static_cast<Qty>(std::strtoul(p, &p, 10)); ++p;
    m.raw_px = std::strtoll(p, &p, 10); ++p;
    m.dir = static_cast<int>(std::strtol(p, &p, 10));
    d.msgs.push_back(m);
    if (m.raw_px / kRawPerTick > d.max_px) d.max_px = m.raw_px / kRawPerTick;
  }
  std::fclose(f);

  f = std::fopen(opath, "r");
  if (!f) { std::perror(opath); return false; }
  std::vector<int64_t> vals;
  while (std::fgets(line, sizeof(line), f)) {
    vals.clear();
    for (char* p = line; *p && *p != '\n' && *p != '\r';) {
      vals.push_back(std::strtoll(p, &p, 10));
      if (*p == ',') ++p;
    }
    if (d.levels == 0) d.levels = static_cast<int>(vals.size() / 4);
    if (static_cast<int>(vals.size()) != 4 * d.levels) { std::fprintf(stderr, "orderbook: ragged row\n"); return false; }
    const size_t base = d.book.size();
    d.book.resize(base + 2 * static_cast<size_t>(d.levels));
    for (int i = 0; i < d.levels; ++i) {  // columns: ask px, ask size, bid px, bid size, per level
      const int64_t ap = vals[4 * i], bp = vals[4 * i + 2];
      const bool a_ok = ap > 0 && ap < 9'000'000'000 && vals[4 * i + 1] > 0;
      const bool b_ok = bp > 0 && bp < 9'000'000'000 && vals[4 * i + 3] > 0;
      d.book[base + i] = a_ok ? Lvl{ap / kRawPerTick, static_cast<Qty>(vals[4 * i + 1])} : Lvl{-1, 0};
      d.book[base + d.levels + i] = b_ok ? Lvl{bp / kRawPerTick, static_cast<Qty>(vals[4 * i + 3])} : Lvl{-1, 0};
      if (a_ok && ap / kRawPerTick > d.max_px) d.max_px = ap / kRawPerTick;
    }
  }
  std::fclose(f);
  if (d.book.size() != d.msgs.size() * 2 * static_cast<size_t>(d.levels)) {
    std::fprintf(stderr, "message and orderbook files have different row counts\n");
    return false;
  }
  return true;
}

struct MmFill { double t; int sign; Price px; Qty qty; double mid; };

struct Sink {
  OrderId last_oid = 0;
  Qty hist_filled = 0;   // quantity the current taker took from historical orders
  uint64_t trades = 0, volume = 0;
  // market maker
  int64_t inv = 0;
  double cash = 0, now = 0, mid = 0;
  OrderId bid = 0, ask = 0;
  Price bid_px = 0, ask_px = 0;
  std::vector<MmFill> fills;
  void on_ack(uint32_t, uint64_t, OrderId oid) { last_oid = oid; }
  void on_reject(uint32_t, uint64_t, RejectReason) { last_oid = 0; }
  void on_fill(uint32_t c, OrderId oid, Price px, Qty q, Qty leaves, Side s, bool maker) {
    if (c == MM) {
      const int sg = (s == Side::Buy) ? 1 : -1;
      inv += sg * static_cast<int64_t>(q);
      cash -= sg * static_cast<double>(px) * q;
      fills.push_back({now, sg, px, q, mid});
      if (leaves == 0) { if (oid == bid) bid = 0; if (oid == ask) ask = 0; }
    } else if (maker) {
      hist_filled += q;
    }
  }
  void on_canceled(uint32_t, OrderId, Qty, CancelReason) {}
  void on_trade(Price, Qty q, Side) { ++trades; volume += q; }
  void on_level(Side, Price, Qty) {}
};

struct Stats {
  uint64_t events = 0, l1_ok = 0, full_ok = 0, levels_cmp = 0, levels_ok = 0;
  uint64_t execs = 0, execs_matched = 0, exec_shares = 0, hidden = 0, hidden_shares = 0;
  uint64_t unknown_refs = 0, rebound = 0, seeded_levels = 0, rebuilt_levels = 0, crossed_adds = 0, skipped = 0;
  double secs = 0;
};

struct MmCfg {
  bool on = false;
  AsParams p;
  double interval = 1.0;   // seconds of market time between quote updates
  bool hidden_fills = true;
};

struct MmResult {
  double pnl = 0, sd_30m = 0, mean_abs_inv = 0, max_abs_inv = 0, final_inv = 0;
  double fills = 0, shares = 0, edge0 = 0, mo1 = 0, mo10 = 0, mo60 = 0, quote_msgs = 0;
  std::vector<double> t, mid, pnl_path, inv_path;  // one sample per second
};

class Replayer {
 public:
  Replayer(const Data& d, const MmCfg& mm)
      : d_(d), mm_(mm), eng_(d.max_px + 2000, 1 << 20, s_), vol_(4.0, 300.0) {}

  void run(Stats& st, MmResult* res) {
    const uint64_t t0 = now_ns();
    const double start = d_.msgs.front().t;
    double next_quote = start + 60.0;  // let the book and the volatility estimate settle for a minute
    double next_sample = std::floor(start) + 1.0;
    double abs_inv = 0, max_abs = 0;

    reconcile(0, st, true);  // seed from the state after the first event
    for (size_t k = 1; k < d_.msgs.size(); ++k) {
      const Msg& m = d_.msgs[k];
      s_.now = m.t;
      s_.mid = market_mid();
      while (m.t >= next_sample) {  // one sample per second of market time
        if (res) {
          res->t.push_back(next_sample - start);
          res->mid.push_back(s_.mid);
          res->pnl_path.push_back((s_.cash + static_cast<double>(s_.inv) * s_.mid) * 0.01);
          res->inv_path.push_back(static_cast<double>(s_.inv));
        }
        const double a = std::fabs(static_cast<double>(s_.inv));
        abs_inv += a;
        if (a > max_abs) max_abs = a;
        ++samples_;
        next_sample += 1.0;
      }
      if (mm_.on && m.t >= next_quote) { requote(m.t - start); next_quote = m.t + mm_.interval; }
      apply(m, st);
      reconcile(k, st, false);
      ++st.events;
    }
    st.secs = (now_ns() - t0) / 1e9;
    if (res) finish(*res, abs_inv, max_abs);
  }

 private:
  static uint64_t key(Side s, Price px) { return static_cast<uint64_t>(px) * 2 + static_cast<uint64_t>(s); }

  Qty mm_qty_at(Side side, Price px) const {
    if (side == Side::Buy) return (s_.bid && s_.bid_px == px) ? eng_.leaves(s_.bid) : 0;
    return (s_.ask && s_.ask_px == px) ? eng_.leaves(s_.ask) : 0;
  }
  // Best prices of the historical book, with the maker's own quotes removed.
  Price hist_best_bid() const {
    const OrderBook& b = eng_.book();
    Price p = b.best_bid();
    if (p >= 0 && b.level_qty(Side::Buy, p) == mm_qty_at(Side::Buy, p)) p = b.next_bid_below(p);
    return p;
  }
  Price hist_best_ask() const {
    const OrderBook& b = eng_.book();
    Price p = b.best_ask();
    if (p < b.max_price() && b.level_qty(Side::Sell, p) == mm_qty_at(Side::Sell, p)) p = b.next_ask_above(p);
    return p;
  }
  double market_mid() const {
    const Price bb = hist_best_bid(), ba = hist_best_ask();
    if (bb < 0 || ba >= eng_.book().max_price()) return s_.mid;
    return 0.5 * static_cast<double>(bb + ba);
  }

  OrderId send(uint32_t who, Side side, Price px, Qty q, Tif tif) {
    eng_.new_order(who, 0, side, px, q, tif, OrdType::Limit);
    return s_.last_oid;
  }
  void add_seed(Side side, Price px, Qty q) {
    const OrderId oid = send(HIST, side, px, q, Tif::GTC);
    if (oid && eng_.leaves(oid) > 0) seeds_[key(side, px)].push_back(oid);
  }
  // Remove quantity from the anonymous orders at a level. Returns what could not be removed.
  Qty reduce_seeds(Side side, Price px, Qty q) {
    auto it = seeds_.find(key(side, px));
    if (it == seeds_.end()) return q;
    auto& v = it->second;
    while (q > 0 && !v.empty()) {
      const Qty have = eng_.leaves(v.front());
      if (have == 0) { v.erase(v.begin()); continue; }
      const Qty by = q < have ? q : have;
      eng_.reduce(HIST, v.front(), by);
      q -= by;
      if (by == have) v.erase(v.begin());
    }
    return q;
  }
  // Remove `q` from historical order `id` (or, if we do not know it, from the level).
  void remove_hist(const Msg& m, Side side, Price px, Qty q, Stats& st) {
    auto it = live_.find(m.id);
    if (it != live_.end()) {
      const Qty have = eng_.leaves(it->second);
      const Qty by = q < have ? q : have;
      if (by > 0) eng_.reduce(HIST, it->second, by);
      q -= by;
      if (by == have) live_.erase(it);
    }
    if (q == 0) return;
    q = reduce_seeds(side, px, q);
    if (q == 0) return;
    // An id we have never seen, at a level with no anonymous depth. Exchanges
    // re-number an order when it is replaced, and LOBSTER does not report the
    // replace, so the quantity is sitting in our book under its old id. Take
    // it from an order at this level: one that is large enough if there is
    // one (and remember the new id), otherwise from the back of the queue.
    std::vector<std::pair<OrderId, Qty>> at;
    eng_.book().for_each_at(side, px, [&](OrderId id, Qty qty) {
      if (eng_.owner_of(id) == HIST) at.emplace_back(eng_.order_id_of(id), qty);
    });
    for (auto& o : at)
      if (o.second >= q) {
        eng_.reduce(HIST, o.first, q);
        if (o.second > q) live_[m.id] = o.first;
        ++st.rebound;
        return;
      }
    for (size_t i = at.size(); i-- > 0 && q > 0;) {
      const Qty by = q < at[i].second ? q : at[i].second;
      eng_.reduce(HIST, at[i].first, by);
      q -= by;
    }
    if (q > 0) ++st.unknown_refs;
  }

  // The maker's quote on `side` is first in line at the best price and at
  // least as good as historical trade price `px`: it would have traded first.
  void maybe_fill_mm(Side side, Price px, Qty size) {
    if (!mm_.on) return;
    const OrderBook& b = eng_.book();
    const OrderId q = (side == Side::Buy) ? s_.bid : s_.ask;
    if (!q) return;
    const Price qpx = (side == Side::Buy) ? s_.bid_px : s_.ask_px;
    const Price best = (side == Side::Buy) ? b.best_bid() : b.best_ask();
    const bool good = (side == Side::Buy) ? (qpx >= px) : (qpx <= px);
    if (qpx != best || !good || b.front_id(side, qpx) != static_cast<uint32_t>(q)) return;
    const Qty have = eng_.leaves(q);
    send(AGG, side == Side::Buy ? Side::Sell : Side::Buy, qpx, size < have ? size : have, Tif::IOC);
  }

  void apply(const Msg& m, Stats& st) {
    const Side side = (m.dir == 1) ? Side::Buy : Side::Sell;
    const Price px = m.raw_px / kRawPerTick;
    switch (m.type) {
      case 1: {
        if (m.raw_px % kRawPerTick) { ++st.skipped; break; }
        s_.hist_filled = 0;
        const size_t fills_before = s_.fills.size();
        const OrderId oid = send(HIST, side, px, m.size, Tif::GTC);
        if (s_.fills.size() != fills_before || s_.hist_filled) ++st.crossed_adds;  // it traded on arrival
        if (oid && eng_.leaves(oid) > 0) live_[m.id] = oid;
        break;
      }
      case 2:
      case 3:
        remove_hist(m, side, px, m.size, st);
        break;
      case 4: {
        ++st.execs;
        st.exec_shares += m.size;
        maybe_fill_mm(side, px, m.size);
        // Is the executed order the one price-time priority says is next?
        auto it = live_.find(m.id);
        const OrderBook& b = eng_.book();
        const Price best = (side == Side::Buy) ? hist_best_bid() : hist_best_ask();
        bool matched = false;
        if (it != live_.end() && best == px && b.front_id(side, px) == static_cast<uint32_t>(it->second) &&
            eng_.leaves(it->second) >= m.size && b.level_qty(side, px) > 0 &&
            ((side == Side::Buy) ? b.best_bid() : b.best_ask()) == px) {
          s_.hist_filled = 0;
          send(AGG, side == Side::Buy ? Side::Sell : Side::Buy, px, m.size, Tif::IOC);  // real matching
          matched = (s_.hist_filled == m.size);
          if (eng_.leaves(it->second) == 0) live_.erase(it);
        }
        if (matched) ++st.execs_matched;
        else remove_hist(m, side, px, m.size, st);
        break;
      }
      case 5:
        ++st.hidden;
        st.hidden_shares += m.size;
        if (mm_.hidden_fills) maybe_fill_mm(side, px, m.size);
        break;
      default: break;  // 7: trading halt indicator
    }
  }

  // Drop every historical order we hold at a level.
  void wipe_level(Side side, Price px, Stats& st) {
    std::vector<OrderId> drop;
    eng_.book().for_each_at(side, px, [&](OrderId id, Qty) {
      if (eng_.owner_of(id) == HIST) drop.push_back(eng_.order_id_of(id));
    });
    for (OrderId o : drop) eng_.cancel(HIST, o);
    seeds_.erase(key(side, px));
    ++st.rebuilt_levels;
  }

  // Compare our top levels with the orderbook file. A level seen for the
  // first time (beyond the previous row's window) is adopted from the file.
  void reconcile(size_t row, Stats& st, bool seed_all) {
    const OrderBook& b = eng_.book();
    const int L = d_.levels;
    bool all_ok = true, l1_ok = true;
    for (int sd = 0; sd < 2; ++sd) {
      const Side side = sd == 0 ? Side::Sell : Side::Buy;
      const Lvl* cur = sd == 0 ? d_.asks(row) : d_.bids(row);
      const Lvl* prev = seed_all ? nullptr : (sd == 0 ? d_.asks(row - 1) : d_.bids(row - 1));
      const Price prev_worst = prev ? prev[L - 1].px : -1;
      for (int i = 0; i < L; ++i) {
        if (cur[i].px < 0) continue;
        const Price px = cur[i].px;
        Qty ours = b.level_qty(side, px) - mm_qty_at(side, px);
        const bool fresh = seed_all || prev_worst < 0 || (side == Side::Sell ? px > prev_worst : px < prev_worst);
        if (ours != cur[i].qty && fresh) {
          if (ours > cur[i].qty) {  // orders we hold were cancelled while out of the window: rebuild the level
            wipe_level(side, px, st);
            ours = 0;
          } else {
            ++st.seeded_levels;
          }
          add_seed(side, px, cur[i].qty - ours);
          ours = b.level_qty(side, px) - mm_qty_at(side, px);
        }
        if (seed_all) continue;
        ++st.levels_cmp;
        if (ours == cur[i].qty) ++st.levels_ok;
        else { all_ok = false; if (i == 0) l1_ok = false; }
      }
      // Prices the window has just grown to cover: anything we still hold
      // there that the file does not show was cancelled while out of sight.
      Price cur_worst = -1;
      for (int i = L - 1; i >= 0 && cur_worst < 0; --i) cur_worst = cur[i].px;
      if (!seed_all && prev_worst >= 0 && cur_worst >= 0) {
        auto in_file = [&](Price px) { for (int i = 0; i < L; ++i) if (cur[i].px == px) return true; return false; };
        std::vector<Price> stale;
        if (side == Side::Sell)
          for (Price p = b.next_ask_above(prev_worst); p <= cur_worst; p = b.next_ask_above(p)) { if (!in_file(p)) stale.push_back(p); }
        else
          for (Price p = b.next_bid_below(prev_worst); p >= cur_worst; p = b.next_bid_below(p)) { if (!in_file(p)) stale.push_back(p); }
        for (Price p : stale) wipe_level(side, p, st);
      }
      // level 1 price must also be our best price (no phantom level in front)
      if (!seed_all && cur[0].px >= 0) {
        const Price best = side == Side::Sell ? hist_best_ask() : hist_best_bid();
        if (best != cur[0].px) { l1_ok = false; all_ok = false; }
      }
    }
    if (!seed_all) { st.l1_ok += l1_ok; st.full_ok += all_ok; }
  }

  void requote(double t_rel) {
    const Price bb = hist_best_bid(), ba = hist_best_ask();
    if (bb < 0 || ba >= eng_.book().max_price()) return;
    const double mid = 0.5 * static_cast<double>(bb + ba);
    vol_.update(mid, t_rel);
    const int64_t lots = std::llround(static_cast<double>(s_.inv) / mm_.p.size);
    const Quote q = as_quote(mm_.p, mid, lots, vol_.var_per_sec(), bb, ba);
    // Keep a quote that is already at the right price: it keeps its place in the queue.
    if (s_.bid && (!q.bid_on || s_.bid_px != q.bid)) { eng_.cancel(MM, s_.bid); s_.bid = 0; ++quote_msgs_; }
    if (s_.ask && (!q.ask_on || s_.ask_px != q.ask)) { eng_.cancel(MM, s_.ask); s_.ask = 0; ++quote_msgs_; }
    if (!s_.bid && q.bid_on) {
      s_.bid_px = q.bid;
      const OrderId o = send(MM, Side::Buy, q.bid, mm_.p.size, Tif::GTC);
      s_.bid = (o && eng_.leaves(o) > 0) ? o : 0;
      ++quote_msgs_;
    }
    if (!s_.ask && q.ask_on) {
      s_.ask_px = q.ask;
      const OrderId o = send(MM, Side::Sell, q.ask, mm_.p.size, Tif::GTC);
      s_.ask = (o && eng_.leaves(o) > 0) ? o : 0;
      ++quote_msgs_;
    }
  }

  void finish(MmResult& r, double abs_inv, double max_abs) {
    const double mid = market_mid();
    r.pnl = (s_.cash + static_cast<double>(s_.inv) * mid) * 0.01;
    r.final_inv = static_cast<double>(s_.inv);
    r.mean_abs_inv = samples_ ? abs_inv / static_cast<double>(samples_) : 0;
    r.max_abs_inv = max_abs;
    r.fills = static_cast<double>(s_.fills.size());
    r.quote_msgs = static_cast<double>(quote_msgs_);
    { double sum = 0, sq = 0; int n = 0;  // dispersion of PnL over 30-minute blocks
      for (size_t i = 1800; i < r.pnl_path.size(); i += 1800) {
        const double dlt = r.pnl_path[i] - r.pnl_path[i - 1800];
        sum += dlt; sq += dlt * dlt; ++n;
      }
      if (n > 1) r.sd_30m = std::sqrt((sq - sum * sum / n) / (n - 1)); }
    // Edge versus the mid at the fill, and markouts: the same fill valued at the mid h seconds later.
    double q0 = 0, q1 = 0, q10 = 0, q60 = 0, e0 = 0, m1 = 0, m10 = 0, m60 = 0;
    const double start = d_.msgs.front().t;
    auto mark = [&](const MmFill& f, int h, double& acc, double& qty) {
      const size_t idx = static_cast<size_t>(f.t - std::floor(start)) + static_cast<size_t>(h);
      if (idx < r.mid.size()) { acc += f.sign * (r.mid[idx] - static_cast<double>(f.px)) * f.qty; qty += f.qty; }
    };
    for (const MmFill& f : s_.fills) {
      e0 += f.sign * (f.mid - static_cast<double>(f.px)) * f.qty; q0 += f.qty;
      mark(f, 1, m1, q1); mark(f, 10, m10, q10); mark(f, 60, m60, q60);
    }
    r.shares = q0;
    if (q0 > 0) r.edge0 = e0 / q0;
    if (q1 > 0) r.mo1 = m1 / q1;
    if (q10 > 0) r.mo10 = m10 / q10;
    if (q60 > 0) r.mo60 = m60 / q60;
  }

  const Data& d_;
  MmCfg mm_;
  Sink s_;
  Engine<Sink> eng_;
  VolEstimator vol_;
  std::unordered_map<uint64_t, OrderId> live_;                 // LOBSTER order id -> our order id
  std::unordered_map<uint64_t, std::vector<OrderId>> seeds_;   // anonymous depth per (side, price)
  uint64_t samples_ = 0, quote_msgs_ = 0;
};

int main(int argc, char** argv) {
  const char* mpath = arg_str(argc, argv, "--messages", nullptr);
  const char* opath = arg_str(argc, argv, "--orderbook", nullptr);
  if (!mpath || !opath) {
    std::puts("usage: mx_replay --messages FILE --orderbook FILE [--mm] [--out DIR]\n"
              "       [--gamma G] [--spread TICKS] [--size SHARES] [--max-inv LOTS] [--interval SECONDS]");
    return 1;
  }
  const std::string out = arg_str(argc, argv, "--out", "out");
  ::mkdir(out.c_str(), 0755);
  Data d;
  const uint64_t tl = now_ns();
  if (!load(mpath, opath, d)) return 1;
  std::printf("loaded %zu events, %d levels, %.1f s of market time, in %.2f s\n", d.msgs.size(), d.levels,
              d.msgs.back().t - d.msgs.front().t, (now_ns() - tl) / 1e9);

  // --- 1. plain replay: does our book reproduce the exchange's? ---
  { Stats st;
    MmCfg off;
    Replayer r(d, off);
    r.run(st, nullptr);
    std::printf("\nReplay without the market maker\n");
    std::printf("  events replayed            %llu in %.2f s (%.2f M events/s, including the per-event book check)\n",
                (unsigned long long)st.events, st.secs, st.events / st.secs / 1e6);
    std::printf("  best bid+ask match file    %.3f%% of events\n", 100.0 * st.l1_ok / st.events);
    std::printf("  all %d levels match file   %.3f%% of events (%.3f%% of individual levels)\n", d.levels,
                100.0 * st.full_ok / st.events, 100.0 * st.levels_ok / st.levels_cmp);
    std::printf("  visible executions         %llu (%llu shares); %.2f%% hit the order our price-time priority picks\n",
                (unsigned long long)st.execs, (unsigned long long)st.exec_shares, 100.0 * st.execs_matched / st.execs);
    std::printf("  hidden executions          %llu (%llu shares), not applied to the book\n",
                (unsigned long long)st.hidden, (unsigned long long)st.hidden_shares);
    std::printf("  levels adopted from file   %llu added on entering the window, %llu rebuilt\n",
                (unsigned long long)st.seeded_levels, (unsigned long long)st.rebuilt_levels);
    std::printf("  re-numbered orders matched %llu by level; quantity never found %llu events; crossed on arrival %llu; off-tick skipped %llu\n",
                (unsigned long long)st.rebound, (unsigned long long)st.unknown_refs, (unsigned long long)st.crossed_adds, (unsigned long long)st.skipped); }

  if (!arg_flag(argc, argv, "--mm")) return 0;

  // --- 2. the market maker on this day ---
  MmCfg base;
  base.on = true;
  base.p.size = static_cast<Qty>(arg_int(argc, argv, "--size", 100));
  base.p.max_inv = arg_int(argc, argv, "--max-inv", 10);  // in lots of `size`
  base.p.tau = arg_dbl(argc, argv, "--tau", 1.0);
  base.interval = arg_dbl(argc, argv, "--interval", 1.0);
  base.hidden_fills = !arg_flag(argc, argv, "--no-hidden-fills");
  const double one_gamma = arg_dbl(argc, argv, "--gamma", -1), one_spread = arg_dbl(argc, argv, "--spread", -1);

  std::printf("\nMarket maker: %u shares per quote, inventory cap %lld lots, re-quote every %.1f s\n", base.p.size,
              (long long)base.p.max_inv, base.interval);
  std::printf("PnL in dollars, marked to the mid, no fees. edge and markouts in cents per share.\n");
  std::printf("%-7s %-6s %9s %8s %8s %8s %8s %6s %8s %6s %6s %6s %6s\n", "spread", "gamma", "PnL$", "sd30m$",
              "endInv", "avg|q|", "max|q|", "fills", "shares", "edge", "mo1s", "mo10s", "mo60s");
  FILE* csv = std::fopen((out + "/replay_sweep.csv").c_str(), "w");
  if (csv) std::fprintf(csv, "spread_ticks,gamma,pnl_usd,sd_30min_usd,final_inv,mean_abs_inv,max_abs_inv,fills,shares,"
                             "edge_cents,markout_1s,markout_10s,markout_60s,quote_msgs\n");
  std::vector<double> spreads = {6, 10, 14, 20}, gammas = {0, 0.02, 0.05, 0.1, 0.3};
  if (one_spread > 0) spreads = {one_spread};
  if (one_gamma >= 0) gammas = {one_gamma};
  for (double sp : spreads)
    for (double g : gammas) {
      MmCfg c = base;
      c.p.k = 2.0 / sp;  // baseline spread of the quoter = 2/k ticks
      c.p.gamma = g;
      Stats st;
      MmResult r;
      Replayer rp(d, c);
      rp.run(st, &r);
      std::printf("%-7g %-6g %9.0f %8.0f %8.0f %8.0f %8.0f %6.0f %8.0f %6.2f %6.2f %6.2f %6.2f\n", sp, g, r.pnl,
                  r.sd_30m, r.final_inv, r.mean_abs_inv, r.max_abs_inv, r.fills, r.shares, r.edge0, r.mo1, r.mo10, r.mo60);
      if (csv) std::fprintf(csv, "%g,%g,%.2f,%.2f,%.0f,%.1f,%.0f,%.0f,%.0f,%.4f,%.4f,%.4f,%.4f,%.0f\n", sp, g, r.pnl,
                            r.sd_30m, r.final_inv, r.mean_abs_inv, r.max_abs_inv, r.fills, r.shares, r.edge0, r.mo1,
                            r.mo10, r.mo60, r.quote_msgs);
      char name[512];
      std::snprintf(name, sizeof(name), "%s/replay_path_s%g_g%g.csv", out.c_str(), sp, g);
      if (FILE* f = std::fopen(name, "w")) {
        std::fprintf(f, "t,mid,inventory,pnl_usd\n");
        for (size_t i = 0; i < r.t.size(); i += 10)
          std::fprintf(f, "%.0f,%.1f,%.0f,%.2f\n", r.t[i], r.mid[i], r.inv_path[i], r.pnl_path[i]);
        std::fclose(f);
      }
    }
  if (csv) std::fclose(csv);
  std::printf("\nwrote %s/replay_sweep.csv and replay_path_*.csv\n", out.c_str());
  return 0;
}
