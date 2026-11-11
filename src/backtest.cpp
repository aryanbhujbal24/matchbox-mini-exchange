// mx_backtest: run the market maker against a simulated market, in-process,
// through the SAME matching engine the live exchange uses.
//
// Market model (all sizes in units, prices in ticks, time in steps of dt):
//   - A fundamental value V follows a random walk.
//   - A slow liquidity provider (LP) keeps a ladder of quotes around V but
//     only re-centres it every lp_every steps, so its quotes go stale.
//   - Noise traders send market orders at random: uninformed flow.
//   - Informed traders know V. They send an IOC order only when a resting
//     quote is mispriced against V by more than `inf_edge`: toxic flow.
//   - Our market maker quotes inside the LP with Avellaneda-Stoikov.
//
// The maker only re-quotes every mm_every steps and its reference price is
// the LP's (stale) mid, so its quotes can be picked off too. It earns the
// spread from noise traders and loses to informed ones (adverse selection).
// PnL and fill edges are marked to V, the true value, which a simulation
// knows and a real desk does not. Strategies are compared on common random numbers:
// same seed -> same V path and same order arrivals for every strategy.
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include <sys/stat.h>
#include "mx/engine.hpp"
#include "mx/strategy.hpp"
#include "mx/util.hpp"
using namespace mx;

enum : uint32_t { MM = 1, LP = 2, NOISE = 3, INFORMED = 4 };

struct MmFill { int step; int sign; Price px; Qty qty; double ref; double value; uint32_t taker; };

struct BtSink {
  OrderId last_oid = 0;
  int step = 0;
  double ref = 0;          // reference mid at the maker's last quote update
  double value = 0;        // fundamental value right now
  uint32_t taker = 0;      // who is sending the order being matched
  int64_t inv = 0;
  double cash = 0;
  OrderId bid = 0, ask = 0;
  std::vector<MmFill> fills;
  void on_ack(uint32_t, uint64_t, OrderId oid) { last_oid = oid; }
  void on_reject(uint32_t, uint64_t, RejectReason) { last_oid = 0; }
  void on_fill(uint32_t c, OrderId oid, Price px, Qty q, Qty leaves, Side s, bool) {
    if (c != MM) return;
    const int sg = (s == Side::Buy) ? 1 : -1;
    inv += sg * static_cast<int64_t>(q);
    cash -= sg * static_cast<double>(px) * q;
    fills.push_back({step, sg, px, q, ref, value, taker});
    if (leaves == 0) { if (oid == bid) bid = 0; if (oid == ask) ask = 0; }
  }
  void on_canceled(uint32_t, OrderId, Qty, CancelReason) {}
  void on_trade(Price, Qty, Side) {}
  void on_level(Side, Price, Qty) {}
};

struct Cfg {
  double sigma = 2.0;       // ticks per sqrt(second)
  double dt = 0.1;          // seconds per step
  int steps = 36000;        // 1 hour
  int lp_every = 10;        // LP re-centres once a second
  int mm_every = 5;         // our maker re-quotes every 0.5 s
  int lp_half = 5, lp_levels = 3;
  Qty lp_size = 50;
  double noise_rate = 2.0;  // market orders per second
  Qty noise_max = 5;
  double inf_rate = 1.0;    // informed looks per second
  Qty inf_size = 5;
  double inf_edge = 1.0;
  Price start = 10000;
};

struct Result {
  double pnl = 0, sd_1m = 0, mean_abs_inv = 0, max_abs_inv = 0;
  double fills = 0, volume = 0;
  // Per unit traded, in ticks:
  double quoted = 0;      // edge versus the reference mid we quoted around
  double realized = 0;    // edge versus true value at the moment of the fill
  double realized10 = 0;  // edge versus true value 10 s later (sanity check: should match)
  double edge_noise = 0, edge_inf = 0;  // realized edge split by counterparty
  double inf_share = 0;   // fraction of our volume traded against informed flow
};

static Result run(const Cfg& c, const AsParams& p, uint64_t seed, const char* path_csv) {
  BtSink s;
  Engine<BtSink> eng(20000, 4096, s);
  std::mt19937_64 rng_v(seed), rng_f(seed * 0x9E3779B97F4A7C15ull + 1);
  std::normal_distribution<double> norm(0.0, 1.0);
  std::poisson_distribution<int> n_noise(c.noise_rate * c.dt), n_inf(c.inf_rate * c.dt);
  VolEstimator vol(c.sigma * c.sigma);
  std::vector<OrderId> lp_orders;
  std::vector<double> mid(c.steps), pnl(c.steps), val(c.steps);
  std::vector<int64_t> invs(c.steps);
  uint64_t cl = 0;
  double V = static_cast<double>(c.start);
  double abs_inv_sum = 0;
  int64_t max_abs = 0;

  auto send = [&](uint32_t who, Side side, Price px, Qty q, Tif tif, OrdType t) {
    s.taker = who;
    eng.new_order(who, ++cl, side, px, q, tif, t);
    return s.last_oid;
  };

  for (int step = 0; step < c.steps; ++step) {
    s.step = step;
    V += c.sigma * std::sqrt(c.dt) * norm(rng_v);
    s.value = V;

    if (step % c.lp_every == 0) {  // slow LP re-centres its ladder on V
      for (OrderId o : lp_orders) eng.cancel(LP, o);
      lp_orders.clear();
      const Price ctr = std::llround(V);
      for (int l = 0; l < c.lp_levels; ++l) {
        lp_orders.push_back(send(LP, Side::Buy, ctr - c.lp_half - l, c.lp_size, Tif::GTC, OrdType::Limit));
        lp_orders.push_back(send(LP, Side::Sell, ctr + c.lp_half + l, c.lp_size, Tif::GTC, OrdType::Limit));
      }
    }

    // Market maker: pull quotes, look at the market without them, re-quote.
    if (step % c.mm_every == 0) {
      if (s.bid) { eng.cancel(MM, s.bid); s.bid = 0; }
      if (s.ask) { eng.cancel(MM, s.ask); s.ask = 0; }
      const Price bb = eng.book().best_bid(), ba = eng.book().best_ask();
      if (eng.book().has_bid() && eng.book().has_ask()) {
        s.ref = 0.5 * static_cast<double>(bb + ba);
        vol.update(s.ref, step * c.dt);
        const Quote q = as_quote(p, s.ref, s.inv, vol.var_per_sec(), bb, ba);
        if (q.bid_on) s.bid = send(MM, Side::Buy, q.bid, p.size, Tif::GTC, OrdType::Limit);
        if (q.ask_on) s.ask = send(MM, Side::Sell, q.ask, p.size, Tif::GTC, OrdType::Limit);
      }
    }

    // Informed flow: trade only against quotes that are wrong versus V.
    for (int i = n_inf(rng_f); i > 0; --i) {
      const OrderBook& b = eng.book();
      if (b.has_ask() && static_cast<double>(b.best_ask()) <= V - c.inf_edge)
        send(INFORMED, Side::Buy, static_cast<Price>(std::floor(V - c.inf_edge)), c.inf_size, Tif::IOC, OrdType::Limit);
      else if (b.has_bid() && static_cast<double>(b.best_bid()) >= V + c.inf_edge)
        send(INFORMED, Side::Sell, static_cast<Price>(std::ceil(V + c.inf_edge)), c.inf_size, Tif::IOC, OrdType::Limit);
    }
    // Noise flow: random side, random size.
    for (int i = n_noise(rng_f); i > 0; --i) {
      const Side side = (rng_f() & 1) ? Side::Buy : Side::Sell;
      const Qty q = 1 + static_cast<Qty>(rng_f() % c.noise_max);
      send(NOISE, side, 0, q, Tif::IOC, OrdType::Market);
    }

    mid[step] = s.ref;
    val[step] = V;
    invs[step] = s.inv;
    pnl[step] = s.cash + static_cast<double>(s.inv) * V;  // marked to true value
    const int64_t a = s.inv < 0 ? -s.inv : s.inv;
    abs_inv_sum += static_cast<double>(a);
    if (a > max_abs) max_abs = a;
  }

  Result r;
  r.pnl = pnl.back();
  r.mean_abs_inv = abs_inv_sum / c.steps;
  r.max_abs_inv = static_cast<double>(max_abs);
  r.fills = static_cast<double>(s.fills.size());
  // volatility of PnL: standard deviation of 1-minute PnL changes
  { const int w = static_cast<int>(60.0 / c.dt);
    double sum = 0, sq = 0; int n = 0;
    for (int i = w; i < c.steps; i += w) { const double d = pnl[i] - pnl[i - w]; sum += d; sq += d * d; ++n; }
    if (n > 1) r.sd_1m = std::sqrt((sq - sum * sum / n) / (n - 1)); }
  // Fill edges, per unit, from our side. quoted > realized means the fills
  // we got were, on average, the ones where the price had already moved
  // against us: adverse selection.
  { const int h10 = static_cast<int>(10 / c.dt);
    double qa = 0, q10 = 0, qn = 0, qi = 0, eq = 0, er = 0, e10 = 0, en = 0, ei = 0;
    for (const MmFill& f : s.fills) {
      const double q = f.qty, px = static_cast<double>(f.px);
      const double real = f.sign * (f.value - px) * q;
      qa += q; eq += f.sign * (f.ref - px) * q; er += real;
      if (f.taker == INFORMED) { qi += q; ei += real; } else { qn += q; en += real; }
      if (f.step + h10 < c.steps) { e10 += f.sign * (val[f.step + h10] - px) * q; q10 += q; }
    }
    r.volume = qa;
    if (qa > 0) { r.quoted = eq / qa; r.realized = er / qa; r.inf_share = qi / qa; }
    if (q10 > 0) r.realized10 = e10 / q10;
    if (qn > 0) r.edge_noise = en / qn;
    if (qi > 0) r.edge_inf = ei / qi; }

  if (path_csv) {
    if (FILE* f = std::fopen(path_csv, "w")) {
      std::fprintf(f, "t,value,mid,inventory,pnl\n");
      const int every = static_cast<int>(1 / c.dt);
      for (int i = 0; i < c.steps; i += every)
        std::fprintf(f, "%.1f,%.3f,%.1f,%lld,%.1f\n", i * c.dt, val[i], mid[i], (long long)invs[i], pnl[i]);
      std::fclose(f);
    }
  }
  return r;
}

struct Agg { Result mean; double pnl_sd = 0; };

static std::vector<double*> fields(Result& r) {
  return {&r.pnl, &r.sd_1m, &r.mean_abs_inv, &r.max_abs_inv, &r.fills, &r.volume, &r.quoted,
          &r.realized, &r.realized10, &r.edge_noise, &r.edge_inf, &r.inf_share};
}

static Agg sweep(const Cfg& c, const AsParams& p, int seeds) {
  Agg a;
  double sq = 0;
  auto dst = fields(a.mean);
  for (int s = 1; s <= seeds; ++s) {
    Result r = run(c, p, static_cast<uint64_t>(s), nullptr);
    auto src = fields(r);
    for (size_t i = 0; i < dst.size(); ++i) *dst[i] += *src[i];
    sq += r.pnl * r.pnl;
  }
  const double n = seeds;
  a.pnl_sd = seeds > 1 ? std::sqrt((sq - a.mean.pnl * a.mean.pnl / n) / (n - 1)) : 0;
  for (double* v : dst) *v /= n;
  return a;
}

static void header(const char* first) {
  std::printf("%-7s %9s %8s %6s %7s %7s %7s %8s %7s %8s %7s %7s %6s\n", first, "meanPnL", "sdPnL", "ratio",
              "sd1min", "avg|q|", "max|q|", "volume", "quoted", "realized", "vsNoise", "vsInf", "inf%");
}
static void line(double x, const Agg& a, FILE* csv) {
  const Result& m = a.mean;
  std::printf("%-7.3g %9.0f %8.0f %6.2f %7.1f %7.1f %7.1f %8.0f %7.3f %8.3f %7.3f %7.3f %6.1f\n", x, m.pnl,
              a.pnl_sd, a.pnl_sd > 0 ? m.pnl / a.pnl_sd : 0, m.sd_1m, m.mean_abs_inv, m.max_abs_inv, m.volume,
              m.quoted, m.realized, m.edge_noise, m.edge_inf, 100 * m.inf_share);
  if (csv)
    std::fprintf(csv, "%g,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", x, m.pnl, a.pnl_sd,
                 m.sd_1m, m.mean_abs_inv, m.max_abs_inv, m.fills, m.volume, m.quoted, m.realized, m.realized10,
                 m.edge_noise, m.edge_inf, m.inf_share);
}
static const char* kCsvCols = "mean_pnl,sd_pnl,sd_1min,mean_abs_inv,max_abs_inv,fills,volume,quoted_edge,"
                              "realized_edge,realized_edge_10s,edge_vs_noise,edge_vs_informed,informed_share";

int main(int argc, char** argv) {
  const int seeds = static_cast<int>(arg_int(argc, argv, "--seeds", 40));
  const std::string out = arg_str(argc, argv, "--out", "out");
  ::mkdir(out.c_str(), 0755);
  Cfg c;
  c.steps = static_cast<int>(arg_int(argc, argv, "--steps", c.steps));
  c.sigma = arg_dbl(argc, argv, "--sigma", c.sigma);
  c.inf_rate = arg_dbl(argc, argv, "--inf-rate", c.inf_rate);
  c.noise_rate = arg_dbl(argc, argv, "--noise-rate", c.noise_rate);
  c.mm_every = static_cast<int>(arg_int(argc, argv, "--mm-every", c.mm_every));
  AsParams base;
  base.k = arg_dbl(argc, argv, "--k", base.k);
  base.tau = arg_dbl(argc, argv, "--tau", base.tau);
  base.max_inv = arg_int(argc, argv, "--max-inv", base.max_inv);

  std::printf("Market: sigma %.1f ticks/sqrt(s), noise %.2f/s, informed %.2f/s, maker re-quotes every %.1f s\n",
              c.sigma, c.noise_rate, c.inf_rate, c.mm_every * c.dt);
  std::printf("%d seeds x %.0f simulated minutes per row\n\n", seeds, c.steps * c.dt / 60);

  std::printf("Sweep 1: risk aversion gamma (gamma = 0 is a symmetric quoter with no inventory skew)\n");
  header("gamma");
  FILE* f1 = std::fopen((out + "/gamma_sweep.csv").c_str(), "w");
  if (f1) std::fprintf(f1, "gamma,%s\n", kCsvCols);
  for (double g : {0.0, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5}) {
    AsParams p = base;
    p.gamma = g;
    line(g, sweep(c, p, seeds), f1);
  }
  if (f1) std::fclose(f1);

  std::printf("\nSweep 2: informed-trader arrival rate (gamma = 0.05)\n");
  header("inf/s");
  FILE* f2 = std::fopen((out + "/toxicity_sweep.csv").c_str(), "w");
  if (f2) std::fprintf(f2, "inf_rate,%s\n", kCsvCols);
  for (double ir : {0.0, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0}) {
    Cfg c2 = c;
    c2.inf_rate = ir;
    AsParams p = base;
    p.gamma = 0.05;
    line(ir, sweep(c2, p, seeds), f2);
  }
  if (f2) std::fclose(f2);

  // One sample path per strategy (same seed) for the charts.
  for (double g : {0.0, 0.05}) {
    AsParams p = base;
    p.gamma = g;
    char name[256];
    std::snprintf(name, sizeof(name), "%s/path_gamma_%g.csv", out.c_str(), g);
    run(c, p, 1, name);
  }
  std::printf("\nwrote %s/gamma_sweep.csv, toxicity_sweep.csv, path_gamma_*.csv\n", out.c_str());
  std::printf("PnL in ticks x units, marked to true value. ratio = meanPnL / sdPnL across seeds.\n"
              "quoted / realized / vsNoise / vsInf: edge per unit traded, in ticks. inf%%: share of volume vs informed.\n");
  return 0;
}
