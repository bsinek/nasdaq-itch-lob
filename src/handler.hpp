#pragma once
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

#include "book.hpp"
#include "messages.hpp"

namespace itch {

constexpr int kNSyms = 8;
inline const char* const kSymbols[kNSyms] = {"AAPL", "MSFT", "AMZN", "NVDA",
                                             "AMD",  "INTC", "FB",   "TSLA"};

struct Order {
  uint16_t locate;
  uint8_t side;  // 'B' or 'S'
  uint8_t _pad;
  uint32_t price;
  uint32_t shares;
};

#pragma pack(push, 1)
// One record per event that changed the top-5 of a tracked book; a U replace
// emits a single post-replace record (the intermediate removed-only state
// never existed at the exchange). 96 bytes.
struct SnapRec {
  uint64_t ts;       // ns since midnight
  uint16_t sym;      // index into kSymbols
  uint8_t reason;    // 1 add, 2 exec, 3 cancel, 4 delete, 5 replace
  uint8_t _pad[5];
  uint32_t bid_px[5], bid_sz[5], ask_px[5], ask_sz[5];  // zeros beyond depth
};
// One record per tracked resting add within 5 ticks of the same-side touch.
// The outcome fields are back-filled as the stream reveals the order's fate:
// this labeling is only possible with order-level (L3) data. 64 bytes.
struct OrderRec {
  uint64_t ts_add;
  uint64_t ref;
  uint16_t sym;
  uint8_t side;            // 'B'/'S'
  uint8_t dist_ticks;      // 0..5 ticks from same-side best at insertion
  uint32_t px;
  uint32_t sz;
  uint32_t queue_ahead;    // shares already at this price level
  uint32_t level_ct;       // orders already at this price level
  uint32_t same_depth5;    // shares in top-5 same side (pre-insert)
  uint32_t opp_depth5;     // shares in top-5 opposite side
  uint32_t spread;         // ask-bid at insertion, Price(4) units
  uint8_t outcome;         // 0 alive; 'F' filled>=1sh, 'X' cancelled, 'R' replaced, 'U' alive at EOD
  uint8_t _pad[3];
  uint64_t ts_outcome;     // first fill, or death, or EOD
  uint32_t exec_shares;    // total shares executed over the order's life
};
#pragma pack(pop)
static_assert(sizeof(SnapRec) == 96);
static_assert(sizeof(OrderRec) == 64);

// Full-feed handler: parses every message, maintains books + validation
// counters for the tracked symbols, optionally exports snapshots.bin /
// orders.bin. Single-threaded; this is the benchmarked hot path.
class Handler {
 public:
  Handler() {
    sym_of_locate_.assign(65536, -1);
    state_.assign(65536, 0);
    vol_raw_.assign(65536, 0);
    orders_.reserve(1u << 21);
    row_of_ref_.reserve(1u << 20);
    for (int s = 0; s < kNSyms; ++s) {
      std::memset(sym_pad_[s], ' ', 8);
      std::memcpy(sym_pad_[s], kSymbols[s], std::strlen(kSymbols[s]));
    }
  }

  ~Handler() {
    if (snap_f_) std::fclose(snap_f_);
  }

  void enable_export(const std::string& dir) {
    exporting_ = true;
    export_dir_ = dir;
    ::mkdir(dir.c_str(), 0755);  // best-effort; fopen below reports real failures
    snap_f_ = std::fopen((dir + "/snapshots.bin").c_str(), "wb");
    if (!snap_f_) { std::perror("snapshots.bin"); std::exit(2); }
    std::setvbuf(snap_f_, nullptr, _IOFBF, 4u << 20);
    order_rows_.reserve(1u << 22);
  }

  void on_message(const uint8_t* m) {
    ++count_[m[0]];
    switch (m[0]) {
      case 'A': handle_add(m, false); break;
      case 'F': handle_add(m, true); break;
      case 'E': handle_exec(m); break;
      case 'C': handle_exec_px(m); break;
      case 'X': handle_cancel(m); break;
      case 'D': handle_delete(m); break;
      case 'U': handle_replace(m); break;
      case 'P': {  // hidden-liquidity match: counts toward volume, never touches the book
        const uint16_t loc = f_locate(m);
        const uint32_t sh = be32(m + 20);
        vol_raw_[loc] += sh;
        const int sym = sym_of_locate_[loc];
        if (sym >= 0) last_trade_[sym] = {be32(m + 32), sh, f_ts(m)};
        break;
      }
      case 'Q': handle_cross(m); break;
      case 'S': {
        const char ev = char(m[11]);
        if (ev == 'Q') market_open_ = true;
        else if (ev == 'M') market_open_ = false;
        sys_event_ts_[uint8_t(ev)] = f_ts(m);
        break;
      }
      case 'H': {
        state_[f_locate(m)] = m[19];
        break;
      }
      case 'R': handle_directory(m); break;
      default: break;  // Y L V W K J h B I N O: counted only
    }
    last_ts_ = f_ts(m);
  }

  // Called once after the stream ends: labels still-alive lifecycle rows and
  // flushes orders.bin.
  void finish() {
    if (!exporting_) return;
    for (auto& [ref, idx] : row_of_ref_) {
      OrderRec& r = order_rows_[idx];
      if (r.outcome == 0) { r.outcome = 'U'; r.ts_outcome = last_ts_; }
    }
    for (auto& r : order_rows_)
      if (r.outcome == 0) { r.outcome = 'U'; r.ts_outcome = last_ts_; }
    FILE* f = std::fopen((export_dir_ + "/orders.bin").c_str(), "wb");
    if (!f) { std::perror("orders.bin"); std::exit(2); }
    std::fwrite(order_rows_.data(), sizeof(OrderRec), order_rows_.size(), f);
    std::fclose(f);
    std::fclose(snap_f_);
    snap_f_ = nullptr;
  }

  // --- results (read by itch_parse.cpp reporting) ---
  const std::array<uint64_t, 256>& counts() const { return count_; }
  uint64_t exec_ok() const { return v_exec_ok_; }
  uint64_t exec_bad() const { return v_exec_bad_; }
  uint64_t mod_ok() const { return v_mod_ok_; }
  uint64_t mod_bad() const { return v_mod_bad_; }
  uint64_t book_missing() const { return v_book_missing_; }
  uint64_t crossed_instants(int s) const { return crossed_[s]; }
  uint64_t nasdaq_volume(int s) const { return sym_locate_[s] >= 0 ? vol_raw_[sym_locate_[s]] : 0; }
  uint64_t book_exec_shares(int s) const { return vol_book_[s]; }
  uint32_t open_cross_px(int s) const { return open_px_[s]; }
  uint32_t close_cross_px(int s) const { return close_px_[s]; }
  uint64_t snapshots_written() const { return n_snaps_; }
  uint64_t order_rows() const { return order_rows_.size(); }
  const Book& book(int s) const { return books_[s]; }
  struct LastTrade { uint32_t px, sh; uint64_t ts; };
  const LastTrade& last_trade(int s) const { return last_trade_[s]; }
  int locate_of_sym(int s) const { return sym_locate_[s]; }
  uint64_t last_ts() const { return last_ts_; }
  uint64_t sys_event_ts(char ev) const { return sys_event_ts_[uint8_t(ev)]; }

 private:
  bool tracked(uint16_t loc) const { return sym_of_locate_[loc] >= 0; }

  void handle_directory(const uint8_t* m) {
    const uint16_t loc = f_locate(m);
    for (int s = 0; s < kNSyms; ++s) {
      if (std::memcmp(m + 11, sym_pad_[s], 8) == 0) {
        sym_of_locate_[loc] = int16_t(s);
        sym_locate_[s] = loc;
        break;
      }
    }
  }

  void handle_add(const uint8_t* m, bool /*mpid*/) {
    const uint16_t loc = f_locate(m);
    const int sym = sym_of_locate_[loc];
    if (sym < 0) return;
    const uint64_t ref = be64(m + 11);
    const char side = char(m[19]);
    const uint32_t sh = be32(m + 20);
    const uint32_t px = be32(m + 32);
    add_order(sym, loc, ref, side, sh, px, f_ts(m), 1);
  }

  // Shared by A/F and the new leg of U. reason: 1 add, 5 replace.
  // extra_top5 lets U report its remove leg's top-5 change through the single
  // post-replace snapshot (a U is atomic at the exchange; emitting the
  // intermediate removed-but-not-readded state would record a book state that
  // never existed).
  void add_order(int sym, uint16_t loc, uint64_t ref, char side, uint32_t sh,
                 uint32_t px, uint64_t ts, uint8_t reason, bool extra_top5 = false) {
    Book& b = books_[sym];
    if (exporting_) maybe_record_order(sym, b, loc, ref, side, px, sh, ts);
    orders_.emplace(ref, Order{loc, uint8_t(side), 0, px, sh});
    const bool t5 = b.side(side).add(px, sh) || extra_top5;
    after_mutation(sym, reason, ts, t5);
  }

  void handle_exec(const uint8_t* m) {
    const uint16_t loc = f_locate(m);
    const uint32_t sh = be32(m + 19);
    vol_raw_[loc] += sh;
    const auto it = orders_.find(be64(m + 11));
    if (it == orders_.end()) {
      if (tracked(loc)) ++v_exec_bad_;
      return;
    }
    apply_exec(it, sh, f_ts(m), true, it->second.price);
  }

  void handle_exec_px(const uint8_t* m) {
    const uint16_t loc = f_locate(m);
    const uint32_t sh = be32(m + 19);
    const bool printable = m[31] == 'Y';
    if (printable) vol_raw_[loc] += sh;
    const auto it = orders_.find(be64(m + 11));
    if (it == orders_.end()) {
      if (tracked(loc)) ++v_exec_bad_;
      return;
    }
    apply_exec(it, sh, f_ts(m), printable, be32(m + 32));
  }

  void apply_exec(std::unordered_map<uint64_t, Order>::iterator it, uint32_t sh,
                  uint64_t ts, bool printable, uint32_t trade_px) {
    Order& o = it->second;
    const int sym = sym_of_locate_[o.locate];
    last_trade_[sym] = {trade_px, sh, ts};
    if (o.shares < sh) ++v_exec_bad_; else ++v_exec_ok_;
    const bool full = o.shares <= sh;
    const auto rr = books_[sym].side(o.side).remove(o.price, sh, full);
    if (!rr.found || rr.deficit) ++v_book_missing_;
    if (printable) vol_book_[sym] += sh;
    if (exporting_) note_exec(it->first, sh, ts);
    if (full) {
      if (exporting_) note_death(it->first, ts, 'X');  // fully-executed: row already 'F'
      orders_.erase(it);
    } else {
      o.shares -= sh;
    }
    after_mutation(sym, 2, ts, rr.top5);
  }

  void handle_cancel(const uint8_t* m) {
    const uint16_t loc = f_locate(m);
    const uint32_t sh = be32(m + 19);
    const auto it = orders_.find(be64(m + 11));
    if (it == orders_.end()) {
      if (tracked(loc)) ++v_mod_bad_;
      return;
    }
    Order& o = it->second;
    const int sym = sym_of_locate_[o.locate];
    if (o.shares < sh) ++v_mod_bad_; else ++v_mod_ok_;
    const bool full = o.shares <= sh;
    const auto rr = books_[sym].side(o.side).remove(o.price, sh, full);
    if (!rr.found || rr.deficit) ++v_book_missing_;
    if (full) {
      if (exporting_) note_death(it->first, f_ts(m), 'X');
      orders_.erase(it);
    } else {
      o.shares -= sh;
    }
    after_mutation(sym, 3, f_ts(m), rr.top5);
  }

  void handle_delete(const uint8_t* m) {
    const uint16_t loc = f_locate(m);
    const auto it = orders_.find(be64(m + 11));
    if (it == orders_.end()) {
      if (tracked(loc)) ++v_mod_bad_;
      return;
    }
    ++v_mod_ok_;
    Order& o = it->second;
    const int sym = sym_of_locate_[o.locate];
    const auto rr = books_[sym].side(o.side).remove(o.price, o.shares, true);
    if (!rr.found || rr.deficit) ++v_book_missing_;
    if (exporting_) note_death(it->first, f_ts(m), 'X');
    orders_.erase(it);
    after_mutation(sym, 4, f_ts(m), rr.top5);
  }

  void handle_replace(const uint8_t* m) {
    const uint16_t loc = f_locate(m);
    const auto it = orders_.find(be64(m + 11));
    if (it == orders_.end()) {
      if (tracked(loc)) ++v_mod_bad_;
      return;
    }
    ++v_mod_ok_;
    const Order o = it->second;  // copy: side/locate survive the erase
    const int sym = sym_of_locate_[o.locate];
    const auto rr = books_[sym].side(o.side).remove(o.price, o.shares, true);
    if (!rr.found || rr.deficit) ++v_book_missing_;
    if (exporting_) note_death(it->first, f_ts(m), 'R');
    orders_.erase(it);
    const uint64_t new_ref = be64(m + 19);
    const uint32_t sh = be32(m + 27);
    const uint32_t px = be32(m + 31);
    add_order(sym, o.locate, new_ref, char(o.side), sh, px, f_ts(m), 5, rr.top5);
  }

  void handle_cross(const uint8_t* m) {
    const uint16_t loc = f_locate(m);
    const uint64_t sh = be64(m + 11);
    vol_raw_[loc] += sh;
    const int sym = sym_of_locate_[loc];
    if (sym < 0) return;
    const char ct = char(m[39]);
    const uint32_t px = be32(m + 27);
    if (ct == 'O') { open_px_[sym] = px; opened_[sym] = true; }
    else if (ct == 'C') { close_px_[sym] = px; closed_[sym] = true; }
  }

  void maybe_record_order(int sym, Book& b, uint16_t loc, uint64_t ref, char side,
                          uint32_t px, uint32_t sh, uint64_t ts) {
    if (!market_open_ || !opened_[sym] || closed_[sym]) return;
    if (state_[loc] != 'T') return;
    if (!b.has_bbo() || b.crossed()) return;
    const BookSide& same = b.side(side);
    const uint32_t best = same.best().price;
    int64_t d = side == 'B' ? int64_t(best) - int64_t(px) : int64_t(px) - int64_t(best);
    if (d < 0) d = 0;              // price-improving order: it becomes the touch
    else if (d > 500) return;      // > 5 ticks ($0.05) from touch: out of scope
    const Level* L = d == 0 && px != best ? nullptr : same.find(px);
    OrderRec r{};
    r.ts_add = ts;
    r.ref = ref;
    r.sym = uint16_t(sym);
    r.side = uint8_t(side);
    r.dist_ticks = uint8_t(d / 100);
    r.px = px;
    r.sz = sh;
    r.queue_ahead = L ? L->shares : 0;
    r.level_ct = L ? L->count : 0;
    r.same_depth5 = uint32_t(same.shares_top(5));
    r.opp_depth5 = uint32_t(b.side(side == 'B' ? 'S' : 'B').shares_top(5));
    r.spread = b.spread();
    row_of_ref_[ref] = uint32_t(order_rows_.size());
    order_rows_.push_back(r);
  }

  void note_exec(uint64_t ref, uint32_t sh, uint64_t ts) {
    const auto it = row_of_ref_.find(ref);
    if (it == row_of_ref_.end()) return;
    OrderRec& r = order_rows_[it->second];
    r.exec_shares += sh;
    if (r.outcome == 0) { r.outcome = 'F'; r.ts_outcome = ts; }
  }

  // cause: 'X' cancel/delete/fully-executed, 'R' replaced. A row that already
  // filled keeps outcome 'F'.
  void note_death(uint64_t ref, uint64_t ts, char cause) {
    const auto it = row_of_ref_.find(ref);
    if (it == row_of_ref_.end()) return;
    OrderRec& r = order_rows_[it->second];
    if (r.outcome == 0) { r.outcome = uint8_t(cause); r.ts_outcome = ts; }
    row_of_ref_.erase(it);
  }

  void after_mutation(int sym, uint8_t reason, uint64_t ts, bool top5_changed) {
    Book& b = books_[sym];
    if (b.crossed() && market_open_ && opened_[sym] && !closed_[sym] &&
        state_[sym_locate_[sym]] == 'T')
      ++crossed_[sym];
    if (exporting_ && top5_changed) {
      SnapRec r{};
      r.ts = ts;
      r.sym = uint16_t(sym);
      r.reason = reason;
      const size_t nb = b.bid.depth() < 5 ? b.bid.depth() : 5;
      for (size_t i = 0; i < nb; ++i) { r.bid_px[i] = b.bid.at(i).price; r.bid_sz[i] = b.bid.at(i).shares; }
      const size_t na = b.ask.depth() < 5 ? b.ask.depth() : 5;
      for (size_t i = 0; i < na; ++i) { r.ask_px[i] = b.ask.at(i).price; r.ask_sz[i] = b.ask.at(i).shares; }
      std::fwrite(&r, sizeof r, 1, snap_f_);
      ++n_snaps_;
    }
  }

  // --- state ---
  std::array<Book, kNSyms> books_{};
  std::unordered_map<uint64_t, Order> orders_;
  std::vector<int16_t> sym_of_locate_;
  std::array<int, kNSyms> sym_locate_{-1, -1, -1, -1, -1, -1, -1, -1};
  char sym_pad_[kNSyms][8];
  std::vector<uint8_t> state_;   // last H trading state per locate
  bool market_open_ = false;     // between S:Q and S:M
  std::array<bool, kNSyms> opened_{}, closed_{};
  std::array<uint32_t, kNSyms> open_px_{}, close_px_{};
  uint64_t last_ts_ = 0;
  std::array<uint64_t, 256> sys_event_ts_{};
  std::array<LastTrade, kNSyms> last_trade_{};

  // validation
  std::array<uint64_t, 256> count_{};
  uint64_t v_exec_ok_ = 0, v_exec_bad_ = 0, v_mod_ok_ = 0, v_mod_bad_ = 0;
  uint64_t v_book_missing_ = 0;
  std::array<uint64_t, kNSyms> crossed_{};
  std::vector<uint64_t> vol_raw_;              // per locate, printable exec + P + Q shares
  std::array<uint64_t, kNSyms> vol_book_{};    // printable shares removed from tracked books

  // export
  bool exporting_ = false;
  std::string export_dir_;
  FILE* snap_f_ = nullptr;
  uint64_t n_snaps_ = 0;
  std::vector<OrderRec> order_rows_;
  std::unordered_map<uint64_t, uint32_t> row_of_ref_;
};

}  // namespace itch
