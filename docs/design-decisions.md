# Design decisions

Each entry: what was chosen, what was rejected, and what it costs.

## D1 Integer tick prices
Prices are `int64` ticks. Floating point breaks equality tests and cannot
index a price level. Cost: none.

## D2 Flat array of price levels, with a bitmap of non-empty levels
`levels_[price]` is a direct index: O(1) and one cache line per level.
Rejected: `std::map<Price, Level>` (a node allocation per level and a pointer
chase per lookup); measured 4 to 5 times slower. The first version walked
level by level to the next best price when the best level emptied. On a
sparse book that walk was 63% of the run time and erased the whole advantage
over `std::map`. A two-level bitmap of non-empty levels replaced it and made
the sparse case 5 times faster (benchmarks.md, section 1). Cost: memory still
grows with the price range.

## D3 Pool allocator and intrusive index lists
All orders live in one preallocated array and link to each other by 32-bit
index. Rejected: `std::list` (allocation per order, 16 bytes of pointers,
scattered memory). Cost: a fixed maximum number of resting orders.

## D4 Template callback for fills
`add_limit` takes the fill handler as a template parameter so it inlines into
the matching loop. Rejected: `std::function` (an indirect call per fill).

## D5 Order id = generation | slot
The low 32 bits index the engine's tables directly; the high 32 bits change
every time a slot is reused. Rejected: a hash map from id to order (slower,
allocates) and plain slot reuse (a late cancel could hit a stranger's order).

## D6 Single-threaded engine
One thread owns the book. Matching is inherently sequential (the next order
depends on the book the previous one left), so locks would add cost without
adding parallelism, and a single writer makes the engine deterministic.
Scaling out means one engine per instrument, not threads inside one book.

## D7 Lock-free SPSC queues between threads
Bounded ring buffers with cache-line-separated head and tail and a cached
copy of the other side's index. Rejected: mutex + condition variable (a
syscall and a possible context switch per message). Cost: exactly one
producer and one consumer per queue, which fixes the thread topology.

## D8 epoll, level-triggered
Rejected: a thread per connection (does not scale, and puts locks around the
engine's input) and edge-triggered epoll (fewer wake-ups, but every handler
must drain to `EAGAIN` or it stalls). Level-triggered is harder to get wrong.

## D9 Fixed-width binary protocol with a length prefix
Parsing is a `memcpy` into a packed struct. Rejected for the core path: text
protocols (FIX tag=value needs a parser and number conversion) and protobuf
(varint decode, allocation, schema evolution we do not need yet). The
measured comparison against protobuf and gRPC is milestone M5.

## D10 TCP for orders, UDP multicast for market data
Orders need reliability and ordering; TCP provides both. Market data goes to
many receivers and stale data is worthless, so retransmission is the wrong
behaviour: UDP lets the publisher never wait for a slow consumer. Cost:
receivers must detect loss themselves (sequence numbers), and full recovery
needs a snapshot channel, which is not built yet.

## D11 TCP_NODELAY on both ends
Nagle's algorithm holds small writes until the previous segment is ACKed.
For 28-byte orders that means tens of milliseconds. Batching is done
explicitly in user space instead (one `send()` per session per loop).

## D12 Write-ahead journal, group commit
The engine logs a whole batch with one `write()` before applying it.
Rejected: logging after matching (a crash could lose an order the client was
told about) and one `write()` per command (a syscall per order). `--fsync`
adds `fdatasync` per batch for power-loss safety at a large latency cost;
the default survives a process crash only.

## D13 Journal the inputs, not the state
Because the engine is deterministic, the command log is a complete
description of the state. Rejected: periodic book snapshots alone (lose
everything since the last one). Cost: recovery time grows with the log;
snapshot + log tail is the next step.

## D14 Idle strategy is a flag
The engine thread spins briefly, then yields, then sleeps 50 us. `--spin`
makes it burn a core instead. On an isolated core spinning gives the lowest
and most stable latency; on a shared 2-vCPU machine it steals time from the
gateway. Both are measured in benchmarks.md.

## D15 Absolute level updates on the feed
A Level message carries the new total quantity at a price, not a delta.
Applying it twice is harmless and a missed one is repaired by the next
update to that level. Cost: no per-order detail on the feed (it is L2, not L3).

## D16 The market maker removes its own quotes from its view of the market
The reference price is the mid of everyone else's orders. Rejected: the raw
mid. With inventory skew the raw mid feeds back: skewed quotes move the mid,
the mid moves the next quotes, and the price runs away.

## D17 Quotes rounded to the nearest tick
Rejected: floor for the bid and ceil for the ask. That widens the spread by
up to a tick whenever the reservation price is fractional, which made skewed
strategies look more profitable than the unskewed baseline for the wrong reason.

## D18 Profile first, and keep the experiments that failed
The bitmap (D2) came from a flame graph, not a guess. The same profile
suggested the market data thread's idle polling was hurting the engine;
changing it produced no measurable gain, so the change was reverted and the
result recorded. CPU pinning on two cores likewise. Both are in benchmarks.md.

## D19 gRPC is not on the order path
Measured on the same workload, gRPC unary is 3.6 times slower than the
fixed-width protocol on a raw socket and gRPC streaming 2.1 times. Protobuf
on a raw socket is within about 2 us. So the encoding is a minor cost and the
framework a major one. gRPC would be the right choice for a control-plane
API beside the exchange, where schema evolution and tooling matter more than
tens of microseconds.
