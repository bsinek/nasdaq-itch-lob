// Scripted message sequences with known book outcomes, run against the real
// Handler. Builds raw ITCH bytes so the wire-decode path is tested too.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "handler.hpp"

using namespace itch;

static int checks = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++checks;                                                              \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                            \
    }                                                                      \
  } while (0)

struct Msg {
  std::vector<uint8_t> b;
  explicit Msg(char type) : b(kMsgLen[uint8_t(type)], 0) { b[0] = uint8_t(type); }
  Msg& u16(size_t off, uint16_t v) { for (int i = 0; i < 2; ++i) b[off + i] = uint8_t(v >> (8 * (1 - i))); return *this; }
  Msg& u32(size_t off, uint32_t v) { for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(v >> (8 * (3 - i))); return *this; }
  Msg& u48(size_t off, uint64_t v) { for (int i = 0; i < 6; ++i) b[off + i] = uint8_t(v >> (8 * (5 - i))); return *this; }
  Msg& u64(size_t off, uint64_t v) { for (int i = 0; i < 8; ++i) b[off + i] = uint8_t(v >> (8 * (7 - i))); return *this; }
  Msg& ch(size_t off, char c) { b[off] = uint8_t(c); return *this; }
  Msg& str(size_t off, const char* s) {
    std::memset(b.data() + off, ' ', 8);
    std::memcpy(b.data() + off, s, std::strlen(s));
    return *this;
  }
};

static const uint16_t LOC = 42;
static uint64_t g_ts = 34'200'000'000'000ull;  // 09:30

static Msg directory(const char* sym) { return Msg('R').u16(1, LOC).u48(5, g_ts).str(11, sym); }
static Msg sysev(char ev) { Msg m('S'); m.u48(5, g_ts).ch(11, ev); return m; }
static Msg action(char st) { return Msg('H').u16(1, LOC).u48(5, g_ts).ch(19, st); }
static Msg add(uint64_t ref, char side, uint32_t sh, uint32_t px) {
  return Msg('A').u16(1, LOC).u48(5, g_ts += 1000).u64(11, ref).ch(19, side).u32(20, sh).str(24, "AAPL").u32(32, px);
}
static Msg exec(uint64_t ref, uint32_t sh, uint64_t match) {
  return Msg('E').u16(1, LOC).u48(5, g_ts += 1000).u64(11, ref).u32(19, sh).u64(23, match);
}
static Msg cancel(uint64_t ref, uint32_t sh) {
  return Msg('X').u16(1, LOC).u48(5, g_ts += 1000).u64(11, ref).u32(19, sh);
}
static Msg del(uint64_t ref) { return Msg('D').u16(1, LOC).u48(5, g_ts += 1000).u64(11, ref); }
static Msg replace(uint64_t o, uint64_t n, uint32_t sh, uint32_t px) {
  return Msg('U').u16(1, LOC).u48(5, g_ts += 1000).u64(11, o).u64(19, n).u32(27, sh).u32(31, px);
}
static Msg trade_p(uint32_t sh, uint32_t px) {
  return Msg('P').u16(1, LOC).u48(5, g_ts += 1000).ch(19, 'B').u32(20, sh).str(24, "AAPL").u32(32, px).u64(36, 777);
}
static Msg cross(uint64_t sh, uint32_t px, char type) {
  return Msg('Q').u16(1, LOC).u48(5, g_ts += 1000).u64(11, sh).str(19, "AAPL").u32(27, px).u64(31, 888).ch(39, type);
}
static Msg exec_px(uint64_t ref, uint32_t sh, char printable, uint32_t px) {
  return Msg('C').u16(1, LOC).u48(5, g_ts += 1000).u64(11, ref).u32(19, sh).u64(23, 999).ch(31, printable).u32(32, px);
}

int main() {
  Handler h;
  auto feed = [&](const Msg& m) { h.on_message(m.b.data()); };

  feed(directory("AAPL"));
  feed(action('T'));
  feed(sysev('Q'));                    // market open
  feed(cross(1000, 1650000, 'O'));     // opening cross @165.0000

  const Book& b = h.book(0);

  // adds on both sides
  feed(add(1, 'B', 100, 1649900));     // bid 164.99 x100
  feed(add(2, 'B', 200, 1649800));
  feed(add(3, 'S', 300, 1650100));     // ask 165.01 x300
  feed(add(4, 'S', 50, 1650200));
  CHECK(b.bid.best().price == 1649900 && b.bid.best().shares == 100);
  CHECK(b.ask.best().price == 1650100 && b.ask.best().shares == 300);
  CHECK(b.spread() == 200);

  // aggregation at a level
  feed(add(5, 'B', 40, 1649900));
  CHECK(b.bid.best().shares == 140 && b.bid.best().count == 2);

  // partial execution
  feed(exec(1, 60, 1));
  CHECK(b.bid.best().shares == 80 && b.bid.best().count == 2);
  // full execution of remainder drops the order but level survives (order 5)
  feed(exec(1, 40, 2));
  CHECK(b.bid.best().shares == 40 && b.bid.best().count == 1);

  // partial cancel then delete
  feed(cancel(2, 50));
  CHECK(b.bid.at(1).shares == 150);
  feed(del(2));
  CHECK(b.bid.depth() == 1 && b.bid.best().price == 1649900);  // 164.98 level gone

  // replace: order 3 moves price, loses queue
  feed(replace(3, 6, 300, 1650300));
  CHECK(b.ask.best().price == 1650200 && b.ask.best().shares == 50);
  CHECK(b.ask.at(1).price == 1650300 && b.ask.at(1).shares == 300);

  // non-printable exec-with-price: volume must not count it
  const uint64_t vol_before = h.nasdaq_volume(0);
  feed(exec_px(6, 10, 'N', 1650250));
  CHECK(h.nasdaq_volume(0) == vol_before);
  CHECK(b.ask.at(1).shares == 290);
  // printable exec-with-price counts
  feed(exec_px(6, 10, 'Y', 1650250));
  CHECK(h.nasdaq_volume(0) == vol_before + 10);

  // hidden trade P: volume yes, book untouched
  const auto ask_shares = b.ask.best().shares;
  feed(trade_p(500, 1650000));
  CHECK(h.nasdaq_volume(0) == vol_before + 510);
  CHECK(b.ask.best().shares == ask_shares);

  // X cancel down to exactly zero must erase the order (order-erase-via-X path)
  feed(add(20, 'B', 100, 1649500));
  feed(cancel(20, 100));
  CHECK(b.bid.find(1649500) == nullptr);
  feed(exec(20, 10, 3));  // dead ref: must count as an exec mismatch
  CHECK(h.exec_bad() == 1);

  // U of a partially executed order must remove only the REMAINING shares
  feed(add(21, 'S', 200, 1650400));
  feed(exec(21, 50, 4));
  CHECK(b.ask.find(1650400)->shares == 150);
  feed(replace(21, 22, 150, 1650500));
  CHECK(b.ask.find(1650400) == nullptr);        // old level fully cleared
  CHECK(b.ask.find(1650500)->shares == 150);    // new leg resting
  feed(del(22));

  // negative-path counters: unknown refs on a tracked locate must count
  const auto mod_bad_before = h.mod_bad();
  feed(cancel(999, 10));
  feed(del(998));
  feed(replace(997, 996, 100, 1650000));
  CHECK(h.mod_bad() == mod_bad_before + 3);

  // book never went crossed; level accounting never went inconsistent
  for (int s = 0; s < kNSyms; ++s) CHECK(h.crossed_instants(s) == 0);
  CHECK(h.book_missing() == 0);

  // closing cross recorded
  feed(cross(2000, 1652500, 'C'));
  CHECK(h.close_cross_px(0) == 1652500);

  std::printf("book_test: %d checks passed\n", checks);
  return 0;
}
