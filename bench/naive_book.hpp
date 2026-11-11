#pragma once
// The "obvious" order book, kept only as a benchmark baseline:
// std::map of price levels, std::list of orders per level, and an
// unordered_map from order id to its list node. Same matching rules as
// mx::OrderBook, but every order is a heap allocation and every lookup is a
// pointer chase.
#include <list>
#include <map>
#include <unordered_map>
#include "mx/types.hpp"

namespace mx {

class NaiveBook {
 public:
  NaiveBook(Price, size_t) {}

  template <class OnFill>
  Qty add_limit(OrderId id, Side side, Price px, Qty qty, OnFill&& on_fill) {
    if (side == Side::Buy) {
      while (qty > 0 && !asks_.empty() && asks_.begin()->first <= px) qty = take(asks_, asks_.begin(), id, qty, on_fill);
      if (qty > 0) rest(bids_, id, side, px, qty);
    } else {
      while (qty > 0 && !bids_.empty() && bids_.begin()->first >= px) qty = take(bids_, bids_.begin(), id, qty, on_fill);
      if (qty > 0) rest(asks_, id, side, px, qty);
    }
    return qty;
  }

  bool cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    const Ref r = it->second;
    if (r.side == Side::Buy) erase(bids_, r); else erase(asks_, r);
    index_.erase(it);
    return true;
  }

 private:
  struct Order { OrderId id; Qty qty; };
  using Level = std::list<Order>;
  struct Ref { Side side; Price px; Level::iterator it; };

  template <class M>
  void rest(M& m, OrderId id, Side side, Price px, Qty qty) {
    Level& l = m[px];
    l.push_back({id, qty});
    index_[id] = Ref{side, px, std::prev(l.end())};
  }
  template <class M>
  void erase(M& m, const Ref& r) {
    auto lv = m.find(r.px);
    lv->second.erase(r.it);
    if (lv->second.empty()) m.erase(lv);
  }
  template <class M, class OnFill>
  Qty take(M& m, typename M::iterator lv, OrderId taker, Qty qty, OnFill& on_fill) {
    Level& l = lv->second;
    while (qty > 0 && !l.empty()) {
      Order& o = l.front();
      const Qty traded = qty < o.qty ? qty : o.qty;
      qty -= traded;
      o.qty -= traded;
      on_fill(Fill{o.id, taker, lv->first, traded, o.qty});
      if (o.qty == 0) { index_.erase(o.id); l.pop_front(); }
    }
    if (l.empty()) m.erase(lv);
    return qty;
  }

  std::map<Price, Level, std::greater<Price>> bids_;
  std::map<Price, Level> asks_;
  std::unordered_map<OrderId, Ref> index_;
};

}  // namespace mx
