# Benchmarks and performance study

## Environment

Shared cloud VM, 2 vCPUs (Intel Xeon @ 2.10 GHz), Linux, GCC 13, `-O3
-march=native`. Everything runs on the same machine over loopback, with no
core isolation. Numbers move by 10 to 30 percent between runs, so every
comparison below was repeated and only differences well outside that range
are called differences. Hardware performance counters are not exposed in
this VM: `perf` sampling works (software clock), `perf stat` cache and branch
counters do not, so cache behaviour is measured with Valgrind's cachegrind
simulator instead.

These are numbers to beat on real hardware, not headline numbers.

## 1. Order book: data structure comparison

`bench_order_book`: 2,000,000 operations, 70% new limit orders at random
prices around the mid, 30% cancels of a random earlier order. The operation
stream is generated before the clock starts. Three implementations, same
stream; all three produce the same traded-quantity checksum.

- **naive**: `std::map` of levels, `std::list` of orders, `unordered_map` index (`bench/naive_book.hpp`)
- **flat, linear**: flat level array, pooled orders, linear walk to the next best price (the original design, `-DMX_LINEAR_BEST`)
- **flat, bitmap**: the same with a two-level bitmap of non-empty levels (current)

Million operations per second, median of 3 runs:

| Price range of the book | naive | flat, linear | flat, bitmap |
|---|---|---|---|
| 200 ticks (dense) | 6.0 | 24.0 | 32.5 |
| 2,000 ticks | 5.3 | 19.7 | 30.4 |
| 20,000 ticks (sparse) | 4.8 | 4.8 | 25.8 |

Per-operation latency with the current book (dense case, two clock reads
around each operation): p50 61 ns, p99 415 ns, p99.9 628 ns.

### How the bitmap came about

The original flat book was 4 to 5 times faster than the naive one on a dense
book, and no faster at all on a sparse one. A `perf` profile of the sparse
case showed why: 63% of the time was in `fix_best_after_empty`, walking
across empty price levels after the best level emptied.

| | before | after |
|---|---|---|
| Flame graph | [flame_book_wide_before.svg](img/flame_book_wide_before.svg) | [flame_book_wide_after.svg](img/flame_book_wide_after.svg) |
| Time in `fix_best_after_empty` | 63% | below 1% |
| Throughput, sparse book | 4.8 M ops/s | 25.8 M ops/s |

Cachegrind on 500,000 operations (whole program, including generating the
operation stream):

| | Instructions per op | L1 data misses per op |
|---|---|---|
| naive, dense | 603 | 5.4 |
| flat bitmap, dense | 249 | 2.9 |
| flat linear, sparse | 3,027 | 138 |
| flat bitmap, sparse | 258 | 3.6 |

The linear walk cost about 3,000 instructions and 138 cache misses per
operation on a sparse book; the bitmap makes the sparse case cost the same as
the dense one. Branch misprediction is about 9% of conditional branches for
both naive and flat, which is expected for random buy/sell flow.

To reproduce the profile: `cmake -B build-prof -DMX_PROFILE=ON` keeps the
book's internal steps as separate functions so they appear as frames (in the
normal build they all inline into the caller and the flame graph is one bar).

## 2. Exchange end to end over TCP

`mx_loadgen`: 70% new orders, 30% cancels. The clock starts when a request is
queued in the client and stops when its response has been read and parsed.
Journal and market data on.

| Engine idle mode | In flight | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| adaptive (default) | 1 | 34 k req/s | 25 us | 89 us | 205 us |
| adaptive | 64 | 0.95 M req/s | 45 us | 208 us | 336 us |
| adaptive | 512 | 1.86 M req/s | 219 us | 515 us | 2.2 ms |
| `--spin` | 1 | 35 k req/s | 23 us | 81 us | 212 us |
| `--spin` | 64 | 1.15 M req/s | 40 us | 132 us | 291 us |
| `--spin` | 512 | 2.65 M req/s | 125 us | 431 us | 987 us |

With one order in flight, the round trip is two loopback TCP hops and two
thread hand-offs. The book operation is about 0.03 us of the 25. With 512 in
flight, latency is mostly queueing behind the rest of the batch.

About 23% of requests are rejected: cancels for orders that were already
filled, which is expected with random crossing flow.

### Where the exchange's CPU time goes

[flame_exchange.svg](img/flame_exchange.svg), sampled at 1.2 M requests/s:

| Thread | Share of process CPU | Mostly |
|---|---|---|
| mx-engine | 43% | queue polling, matching, scheduler |
| mx-md | 41% | queue polling and yielding |
| mx-gateway | 16% | socket I/O, scheduler |

`Engine::new_order` and everything under it is about 8% of the process. The
rest is polling, hand-offs and the kernel scheduler: four busy threads on two
cores.

### Two experiments that did not help

**Market data thread idle strategy.** The profile above suggested the feed
thread was wasting a core on idle polling. Changing it to spin briefly and
then sleep made no measurable difference to latency (p50 21 to 32 us across
six runs before; 24 to 27 us after, plus one outlier run at 61), throughput (1.8 to 2.3 M before, 1.8 to 2.1 M after) or total CPU
time (2.5 to 2.6 s before, 2.5 s after, for the same workload). The change
was reverted. The polling is cheap to give up, so the other threads were not
actually being starved by it.

**CPU pinning.** With `--spin`, pinning the engine to one core and the
gateway plus client to the other gave p50 26 to 30 us against 24 to 26 us
unpinned: no gain. Putting the client on the engine's core without spinning
gave 72 us. Two vCPUs are not enough to give each hot thread its own core,
so this needs repeating on a machine with at least four cores and `isolcpus`.

## 3. Fixed-width binary vs protobuf vs gRPC

`bench_wire` (needs protobuf and gRPC installed; skipped by CMake otherwise).

**Encoding only**, encode + decode in memory:

| Message | Fixed-width | Protobuf 3.21 |
|---|---|---|
| NewOrder | 15 ns, 28 bytes | 49 ns, 11.6 bytes average |
| Ack | 1 ns, 20 bytes | 40 ns, 13.6 bytes average |

The NewOrder rows include about 14 ns of generating field values in both
columns. Fixed-width encoding is a `memcpy`; protobuf costs roughly 35 to 40
ns more per message and produces smaller messages, because small integers
take fewer bytes as varints.

**Round trip over loopback**, one order in flight, 100,000 round trips, each
server feeding the order to a real engine and returning the ack:

| Transport | Round trips/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| Fixed-width on raw TCP | 30,700 | 31 us | 64 us | 165 us |
| Protobuf on raw TCP (length-prefixed) | 28,600 | 33 us | 65 us | 185 us |
| gRPC unary (one RPC per order) | 8,400 | 112 us | 233 us | 597 us |
| gRPC bidirectional stream | 15,300 | 66 us | 158 us | 501 us |

A second run gave the same ordering within 3%.

- Serialization format alone barely matters at this level: protobuf on a raw
  socket costs about 2 us more than fixed-width structs in a 31 us round
  trip. The kernel's TCP path and thread wake-ups dominate.
- The framework matters a lot. gRPC unary is 3.6 times slower than raw TCP at
  the median and gRPC streaming 2.1 times, for the same payload: HTTP/2
  framing, flow control, its own threads and per-call allocation.
- What gRPC buys for that cost: schema evolution, generated clients in many
  languages, deadlines, authentication, load balancing. That is the right
  trade for a control plane (risk limits, reference data, admin) and the
  wrong one for the order path.
- The blocking single-thread server here has a slower median (31 us) than
  the full three-thread exchange (25 us), because it sleeps in `recv()`
  between orders while the exchange's engine thread polls.

## 4. Recovery and feed integrity

- Journal replay: 500,000 commands (16 MB) in 24 ms, restoring 43,866 resting
  orders with the same best bid and ask as before the restart.
- Market data: in every run above the listener received every datagram, 0 gap
  events. That is loopback with an 8 MB receive buffer. It shows the
  sequencing works, not that loss cannot happen.

## 5. Real order flow

`mx_replay` on the LOBSTER AMZN sample day: 269,747 events through the
engine in 0.04 s, about 7 M events/s, including comparing 20 price levels
against the reference book after every event. Loading and parsing the two
CSV files (74 MB) takes 0.4 s, ten times longer than the replay.

## Reproduce

    cmake -B build && cmake --build build -j
    ./build/bench_order_book [--impl naive] [--spread 20000] [--latency]
    cmake -B build-old -DMX_LINEAR_BEST=ON && cmake --build build-old -j   # the "before" book
    ./build/bench_wire

    cmake -B build-prof -DMX_PROFILE=ON && cmake --build build-prof -j
    perf record -F 5000 -g ./build-prof/bench_order_book --n 4000000 --spread 20000
    perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
    perf record -F 3000 -g -p $(pidof mx_exchange) -- sleep 6      # while mx_loadgen runs
    valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes ./build/bench_order_book --n 500000

## Still to do on real hardware

Hardware counters (`perf stat -e cache-misses,branch-misses`), `isolcpus`
with one core per hot thread, HdrHistogram for recording on the hot path, and
kernel-path experiments: `SO_BUSY_POLL`, `sendmmsg` on the feed, `io_uring`
in place of epoll, huge pages for the level array.
