#pragma once
// Bounded lock-free single-producer / single-consumer ring buffer.
//
// Exactly one thread may call push() and exactly one may call pop().
// head_ and tail_ sit on separate cache lines to avoid false sharing, and
// each side keeps a cached copy of the other's index so the common case
// touches no shared cache line at all.
#include <atomic>
#include <cstddef>
#include <vector>

namespace mx {

template <class T>
class SpscQueue {
 public:
  // capacity must be a power of two
  explicit SpscQueue(size_t capacity) : mask_(capacity - 1), buf_(capacity) {}

  bool push(const T& v) {
    const size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_cache_ > mask_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h - tail_cache_ > mask_) return false;  // full
    }
    buf_[h & mask_] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  bool pop(T& out) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_cache_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t == head_cache_) return false;  // empty
    }
    out = buf_[t & mask_];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

 private:
  const size_t mask_;
  std::vector<T> buf_;
  alignas(64) std::atomic<size_t> head_{0};  // written by producer
  size_t tail_cache_ = 0;                    // producer's view of tail
  alignas(64) std::atomic<size_t> tail_{0};  // written by consumer
  size_t head_cache_ = 0;                    // consumer's view of head
};

}  // namespace mx
