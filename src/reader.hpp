#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <zlib.h>

#include "messages.hpp"

namespace itch {

// Streams a gzipped historical ITCH file, yielding one message at a time
// (type byte first). The returned view is valid until the next call.
// Malformed framing is a hard error: silent skips would corrupt every
// downstream number.
class Reader {
 public:
  explicit Reader(const std::string& path, size_t buf_bytes = 8u << 20);
  ~Reader();

  // nullptr at clean EOF.
  const uint8_t* next(uint16_t& len) {
    if (end_ - pos_ < 2 && !fill(2)) return nullptr;
    const uint16_t need = be16(buf_.data() + pos_);
    if constexpr (kChecked) {
      // 12 = smallest spec message (S). A shorter frame (esp. 0) would leave
      // the type byte unbacked by data and could false-pass the length check.
      if (need < 12) fail_short(need);
    }
    if (end_ - pos_ < size_t(2) + need && !fill(size_t(2) + need)) return nullptr;
    const uint8_t* m = buf_.data() + pos_;
    if constexpr (kChecked) {
      if (kMsgLen[m[2]] != need) fail_framing(m[2], need);
    }
    pos_ += size_t(2) + need;
    len = need;
    ++n_msgs_;
    return m + 2;
  }

  uint64_t messages() const { return n_msgs_; }
  uint64_t inflate_ns() const { return inflate_ns_; }  // time spent inside gzread
  uint64_t bytes_out() const { return bytes_out_; }    // decompressed bytes consumed

 private:
  bool fill(size_t need);
  [[noreturn]] void fail_framing(uint8_t type, uint16_t framed) const;
  [[noreturn]] void fail_short(uint16_t framed) const;

  gzFile f_ = nullptr;
  std::vector<uint8_t> buf_;
  size_t pos_ = 0, end_ = 0;
  bool eof_ = false;
  uint64_t n_msgs_ = 0, inflate_ns_ = 0, bytes_out_ = 0;
};

}  // namespace itch
