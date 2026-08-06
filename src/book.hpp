#pragma once
#include <cstdint>
#include <vector>

namespace itch {

struct Level {
  uint32_t price;   // Price(4): 1 unit = $0.0001; $0.01 tick = 100
  uint32_t shares;
  uint32_t count;   // resting orders at this level
};

// One side of a price-level book, sorted best-first (bids descending, asks
// ascending). Plain vector + linear scan from the front: activity clusters at
// the touch, so the common case touches the first handful of elements and
// stays in one cache line stride.
class BookSide {
 public:
  explicit BookSide(bool is_bid) : is_bid_(is_bid) { lv_.reserve(256); }

  // Mutators return true when the top-5 levels changed.
  bool add(uint32_t px, uint32_t sh) {
    size_t i = 0;
    const size_t n = lv_.size();
    while (i < n && better(lv_[i].price, px)) ++i;
    if (i < n && lv_[i].price == px) {
      lv_[i].shares += sh;
      lv_[i].count += 1;
    } else {
      lv_.insert(lv_.begin() + i, Level{px, sh, 1});
    }
    return i < 5;
  }

  // Removes shares from the level at px; drop_order also decrements the order
  // count (full cancel/delete/final execution). Returns {found, top5_changed}.
  struct RemoveResult { bool found; bool top5; };
  RemoveResult remove(uint32_t px, uint32_t sh, bool drop_order) {
    const size_t n = lv_.size();
    for (size_t i = 0; i < n; ++i) {
      if (lv_[i].price == px) {
        Level& L = lv_[i];
        L.shares = L.shares >= sh ? L.shares - sh : 0;
        if (drop_order && L.count) --L.count;
        if (L.shares == 0) lv_.erase(lv_.begin() + i);
        return {true, i < 5};
      }
      if (better(px, lv_[i].price)) break;  // px would sort above: not present
    }
    return {false, false};
  }

  bool empty() const { return lv_.empty(); }
  const Level& best() const { return lv_[0]; }
  size_t depth() const { return lv_.size(); }
  const Level& at(size_t i) const { return lv_[i]; }

  const Level* find(uint32_t px) const {
    for (const auto& L : lv_) {
      if (L.price == px) return &L;
      if (better(px, L.price)) break;
    }
    return nullptr;
  }

  uint64_t shares_top(size_t k) const {
    uint64_t s = 0;
    const size_t n = lv_.size() < k ? lv_.size() : k;
    for (size_t i = 0; i < n; ++i) s += lv_[i].shares;
    return s;
  }

 private:
  bool better(uint32_t a, uint32_t b) const { return is_bid_ ? a > b : a < b; }
  std::vector<Level> lv_;
  bool is_bid_;
};

struct Book {
  BookSide bid{true}, ask{false};
  BookSide& side(char s) { return s == 'B' ? bid : ask; }
  const BookSide& side(char s) const { return s == 'B' ? bid : ask; }
  bool has_bbo() const { return !bid.empty() && !ask.empty(); }
  bool crossed() const { return has_bbo() && bid.best().price >= ask.best().price; }
  uint32_t spread() const { return ask.best().price - bid.best().price; }
};

}  // namespace itch
