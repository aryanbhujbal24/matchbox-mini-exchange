#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "mx/engine.hpp"
#include "mx/journal.hpp"
#include "mx/l2_book.hpp"
#include "mx/spsc_queue.hpp"
using namespace mx;

// Records everything the engine emits, and mirrors the public feed into an L2Book.
struct RecSink {
  struct Ev { char kind; uint32_t client; uint64_t a; uint64_t b; Price px; Qty qty; Qty leaves; };
  std::vector<Ev> ev;
  L2Book l2{2000};
  uint64_t traded = 0;
  bool record = true;
  void on_ack(uint32_t c, uint64_t cl, OrderId oid) { if (record) ev.push_back({'A', c, cl, oid, 0, 0, 0}); }
  void on_reject(uint32_t c, uint64_t ref, RejectReason r) { if (record) ev.push_back({'R', c, ref, uint64_t(r), 0, 0, 0}); }
  void on_fill(uint32_t c, OrderId oid, Price px, Qty q, Qty leaves, Side, bool maker) {
    if (record) ev.push_back({maker ? 'M' : 'T', c, oid, 0, px, q, leaves});
  }
  void on_canceled(uint32_t c, OrderId oid, Qty q, CancelReason r) { if (record) ev.push_back({'C', c, oid, uint64_t(r), 0, q, 0}); }
  void on_trade(Price, Qty q, Side) { traded += q; }
  void on_level(Side s, Price px, Qty q) { l2.apply(s, px, q); }
};

static uint64_t checksum(const Engine<RecSink>& e) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
  e.book().for_each_order([&](OrderId id, Side s, Price p, Qty q) {
    mix(e.order_id_of(id)); mix(uint64_t(s)); mix(uint64_t(p)); mix(q); mix(e.owner_of(id));
  });
  mix(uint64_t(e.book().best_bid())); mix(uint64_t(e.book().best_ask()));
  return h;
}

static Command rand_cmd(std::mt19937_64& rng, std::vector<OrderId>& known, uint64_t i) {
  Command c{};
  c.client = 1 + rng() % 3;
  if (!known.empty() && rng() % 100 < 35) {
    c.type = uint8_t(rng() % 4 == 0 ? CmdType::Reduce : CmdType::Cancel);
    c.qty = 1 + rng() % 30;
    c.ref = known[rng() % known.size()];  // may be stale, filled, or someone else's
  } else {
    c.type = uint8_t(CmdType::New);
    c.ref = i;
    c.side = rng() & 1;
    c.price = 1000 + Price(rng() % 60) - 30;
    c.qty = 1 + rng() % 50;
    c.tif = (rng() % 10 == 0) ? 1 : 0;
    c.ord_type = (rng() % 20 == 0) ? 1 : 0;
    if (rng() % 500 == 0) c.qty = 0;        // invalid on purpose
    if (rng() % 500 == 0) c.price = 999999; // invalid on purpose
  }
  return c;
}

int main() {
  // --- SPSC queue: every item arrives once, in order, across threads ---
  { SpscQueue<uint64_t> q(1024);
    const uint64_t N = 2'000'000;
    uint64_t sum = 0; bool ordered = true;
    std::thread cons([&] {
      uint64_t v, expect = 0;
      while (expect < N) if (q.pop(v)) { ordered &= (v == expect); sum += v; ++expect; }
    });
    for (uint64_t i = 0; i < N;) if (q.push(i)) ++i;
    cons.join();
    assert(ordered && sum == N * (N - 1) / 2); }

  // --- engine semantics ---
  { RecSink s; Engine<RecSink> e(2000, 8, s);
    e.new_order(1, 100, Side::Sell, 1000, 10, Tif::GTC, OrdType::Limit);
    assert(s.ev.size() == 1 && s.ev[0].kind == 'A');
    OrderId sell = s.ev[0].b;
    // someone else cannot cancel it
    e.cancel(2, sell);
    assert(s.ev.back().kind == 'R' && s.ev.back().b == uint64_t(RejectReason::NotOwner));
    // partial fill: maker report, taker report, both with correct leaves
    s.ev.clear();
    e.new_order(2, 200, Side::Buy, 1000, 4, Tif::GTC, OrdType::Limit);
    assert(s.ev.size() == 3);
    assert(s.ev[0].kind == 'A' && s.ev[0].client == 2);
    assert(s.ev[1].kind == 'M' && s.ev[1].client == 1 && s.ev[1].a == sell && s.ev[1].qty == 4 && s.ev[1].leaves == 6);
    assert(s.ev[2].kind == 'T' && s.ev[2].client == 2 && s.ev[2].leaves == 0);
    assert(s.l2.qty(Side::Sell, 1000) == 6);
    // IOC takes what is there, the rest is cancelled, nothing rests
    s.ev.clear();
    e.new_order(2, 201, Side::Buy, 1000, 10, Tif::IOC, OrdType::Limit);
    assert(s.ev.back().kind == 'C' && s.ev.back().qty == 4 && s.ev.back().b == uint64_t(CancelReason::IocRemainder));
    assert(e.book().order_count() == 0 && !s.l2.has_ask() && !s.l2.has_bid());
    // the filled order's id is dead
    e.cancel(1, sell);
    assert(s.ev.back().kind == 'R' && s.ev.back().b == uint64_t(RejectReason::UnknownOrder));
    // market order into an empty book
    s.ev.clear();
    e.new_order(3, 300, Side::Sell, 0, 5, Tif::IOC, OrdType::Market);
    assert(s.ev.size() == 2 && s.ev[1].kind == 'C' && s.ev[1].qty == 5);
    // reduce keeps queue priority; reducing by everything is a cancel
    { RecSink s3; Engine<RecSink> e3(2000, 8, s3);
      e3.new_order(1, 1, Side::Buy, 1000, 10, Tif::GTC, OrdType::Limit); OrderId first = s3.ev.back().b;
      e3.new_order(1, 2, Side::Buy, 1000, 10, Tif::GTC, OrdType::Limit); OrderId second = s3.ev.back().b;
      e3.reduce(2, first, 4);
      assert(s3.ev.back().kind == 'R');
      e3.reduce(1, first, 4);
      assert(s3.ev.back().kind == 'C' && s3.ev.back().b == uint64_t(CancelReason::Reduced));
      assert(e3.leaves(first) == 6 && s3.l2.qty(Side::Buy, 1000) == 16);
      s3.ev.clear();
      e3.new_order(2, 3, Side::Sell, 1000, 6, Tif::IOC, OrdType::Limit);
      assert(s3.ev[1].kind == 'M' && s3.ev[1].a == first && s3.ev[1].leaves == 0);  // still first in line
      e3.reduce(1, second, 99);
      assert(e3.leaves(second) == 0 && e3.book().order_count() == 0 && !s3.l2.has_bid()); }
    // validation
    e.new_order(3, 301, Side::Buy, 1000, 0, Tif::GTC, OrdType::Limit);
    assert(s.ev.back().b == uint64_t(RejectReason::BadQty));
    e.new_order(3, 302, Side::Buy, 2000, 1, Tif::GTC, OrdType::Limit);
    assert(s.ev.back().b == uint64_t(RejectReason::BadPrice));
    e.new_order(3, 303, static_cast<Side>(7), 1000, 1, Tif::GTC, OrdType::Limit);
    assert(s.ev.back().b == uint64_t(RejectReason::BadMessage));
    // a recycled slot gets a new id: the old id stays invalid
    s.ev.clear();
    e.new_order(1, 400, Side::Buy, 900, 1, Tif::GTC, OrdType::Limit);
    OrderId a = s.ev.back().b;
    e.cancel(1, a);
    e.new_order(1, 401, Side::Buy, 900, 1, Tif::GTC, OrdType::Limit);
    OrderId b = s.ev.back().b;
    assert(uint32_t(a) == uint32_t(b) && a != b);
    e.cancel(1, a);
    assert(s.ev.back().kind == 'R' && e.book().order_count() == 1);
    // book full
    for (int i = 0; i < 7; ++i) e.new_order(1, 500 + i, Side::Buy, 800, 1, Tif::GTC, OrdType::Limit);
    assert(e.book().order_count() == 8);
    e.new_order(1, 600, Side::Buy, 800, 1, Tif::GTC, OrdType::Limit);
    assert(s.ev.back().b == uint64_t(RejectReason::BookFull)); }

  // --- randomized: feed integrity + journal replay determinism ---
  { const char* path = "/tmp/mx_test_journal.bin";
    ::unlink(path);
    const uint64_t N = 300'000;
    std::mt19937_64 rng(7);
    RecSink s; s.record = false;
    Engine<RecSink> e(2000, 1 << 16, s);
    Journal j; assert(j.open(path, false));
    std::vector<OrderId> known;
    std::vector<Command> batch;
    uint64_t in_qty = 0;
    for (uint64_t i = 0; i < N; ++i) {
      Command c = rand_cmd(rng, known, i);
      batch.push_back(c);
      if (batch.size() == 64 || i + 1 == N) {
        assert(j.append(batch.data(), batch.size()));
        for (auto& b : batch) {
          s.record = true; s.ev.clear();
          apply(e, b);
          for (auto& ev : s.ev) if (ev.kind == 'A') { known.push_back(ev.b); in_qty += b.qty; }
          s.record = false;
          if (known.size() > 2000) known.erase(known.begin(), known.begin() + 1000);
        }
        batch.clear();
      }
    }
    // 1) the public L2 feed reproduces the book exactly
    for (Price p = 0; p < 2000; ++p) {
      assert(s.l2.qty(Side::Buy, p) == e.book().level_qty(Side::Buy, p));
      assert(s.l2.qty(Side::Sell, p) == e.book().level_qty(Side::Sell, p));
    }
    assert(s.l2.best_bid() == e.book().best_bid() && s.l2.best_ask() == e.book().best_ask());
    // 2) the book is never crossed
    assert(!e.book().has_bid() || !e.book().has_ask() || e.book().best_bid() < e.book().best_ask());
    // 3) replaying the journal rebuilds the identical state, ids included
    { int fd = ::open(path, O_WRONLY | O_APPEND); assert(fd >= 0);
      assert(::write(fd, "torn-write", 10) == 10); ::close(fd); }  // simulate a crash mid-record
    RecSink s2; s2.record = false;
    Engine<RecSink> e2(2000, 1 << 16, s2);
    size_t n = Journal::replay(path, [&](const Command& c) { apply(e2, c); });
    assert(n == N);
    assert(checksum(e) == checksum(e2));
    assert(e.book().order_count() == e2.book().order_count());
    assert(s.traded == s2.traded && s.traded > 0);
    std::printf("randomized: %llu cmds, %zu resting orders, %llu qty traded, replay checksum match\n",
                (unsigned long long)N, e.book().order_count(), (unsigned long long)s.traded);
    ::unlink(path); }

  std::puts("all engine tests passed");
  return 0;
}
