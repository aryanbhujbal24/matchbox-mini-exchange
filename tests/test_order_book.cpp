#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
#include "mx/order_book.hpp"
using namespace mx;

static std::vector<Fill> fills;
static auto cb = [](const Fill& f) { fills.push_back(f); };

int main() {
  // price-time priority
  { OrderBook b(1000, 100); fills.clear();
    b.add_limit(1, Side::Sell, 100, 10, cb);
    b.add_limit(2, Side::Sell, 100, 10, cb);
    b.add_limit(3, Side::Buy, 100, 15, cb);
    assert(fills.size() == 2);
    assert(fills[0].maker_id == 1 && fills[0].qty == 10 && fills[0].maker_left == 0);
    assert(fills[1].maker_id == 2 && fills[1].qty == 5 && fills[1].maker_left == 5);
    assert(b.depth_at(Side::Sell, 100) == 5);
    assert(b.order_count() == 1); }

  // price improvement: buyer pays the resting price
  { OrderBook b(1000, 100); fills.clear();
    b.add_limit(1, Side::Sell, 99, 5, cb);
    b.add_limit(2, Side::Buy, 105, 5, cb);
    assert(fills.size() == 1 && fills[0].price == 99); }

  // cancel + best price tracking
  { OrderBook b(1000, 100); fills.clear();
    b.add_limit(1, Side::Buy, 90, 5, cb);
    b.add_limit(2, Side::Buy, 95, 5, cb);
    assert(b.best_bid() == 95);
    Qty left = 0;
    assert(b.cancel(2, &left) && left == 5);
    assert(b.best_bid() == 90);
    assert(!b.cancel(2));
    assert(b.cancel(1));
    assert(!b.has_bid() && b.level_qty(Side::Buy, 90) == 0); }

  // market order sweeps levels, remainder discarded
  { OrderBook b(1000, 100); fills.clear();
    b.add_limit(1, Side::Sell, 100, 5, cb);
    b.add_limit(2, Side::Sell, 101, 5, cb);
    Qty left = b.add_market(3, Side::Buy, 20, cb);
    assert(left == 10 && fills.size() == 2 && !b.has_ask()); }

  // IOC respects its limit and never rests
  { OrderBook b(1000, 100); fills.clear();
    b.add_limit(1, Side::Sell, 100, 5, cb);
    b.add_limit(2, Side::Sell, 102, 5, cb);
    Qty left = b.add_ioc(3, Side::Buy, 101, 8, cb);
    assert(left == 3 && fills.size() == 1 && b.best_ask() == 102 && !b.has_bid()); }

  // modify loses priority
  { OrderBook b(1000, 100); fills.clear();
    b.add_limit(1, Side::Buy, 100, 5, cb);
    b.add_limit(2, Side::Buy, 100, 5, cb);
    b.modify(1, 100, 5, cb);
    b.add_limit(3, Side::Sell, 100, 5, cb);
    assert(fills[0].maker_id == 2); }

  // level bitmap agrees with a brute-force scan, including across word and summary boundaries
  { const int64_t N = 10000;
    LevelBitmap bm(N);
    std::vector<bool> ref(N, false);
    std::mt19937_64 rng(3);
    auto brute_prev = [&](int64_t i) { for (; i >= 0; --i) if (ref[i]) return i; return int64_t(-1); };
    auto brute_next = [&](int64_t i) { for (; i < N; ++i) if (ref[i]) return i; return int64_t(-1); };
    for (int round = 0; round < 200000; ++round) {
      const int64_t i = (round % 3 == 0) ? int64_t(rng() % N) : int64_t(4000 + rng() % 200);
      if (rng() % 100 < (round < 100000 ? 60 : 40)) { bm.set(i); ref[i] = true; }
      else { bm.clear(i); ref[i] = false; }
      const int64_t q = int64_t(rng() % N);
      assert(bm.prev(q) == brute_prev(q));
      assert(bm.next(q) == brute_next(q));
    }
    assert(bm.prev(-1) == -1 && bm.next(N) == -1); }

  // a book with levels far apart still finds the next best price
  { OrderBook b(100000, 100); fills.clear();
    b.add_limit(1, Side::Buy, 10, 1, cb);
    b.add_limit(2, Side::Buy, 90000, 1, cb);
    b.add_limit(3, Side::Sell, 99999, 1, cb);
    b.add_limit(4, Side::Sell, 90001, 1, cb);
    assert(b.cancel(2) && b.best_bid() == 10);
    assert(b.cancel(4) && b.best_ask() == 99999);
    assert(b.cancel(1) && !b.has_bid());
    assert(b.cancel(3) && !b.has_ask()); }

  std::puts("all order book tests passed");
  return 0;
}
