#pragma once
// Matching engine: wraps the OrderBook with everything a venue needs around
// it: validation, order ownership, exchange order ids, execution reports and
// market data events.
//
// The engine is single-threaded and deterministic: its output is a pure
// function of the sequence of Commands applied. That property is what makes
// journal replay (crash recovery) and the backtest possible.
//
// Sink is any type with these members (all called synchronously):
//   on_ack(client, cl_ord_id, order_id)
//   on_reject(client, ref, RejectReason)            ref = cl_ord_id or order_id
//   on_fill(client, order_id, price, qty, leaves, side, is_maker)
//   on_canceled(client, order_id, qty, CancelReason)
//   on_trade(price, qty, aggressor_side)            public market data
//   on_level(side, price, total_qty)                public market data (absolute)
#include <cstdint>
#include <vector>
#include "order_book.hpp"
#include "protocol.hpp"

namespace mx {

enum class CmdType : uint8_t { New = 1, Cancel = 2, Reduce = 3 };

// One sequenced input to the engine. Also the journal record format.
struct Command {
  uint8_t  type;      // CmdType
  uint8_t  side;
  uint8_t  tif;
  uint8_t  ord_type;
  uint32_t client;
  uint64_t ref;       // New: cl_ord_id.  Cancel / Reduce: exchange order id.
  int64_t  price;
  uint32_t qty;
  uint32_t pad;
};
static_assert(sizeof(Command) == 32);

template <class Sink>
class Engine {
 public:
  Engine(Price max_price, uint32_t max_orders, Sink& sink)
      : book_(max_price, max_orders), meta_(max_orders + 1), sink_(sink), max_price_(max_price) {
    free_.reserve(max_orders);
    for (uint32_t s = max_orders; s >= 1; --s) free_.push_back(s);
  }

  void new_order(uint32_t client, uint64_t cl_ord_id, Side side, Price px, Qty qty,
                 Tif tif, OrdType type) {
    if (static_cast<uint8_t>(side) > 1 || static_cast<uint8_t>(tif) > 1 ||
        static_cast<uint8_t>(type) > 1)
      return sink_.on_reject(client, cl_ord_id, RejectReason::BadMessage);
    if (qty == 0 || qty > kMaxQty)
      return sink_.on_reject(client, cl_ord_id, RejectReason::BadQty);
    if (type == OrdType::Limit && (px <= 0 || px >= max_price_))
      return sink_.on_reject(client, cl_ord_id, RejectReason::BadPrice);
    if (free_.empty())
      return sink_.on_reject(client, cl_ord_id, RejectReason::BookFull);

    // Exchange order id = generation (high 32 bits) | slot (low 32 bits).
    // The slot indexes our tables directly; the generation makes a recycled
    // slot's old id invalid, so a stale cancel can never hit a new order.
    const uint32_t slot = free_.back();
    free_.pop_back();
    if (++gen_ == 0) ++gen_;
    const OrderId oid = (static_cast<uint64_t>(gen_) << 32) | slot;
    meta_[slot] = Meta{oid, px, client, side};
    sink_.on_ack(client, cl_ord_id, oid);

    const Side opp = (side == Side::Buy) ? Side::Sell : Side::Buy;
    Qty left = qty;
    Price last_px = -1;
    auto on_fill = [&](const Fill& f) {
      const uint32_t mslot = static_cast<uint32_t>(f.maker_id);
      const Meta& m = meta_[mslot];
      left -= f.qty;
      if (f.price != last_px) {  // finished a level: publish its final depth once
        if (last_px >= 0) sink_.on_level(opp, last_px, book_.level_qty(opp, last_px));
        last_px = f.price;
      }
      sink_.on_fill(m.client, m.oid, f.price, f.qty, f.maker_left, opp, true);
      sink_.on_fill(client, oid, f.price, f.qty, left, side, false);
      sink_.on_trade(f.price, f.qty, side);
      if (f.maker_left == 0) release(mslot);
    };

    const bool rests = (type == OrdType::Limit && tif == Tif::GTC);
    Qty rem;
    if (rests) {
      rem = book_.add_limit(slot, side, px, qty, on_fill);
    } else {
      const Price limit = (type == OrdType::Market) ? (side == Side::Buy ? max_price_ - 1 : 0) : px;
      rem = book_.add_ioc(slot, side, limit, qty, on_fill);
    }
    if (last_px >= 0) sink_.on_level(opp, last_px, book_.level_qty(opp, last_px));

    if (rem == 0) {
      release(slot);
    } else if (rests) {
      sink_.on_level(side, px, book_.level_qty(side, px));
    } else {
      sink_.on_canceled(client, oid, rem, CancelReason::IocRemainder);
      release(slot);
    }
  }

  void cancel(uint32_t client, OrderId oid) {
    const uint32_t slot = static_cast<uint32_t>(oid);
    if (slot == 0 || slot >= meta_.size() || meta_[slot].oid != oid)
      return sink_.on_reject(client, oid, RejectReason::UnknownOrder);
    const Meta m = meta_[slot];
    if (m.client != client) return sink_.on_reject(client, oid, RejectReason::NotOwner);
    Qty left = 0;
    book_.cancel(slot, &left);
    sink_.on_canceled(client, oid, left, CancelReason::User);
    sink_.on_level(m.side, m.px, book_.level_qty(m.side, m.px));
    release(slot);
  }

  // Cancel `by` units of a resting order, keeping its place in the queue.
  // Cancelling the whole remaining quantity (or more) is a plain cancel.
  void reduce(uint32_t client, OrderId oid, Qty by) {
    const uint32_t slot = static_cast<uint32_t>(oid);
    if (slot == 0 || slot >= meta_.size() || meta_[slot].oid != oid)
      return sink_.on_reject(client, oid, RejectReason::UnknownOrder);
    const Meta m = meta_[slot];
    if (m.client != client) return sink_.on_reject(client, oid, RejectReason::NotOwner);
    if (by == 0) return sink_.on_reject(client, oid, RejectReason::BadQty);
    if (by >= book_.qty_of(slot)) return cancel(client, oid);
    book_.reduce(slot, by);
    sink_.on_canceled(client, oid, by, CancelReason::Reduced);
    sink_.on_level(m.side, m.px, book_.level_qty(m.side, m.px));
  }

  // Resting quantity of an order, 0 if the id is not live.
  Qty leaves(OrderId oid) const {
    const uint32_t slot = static_cast<uint32_t>(oid);
    if (slot == 0 || slot >= meta_.size() || meta_[slot].oid != oid) return 0;
    return book_.qty_of(slot);
  }

  const OrderBook& book() const { return book_; }

  // Exchange order id for a book id (as seen in OrderBook::for_each_order).
  OrderId order_id_of(OrderId book_id) const { return meta_[book_id].oid; }
  uint32_t owner_of(OrderId book_id) const { return meta_[book_id].client; }

 private:
  struct Meta {
    OrderId  oid = 0;   // 0 = slot not in use
    Price    px = 0;
    uint32_t client = 0;
    Side     side = Side::Buy;
  };

  void release(uint32_t slot) {
    meta_[slot].oid = 0;
    free_.push_back(slot);
  }

  OrderBook book_;
  std::vector<Meta> meta_;
  std::vector<uint32_t> free_;
  Sink& sink_;
  Price max_price_;
  uint32_t gen_ = 0;
};

// Dispatch a sequenced Command into an engine.
template <class E>
inline void apply(E& e, const Command& c) {
  if (c.type == static_cast<uint8_t>(CmdType::New))
    e.new_order(c.client, c.ref, static_cast<Side>(c.side), c.price, c.qty,
                static_cast<Tif>(c.tif), static_cast<OrdType>(c.ord_type));
  else if (c.type == static_cast<uint8_t>(CmdType::Cancel))
    e.cancel(c.client, c.ref);
  else if (c.type == static_cast<uint8_t>(CmdType::Reduce))
    e.reduce(c.client, c.ref, c.qty);
}

}  // namespace mx
