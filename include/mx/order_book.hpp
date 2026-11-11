#pragma once
// Limit order book, price-time priority.
//
// Design (allocation-free on the hot path):
//  - Orders live in a preallocated pool, linked by index (not pointers).
//  - Each price level is an intrusive FIFO list of order indices plus a
//    running total quantity, so depth queries are O(1).
//  - Prices are integer ticks inside [0, max_price); levels are a flat array,
//    so lookup is O(1) and cache friendly. Best bid/ask are tracked, and a
//    bitmap of non-empty levels finds the next best when a level empties.
//  - Order id -> pool slot via a direct table; ids must lie in [1, max_orders].
//    The Engine hands out and recycles ids in that range.
#include <cstddef>
#include <vector>
#include "types.hpp"

// Build with -DMX_PROFILE to keep the book's internal steps as separate
// functions, so they show up as their own frames in a flame graph. In a
// normal build they inline into the caller.
#ifdef MX_PROFILE
#define MX_PROF_NOINLINE __attribute__((noinline))
#else
#define MX_PROF_NOINLINE
#endif

namespace mx {

// Two-level bitmap over price levels: bit i of w_ says level i is non-empty,
// bit j of s_ says word j of w_ is non-zero. Finding the next non-empty
// level is a couple of mask + count-zeros instructions instead of a walk
// across empty levels.
class LevelBitmap {
 public:
  explicit LevelBitmap(size_t n)
      : n_(static_cast<int64_t>(n)), w_((n + 63) / 64, 0), s_((w_.size() + 63) / 64, 0) {}

  void set(size_t i) {
    w_[i >> 6] |= 1ull << (i & 63);
    s_[i >> 12] |= 1ull << ((i >> 6) & 63);
  }
  void clear(size_t i) {
    uint64_t& w = w_[i >> 6];
    w &= ~(1ull << (i & 63));
    if (!w) s_[i >> 12] &= ~(1ull << ((i >> 6) & 63));
  }

  // Highest set index <= i, or -1.
  int64_t prev(int64_t i) const {
    if (i < 0) return -1;
    size_t wi = static_cast<size_t>(i) >> 6;
    const uint64_t w = w_[wi] & (~0ull >> (63 - (i & 63)));
    if (w) return static_cast<int64_t>((wi << 6) + 63 - static_cast<size_t>(__builtin_clzll(w)));
    size_t si = wi >> 6;
    uint64_t sm = s_[si] & ((1ull << (wi & 63)) - 1);
    for (;;) {
      if (sm) {
        wi = (si << 6) + 63 - static_cast<size_t>(__builtin_clzll(sm));
        return static_cast<int64_t>((wi << 6) + 63 - static_cast<size_t>(__builtin_clzll(w_[wi])));
      }
      if (si == 0) return -1;
      sm = s_[--si];
    }
  }

  // Lowest set index >= i, or -1.
  int64_t next(int64_t i) const {
    if (i >= n_) return -1;
    size_t wi = static_cast<size_t>(i) >> 6;
    const uint64_t w = w_[wi] & (~0ull << (i & 63));
    if (w) return static_cast<int64_t>((wi << 6) + static_cast<size_t>(__builtin_ctzll(w)));
    size_t si = wi >> 6;
    const size_t bit = wi & 63;
    uint64_t sm = (bit == 63) ? 0 : s_[si] & (~0ull << (bit + 1));
    for (;;) {
      if (sm) {
        wi = (si << 6) + static_cast<size_t>(__builtin_ctzll(sm));
        return static_cast<int64_t>((wi << 6) + static_cast<size_t>(__builtin_ctzll(w_[wi])));
      }
      if (++si >= s_.size()) return -1;
      sm = s_[si];
    }
  }

 private:
  int64_t n_;
  std::vector<uint64_t> w_, s_;
};

class OrderBook {
 public:
  static constexpr uint32_t NIL = 0xFFFFFFFFu;

  OrderBook(Price max_price, size_t max_orders)
      : levels_(static_cast<size_t>(max_price)),
        pool_(max_orders),
        id_to_slot_(max_orders + 1, NIL),
        nonempty_{LevelBitmap(static_cast<size_t>(max_price)), LevelBitmap(static_cast<size_t>(max_price))},
        max_price_(max_price) {
    for (size_t i = 0; i < max_orders; ++i)
      pool_[i].next = (i + 1 < max_orders) ? static_cast<uint32_t>(i + 1) : NIL;
    free_head_ = max_orders ? 0 : NIL;
    best_bid_ = -1;
    best_ask_ = max_price_;
  }

  // Limit order: match against the opposite side, rest the remainder.
  // Returns the resting quantity. OnFill is a template parameter (not
  // std::function) so the callback inlines into the matching loop.
  template <class OnFill>
  Qty add_limit(OrderId id, Side side, Price px, Qty qty, OnFill&& on_fill) {
    Qty remaining = match(id, side, px, qty, on_fill);
    if (remaining > 0) rest(id, side, px, remaining);
    return remaining;
  }

  // Immediate-or-cancel: match up to `limit`, never rest. Returns unfilled qty.
  template <class OnFill>
  Qty add_ioc(OrderId id, Side side, Price limit, Qty qty, OnFill&& on_fill) {
    return match(id, side, limit, qty, on_fill);
  }

  // Market order: an IOC whose limit is the far end of the book.
  template <class OnFill>
  Qty add_market(OrderId id, Side side, Qty qty, OnFill&& on_fill) {
    return match(id, side, (side == Side::Buy) ? max_price_ - 1 : 0, qty, on_fill);
  }

  // Cancel a resting order. Optionally reports the quantity that was resting.
  MX_PROF_NOINLINE bool cancel(OrderId id, Qty* left = nullptr) {
    uint32_t s = slot_of(id);
    if (s == NIL) return false;
    if (left) *left = pool_[s].qty;
    unlink(s);
    release(s);
    return true;
  }

  // Reduce a resting order's quantity in place, keeping its queue position.
  // `by` must be smaller than the resting quantity (use cancel otherwise).
  bool reduce(OrderId id, Qty by) {
    uint32_t s = slot_of(id);
    if (s == NIL || by >= pool_[s].qty) return false;
    pool_[s].qty -= by;
    levels_[pool_[s].px].total[static_cast<int>(pool_[s].side)] -= by;
    return true;
  }

  // Resting quantity of an order, 0 if it is not in the book.
  Qty qty_of(OrderId id) const {
    uint32_t s = slot_of(id);
    return s == NIL ? 0 : pool_[s].qty;
  }

  // Id of the order at the front of the queue at a level, 0 if the level is empty.
  OrderId front_id(Side side, Price px) const {
    const uint32_t s = levels_[px].head[static_cast<int>(side)];
    return s == NIL ? 0 : pool_[s].id;
  }

  // Visit the orders resting at one level, front of the queue first.
  template <class F>
  void for_each_at(Side side, Price px, F&& f) const {
    for (uint32_t s = levels_[px].head[static_cast<int>(side)]; s != NIL; s = pool_[s].next)
      f(pool_[s].id, pool_[s].qty);
  }

  // Next non-empty level beyond px, moving away from the market.
  Price next_bid_below(Price px) const { return nonempty_[0].prev(px - 1); }  // -1 if none
  Price next_ask_above(Price px) const {
    const int64_t n = nonempty_[1].next(px + 1);
    return n < 0 ? max_price_ : n;
  }

  // Modify = cancel + re-add (loses time priority), the standard simple rule.
  template <class OnFill>
  Qty modify(OrderId id, Price new_px, Qty new_qty, OnFill&& on_fill) {
    uint32_t s = slot_of(id);
    if (s == NIL) return 0;
    Side side = pool_[s].side;
    cancel(id);
    return add_limit(id, side, new_px, new_qty, on_fill);
  }

  Price best_bid() const { return best_bid_; }   // -1 if empty
  Price best_ask() const { return best_ask_; }   // max_price if empty
  bool  has_bid() const  { return best_bid_ >= 0; }
  bool  has_ask() const  { return best_ask_ < max_price_; }
  Price max_price() const { return max_price_; }
  size_t order_count() const { return live_; }

  Qty level_qty(Side side, Price px) const { return levels_[px].total[static_cast<int>(side)]; }
  Qty depth_at(Side side, Price px) const { return level_qty(side, px); }

  // Visit every resting order: bids then asks, ascending price, FIFO within a
  // level. O(max_price); for snapshots and tests, not the hot path.
  template <class F>
  void for_each_order(F&& f) const {
    for (int i = 0; i < 2; ++i)
      for (Price p = 0; p < max_price_; ++p)
        for (uint32_t s = levels_[p].head[i]; s != NIL; s = pool_[s].next)
          f(pool_[s].id, static_cast<Side>(i), p, pool_[s].qty);
  }

 private:
  struct Order {
    OrderId  id;
    Price    px;
    Qty      qty;
    Side     side;
    uint32_t prev, next;
  };
  struct Level {
    uint32_t head[2]  = {NIL, NIL};  // [Buy, Sell]
    uint32_t tail[2]  = {NIL, NIL};
    Qty      total[2] = {0, 0};
  };

  uint32_t slot_of(OrderId id) const {
    return id < id_to_slot_.size() ? id_to_slot_[id] : NIL;
  }

  uint32_t alloc() {
    uint32_t s = free_head_;
    if (s != NIL) { free_head_ = pool_[s].next; ++live_; }
    return s;
  }
  void release(uint32_t s) {
    id_to_slot_[pool_[s].id] = NIL;
    pool_[s].next = free_head_;
    free_head_ = s;
    --live_;
  }

  MX_PROF_NOINLINE void rest(OrderId id, Side side, Price px, Qty qty) {
    uint32_t s = alloc();
    if (s == NIL) return;  // cannot happen when the Engine sizes its id space to the pool
    Order& o = pool_[s];
    o = {id, px, qty, side, NIL, NIL};
    id_to_slot_[id] = s;
    Level& L = levels_[px];
    int i = static_cast<int>(side);
    if (L.tail[i] == NIL) { L.head[i] = L.tail[i] = s; nonempty_[i].set(static_cast<size_t>(px)); }
    else { pool_[L.tail[i]].next = s; o.prev = L.tail[i]; L.tail[i] = s; }
    L.total[i] += qty;
    if (side == Side::Buy  && px > best_bid_) best_bid_ = px;
    if (side == Side::Sell && px < best_ask_) best_ask_ = px;
  }

  // Remove from its level list. Subtracts whatever quantity is still on it.
  MX_PROF_NOINLINE void unlink(uint32_t s) {
    Order& o = pool_[s];
    Level& L = levels_[o.px];
    int i = static_cast<int>(o.side);
    L.total[i] -= o.qty;
    if (o.prev != NIL) pool_[o.prev].next = o.next; else L.head[i] = o.next;
    if (o.next != NIL) pool_[o.next].prev = o.prev; else L.tail[i] = o.prev;
    if (L.head[i] == NIL) {
      nonempty_[i].clear(static_cast<size_t>(o.px));
      fix_best_after_empty(o.side, o.px);
    }
  }

  // The best level just emptied: find the next non-empty one.
  // -DMX_LINEAR_BEST restores the original level-by-level walk, kept so the
  // before/after benchmark in docs/benchmarks.md can be reproduced.
  MX_PROF_NOINLINE void fix_best_after_empty(Side side, Price px) {
#ifdef MX_LINEAR_BEST
    if (side == Side::Buy && px == best_bid_) {
      while (best_bid_ >= 0 && levels_[best_bid_].head[0] == NIL) --best_bid_;
    } else if (side == Side::Sell && px == best_ask_) {
      while (best_ask_ < max_price_ && levels_[best_ask_].head[1] == NIL) ++best_ask_;
    }
#else
    if (side == Side::Buy && px == best_bid_) {
      best_bid_ = nonempty_[0].prev(px - 1);
    } else if (side == Side::Sell && px == best_ask_) {
      const int64_t n = nonempty_[1].next(px + 1);
      best_ask_ = n < 0 ? max_price_ : n;
    }
#endif
  }

  template <class OnFill>
  MX_PROF_NOINLINE Qty match(OrderId taker, Side side, Price limit, Qty qty, OnFill& on_fill) {
    const int opp = (side == Side::Buy) ? 1 : 0;
    while (qty > 0) {
      Price px = (side == Side::Buy) ? best_ask_ : best_bid_;
      bool crosses = (side == Side::Buy) ? (has_ask() && px <= limit)
                                         : (has_bid() && px >= limit);
      if (!crosses) break;
      Level& L = levels_[px];
      while (qty > 0 && L.head[opp] != NIL) {
        uint32_t s = L.head[opp];
        Order& m = pool_[s];
        Qty traded = qty < m.qty ? qty : m.qty;
        qty -= traded;
        m.qty -= traded;
        L.total[opp] -= traded;
        const OrderId maker = m.id;
        const Qty maker_left = m.qty;
        if (maker_left == 0) { unlink(s); release(s); }
        on_fill(Fill{maker, taker, px, traded, maker_left});
      }
    }
    return qty;
  }

  std::vector<Level> levels_;
  std::vector<Order> pool_;
  std::vector<uint32_t> id_to_slot_;
  LevelBitmap nonempty_[2];  // [Buy, Sell]
  Price max_price_;
  Price best_bid_, best_ask_;
  uint32_t free_head_;
  size_t live_ = 0;
};

}  // namespace mx
