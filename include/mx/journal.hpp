#pragma once
// Append-only command journal (write-ahead log).
//
// The engine thread writes each batch of Commands here BEFORE applying them,
// so nothing is acknowledged to a client that is not already in the log.
// Because the engine is deterministic, replaying the journal into a fresh
// engine rebuilds exactly the same book, order ids included.
#include <cerrno>
#include <cstdio>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include "engine.hpp"

namespace mx {

class Journal {
 public:
  ~Journal() { if (fd_ >= 0) ::close(fd_); }

  // fsync=true makes every batch durable against power loss (slow);
  // false survives a process crash but relies on the page cache.
  bool open(const char* path, bool fsync) {
    fd_ = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    sync_ = fsync;
    return fd_ >= 0;
  }

  // One write() per batch: group commit.
  bool append(const Command* c, size_t n) {
    const char* p = reinterpret_cast<const char*>(c);
    size_t left = n * sizeof(Command);
    while (left > 0) {
      ssize_t w = ::write(fd_, p, left);
      if (w < 0) { if (errno == EINTR) continue; return false; }
      p += w;
      left -= static_cast<size_t>(w);
    }
    if (sync_) ::fdatasync(fd_);
    return true;
  }

  // Feed every complete record to fn. A torn record at the tail (crash in
  // the middle of a write) is ignored. Returns the number of records read.
  template <class F>
  static size_t replay(const char* path, F&& fn) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return 0;
    std::vector<Command> buf(4096);
    size_t total = 0, have = 0;
    char* raw = reinterpret_cast<char*>(buf.data());
    const size_t cap = buf.size() * sizeof(Command);
    for (;;) {
      ssize_t r = ::read(fd, raw + have, cap - have);
      if (r < 0) { if (errno == EINTR) continue; break; }
      if (r == 0) break;
      have += static_cast<size_t>(r);
      const size_t n = have / sizeof(Command);
      for (size_t i = 0; i < n; ++i) fn(buf[i]);
      total += n;
      const size_t used = n * sizeof(Command);
      std::memmove(raw, raw + used, have - used);
      have -= used;
    }
    ::close(fd);
    return total;
  }

 private:
  int fd_ = -1;
  bool sync_ = false;
};

}  // namespace mx
