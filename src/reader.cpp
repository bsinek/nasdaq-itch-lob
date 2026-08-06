#include "reader.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "nanotime.hpp"

namespace itch {

Reader::Reader(const std::string& path, size_t buf_bytes) : buf_(buf_bytes) {
  f_ = gzopen(path.c_str(), "rb");
  if (!f_) throw std::runtime_error("cannot open " + path);
  gzbuffer(f_, 1u << 20);
}

Reader::~Reader() {
  if (f_) gzclose(f_);
}

bool Reader::fill(size_t need) {
  if (pos_ > 0) {
    const size_t tail = end_ - pos_;
    if (tail) std::memmove(buf_.data(), buf_.data() + pos_, tail);
    end_ = tail;
    pos_ = 0;
  }
  while (end_ < need && !eof_) {
    const uint64_t t0 = now_ns();
    const int n = gzread(f_, buf_.data() + end_, unsigned(buf_.size() - end_));
    inflate_ns_ += now_ns() - t0;
    if (n < 0) throw std::runtime_error("gzread failed (corrupt gzip stream?)");
    if (n == 0) { eof_ = true; break; }
    end_ += size_t(n);
    bytes_out_ += uint64_t(n);
  }
  if (end_ < need) {
    if (end_ != 0)
      throw std::runtime_error("truncated trailing message: " + std::to_string(end_) +
                               " bytes left after " + std::to_string(n_msgs_) + " messages");
    return false;
  }
  return true;
}

void Reader::fail_framing(uint8_t type, uint16_t framed) const {
  std::fprintf(stderr,
               "framing error after %llu messages: type 0x%02x ('%c') framed len %u, spec len %u\n",
               (unsigned long long)n_msgs_, type, type >= 32 && type < 127 ? type : '?', framed,
               kMsgLen[type]);
  std::exit(2);
}

}  // namespace itch
