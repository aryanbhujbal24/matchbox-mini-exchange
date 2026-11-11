#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace mx {

inline uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}

// Pin the calling thread to one core (no-op when cpu < 0).
inline void pin_cpu(int cpu) {
  if (cpu < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

// Name the calling thread (max 15 chars) so perf, top -H and gdb can tell threads apart.
inline void name_thread(const char* name) { pthread_setname_np(pthread_self(), name); }

// Idle strategy for a thread polling a lock-free queue.
// spin=true: burn a core for the lowest latency. Otherwise spin briefly,
// then yield, then sleep, trading wake-up latency for CPU.
struct Backoff {
  bool spin = false;
  uint32_t spin_iters = 2000;   // pause-loop iterations before giving up the core
  uint32_t yield_iters = 200;   // sched_yield() calls before sleeping
  uint32_t idle = 0;
  void reset() { idle = 0; }
  void wait() {
    if (spin || idle < spin_iters)            { ++idle; cpu_relax(); }
    else if (idle < spin_iters + yield_iters) { ++idle; sched_yield(); }
    else                                      { usleep(50); }
  }
};

// --- tiny argv helpers -------------------------------------------------
inline const char* arg_str(int argc, char** argv, const char* name, const char* def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], name)) return argv[i + 1];
  return def;
}
inline long arg_int(int argc, char** argv, const char* name, long def) {
  const char* v = arg_str(argc, argv, name, nullptr);
  return v ? std::atol(v) : def;
}
inline double arg_dbl(int argc, char** argv, const char* name, double def) {
  const char* v = arg_str(argc, argv, name, nullptr);
  return v ? std::atof(v) : def;
}
inline bool arg_flag(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], name)) return true;
  return false;
}

// Latency samples -> percentiles. Sorting samples is fine for offline
// reporting; swap in HdrHistogram when recording on the hot path matters.
struct LatStats {
  std::vector<uint64_t> ns;
  void add(uint64_t v) { ns.push_back(v); }
  uint64_t pct(double p) {
    if (ns.empty()) return 0;
    return ns[static_cast<size_t>(p * static_cast<double>(ns.size() - 1))];
  }
  void print(const char* label) {
    std::sort(ns.begin(), ns.end());
    std::printf("%s (us): p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f max=%.1f  n=%zu\n", label,
                pct(0.50) / 1e3, pct(0.90) / 1e3, pct(0.99) / 1e3, pct(0.999) / 1e3,
                ns.empty() ? 0.0 : ns.back() / 1e3, ns.size());
  }
};

}  // namespace mx
