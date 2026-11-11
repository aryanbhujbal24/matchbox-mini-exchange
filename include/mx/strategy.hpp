#pragma once
// Avellaneda-Stoikov market making (Avellaneda & Stoikov, 2008,
// "High-frequency trading in a limit order book").
//
//   reservation price  r = s - q * gamma * sigma^2 * tau
//   total spread       d = gamma * sigma^2 * tau + (2/gamma) * ln(1 + gamma/k)
//
//   s      reference mid price (ticks)
//   q      signed inventory
//   gamma  risk aversion. gamma -> 0 gives r = s, d = 2/k: a symmetric quoter.
//   sigma^2 variance of the mid, ticks^2 per second
//   tau    risk horizon in seconds (held constant: continuous trading)
//   k      how fast fill intensity decays with distance from the mid, per tick
//
// The quoter leans its quotes against inventory: long -> both quotes move
// down, so it is more likely to sell than to buy.
#include <cmath>
#include <cstdint>
#include "types.hpp"

namespace mx {

struct AsParams {
  double  gamma   = 0.1;
  double  k       = 0.5;
  double  tau     = 1.0;
  Qty     size    = 5;
  int64_t max_inv = 100;  // hard cap: stop quoting the side that would breach it
};

struct Quote {
  Price bid = 0, ask = 0;
  bool bid_on = false, ask_on = false;
  double reservation = 0, spread = 0;
};

// EWMA estimate of mid-price variance per second.
class VolEstimator {
 public:
  explicit VolEstimator(double init_var = 1.0, double time_const_s = 60.0)
      : var_(init_var), tc_(time_const_s) {}
  void update(double mid, double t_sec) {
    if (has_last_) {
      const double dt = t_sec - last_t_;
      if (dt <= 0) return;
      const double d = mid - last_mid_;
      const double a = 1.0 - std::exp(-dt / tc_);
      var_ += a * (d * d / dt - var_);
    }
    last_mid_ = mid;
    last_t_ = t_sec;
    has_last_ = true;
  }
  double var_per_sec() const { return var_; }

 private:
  double var_, tc_, last_mid_ = 0, last_t_ = 0;
  bool has_last_ = false;
};

// best_bid / best_ask describe the market WITHOUT our own quotes.
inline Quote as_quote(const AsParams& p, double mid, int64_t inv, double var,
                      Price best_bid, Price best_ask) {
  Quote q;
  q.reservation = mid - static_cast<double>(inv) * p.gamma * var * p.tau;
  q.spread = p.gamma * var * p.tau +
             (p.gamma > 1e-12 ? (2.0 / p.gamma) * std::log1p(p.gamma / p.k) : 2.0 / p.k);
  // Round to the nearest tick. (floor/ceil would silently widen the spread
  // by up to a tick whenever the reservation price is fractional.)
  q.bid = static_cast<Price>(std::floor(q.reservation - q.spread / 2 + 0.5));
  q.ask = static_cast<Price>(std::floor(q.reservation + q.spread / 2 + 0.5));
  // Stay passive: never quote through the other side of the market.
  if (q.bid >= best_ask) q.bid = best_ask - 1;
  if (q.ask <= best_bid) q.ask = best_bid + 1;
  if (q.ask <= q.bid) q.ask = q.bid + 1;
  q.bid_on = inv < p.max_inv;
  q.ask_on = inv > -p.max_inv;
  return q;
}

}  // namespace mx
