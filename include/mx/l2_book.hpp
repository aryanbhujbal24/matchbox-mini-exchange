#pragma once
// Client-side price-level book rebuilt from the market data feed.
// Level updates are absolute (total quantity at a price), so applying them
// is idempotent and a single lost update self-heals on the next change.
#include <vector>
#include "types.hpp"

namespace mx {

class L2Book {
 public:
  explicit L2Book(Price max_price)
      : max_(max_price), bb_(-1), ba_(max_price) {
    q_[0].assign(static_cast<size_t>(max_price), 0);
    q_[1].assign(static_cast<size_t>(max_price), 0);
  }

  void apply(Side side, Price px, Qty qty) {
    if (px < 0 || px >= max_) return;
    const int i = static_cast<int>(side);
    q_[i][px] = qty;
    if (side == Side::Buy) {
      if (qty > 0) { if (px > bb_) bb_ = px; }
      else if (px == bb_) { while (bb_ >= 0 && q_[0][bb_] == 0) --bb_; }
    } else {
      if (qty > 0) { if (px < ba_) ba_ = px; }
      else if (px == ba_) { while (ba_ < max_ && q_[1][ba_] == 0) ++ba_; }
    }
  }

  Price best_bid() const { return bb_; }           // -1 if empty
  Price best_ask() const { return ba_; }           // max_price if empty
  bool has_bid() const { return bb_ >= 0; }
  bool has_ask() const { return ba_ < max_; }
  Price max_price() const { return max_; }
  Qty qty(Side side, Price px) const { return q_[static_cast<int>(side)][px]; }

  // Best prices with `own_qty` at `own_px` removed: lets a market maker
  // see the market without its own quotes in it.
  Price best_bid_excl(Price own_px, Qty own_qty) const {
    for (Price p = bb_; p >= 0; --p) {
      int64_t q = q_[0][p];
      if (p == own_px) q -= own_qty;
      if (q > 0) return p;
    }
    return -1;
  }
  Price best_ask_excl(Price own_px, Qty own_qty) const {
    for (Price p = ba_; p < max_; ++p) {
      int64_t q = q_[1][p];
      if (p == own_px) q -= own_qty;
      if (q > 0) return p;
    }
    return max_;
  }

 private:
  std::vector<Qty> q_[2];
  Price max_, bb_, ba_;
};

}  // namespace mx
