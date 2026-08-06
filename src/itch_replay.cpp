// itch-replay: timestamp-paced replay with a live terminal book view —
// price ladder, scrolling trade tape with aggressor direction, rolling
// midprice sparkline, and a live 1s order-flow-imbalance gauge (the same OFI
// quantity ml/ofi.py feeds the models).
// Demo only — pacing is sleep-based and coarse; benchmarks live in itch-parse.
//
//   itch-replay FILE.gz SYMBOL [--speed N] [--start HH:MM:SS] [--duration SEC]
//               [--frames-dir DIR]
//
// Fast-forwards (unpaced, no render) to --start, then replays honoring
// inter-message timestamp deltas divided by --speed. --frames-dir writes
// plain-text frames for scripts/render_gif.py instead of drawing to the tty.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unistd.h>

#include "handler.hpp"
#include "nanotime.hpp"
#include "reader.hpp"

using namespace itch;

static std::string fmt_ts(uint64_t ns) {
  const uint64_t s = ns / 1'000'000'000ull;
  char b[32];
  std::snprintf(b, sizeof b, "%02llu:%02llu:%02llu.%03llu", (unsigned long long)(s / 3600),
                (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60),
                (unsigned long long)(ns / 1'000'000 % 1000));
  return b;
}

static std::string bar(uint32_t sh, uint32_t max_sh, int width) {
  const int n = max_sh ? int(uint64_t(sh) * width / max_sh) : 0;
  return std::string(size_t(n ? n : 1), '#');
}

struct Tape {
  uint64_t ts;
  uint32_t px, sh;
  char dir;  // '^' buyer lifted the ask, 'v' seller hit the bid, '.' inside
};

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s FILE.gz SYMBOL [--speed N] [--start HH:MM:SS] "
                 "[--duration SEC] [--frames-dir DIR]\n",
                 argv[0]);
    return 1;
  }
  const std::string path = argv[1], want = argv[2];
  double speed = 1.0;
  uint64_t start_ns = 34'200'000'000'000ull;  // 09:30
  double duration_s = 30.0;
  std::string frames_dir;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--speed" && i + 1 < argc) speed = std::atof(argv[++i]);
    else if (a == "--start" && i + 1 < argc) {
      unsigned hh = 0, mm = 0, ss = 0;
      std::sscanf(argv[++i], "%u:%u:%u", &hh, &mm, &ss);
      start_ns = (uint64_t(hh) * 3600 + mm * 60 + ss) * 1'000'000'000ull;
    } else if (a == "--duration" && i + 1 < argc) duration_s = std::atof(argv[++i]);
    else if (a == "--frames-dir" && i + 1 < argc) frames_dir = argv[++i];
  }

  int sym = -1;
  for (int s = 0; s < kNSyms; ++s)
    if (want == kSymbols[s]) sym = s;
  if (sym < 0) {
    std::fprintf(stderr, "symbol %s not tracked (", want.c_str());
    for (int s = 0; s < kNSyms; ++s) std::fprintf(stderr, "%s ", kSymbols[s]);
    std::fprintf(stderr, ")\n");
    return 1;
  }

  Reader r(path);
  Handler h;
  const uint64_t end_ns = start_ns + uint64_t(duration_s * 1e9);
  uint64_t wall0 = 0, ts0 = 0, next_frame_wall = 0, n_frames = 0;
  const bool tty_render = frames_dir.empty();
  if (tty_render) std::fputs("\x1b[2J\x1b[?25l", stdout);

  // live-view state (all display-only; the handler is untouched)
  uint32_t pb = 0, qb = 0, pa = 0, qa = 0;          // previous L1
  std::deque<std::pair<uint64_t, double>> ofi_ev;   // (ts, e_n), pruned to 1s
  std::deque<std::pair<uint64_t, double>> mids;     // (ts, mid), pruned to 60s
  std::deque<Tape> tape;                            // newest first, 6 kept
  uint64_t prev_trade_ts = 0;
  double ofi_scale = 1.0;

  uint16_t len;
  while (const uint8_t* m = r.next(len)) {
    h.on_message(m);
    const uint64_t ts = f_ts(m);
    if (ts < start_ns) continue;
    if (ts > end_ns) break;
    if (!wall0) { wall0 = now_ns(); ts0 = ts; next_frame_wall = wall0; }

    const Book& b = h.book(sym);

    // trade tape: last_trade changed => a new execution on this symbol.
    // Aggressor side inferred against the pre-trade quote (Lee-Ready style).
    const auto& lt = h.last_trade(sym);
    if (lt.ts != prev_trade_ts) {
      prev_trade_ts = lt.ts;
      const char dir = (pa && lt.px >= pa) ? '^' : (pb && lt.px <= pb) ? 'v' : '.';
      tape.push_front({lt.ts, lt.px, lt.sh, dir});
      if (tape.size() > 6) tape.pop_back();
    }

    // OFI event on any top-of-book change (Cont-Kukanov-Stoikov e_n)
    if (b.has_bbo()) {
      const uint32_t nb_ = b.bid.best().price, nqb = b.bid.best().shares;
      const uint32_t na_ = b.ask.best().price, nqa = b.ask.best().shares;
      if (nb_ != pb || nqb != qb || na_ != pa || nqa != qa) {
        if (pb && pa) {
          const double e = (nb_ >= pb ? double(nqb) : 0.0) - (nb_ <= pb ? double(qb) : 0.0)
                         - (na_ <= pa ? double(nqa) : 0.0) + (na_ >= pa ? double(qa) : 0.0);
          ofi_ev.emplace_back(ts, e);
        }
        pb = nb_; qb = nqb; pa = na_; qa = nqa;
      }
      while (!ofi_ev.empty() && ofi_ev.front().first + 1'000'000'000ull < ts)
        ofi_ev.pop_front();
    }

    // pace: sleep until this message's wall-clock slot (coarse; demo only)
    const uint64_t target = wall0 + uint64_t(double(ts - ts0) / speed);
    const uint64_t now = now_ns();
    if (target > now + 200'000) usleep(useconds_t((target - now) / 1000));

    if (now_ns() < next_frame_wall) continue;
    next_frame_wall += 80'000'000;  // 12.5 fps

    // ANSI colors in the live view only; frame files stay plain for render_gif.py
    const char* c_ask = tty_render ? "\x1b[31m" : "";
    const char* c_bid = tty_render ? "\x1b[32m" : "";
    const char* c_spr = tty_render ? "\x1b[33m" : "";
    const char* c_ofi = tty_render ? "\x1b[35m" : "";
    const char* c_mid = tty_render ? "\x1b[34m" : "";
    const char* c_up = tty_render ? "\x1b[32m" : "";
    const char* c_dn = tty_render ? "\x1b[31m" : "";
    const char* c_off = tty_render ? "\x1b[0m" : "";

    std::string f;
    f.reserve(6144);
    char line[192];
    std::snprintf(line, sizeof line, "%-6s %s  x%.0f   msgs %llu\n", want.c_str(),
                  fmt_ts(ts).c_str(), speed, (unsigned long long)r.messages());
    f += line;

    // midprice sparkline over the last 60s (sampled per frame)
    if (b.has_bbo()) {
      const double mid = (b.bid.best().price + b.ask.best().price) / 2e4;
      mids.emplace_back(ts, mid);
      while (!mids.empty() && mids.front().first + 60'000'000'000ull < ts) mids.pop_front();
      static const char* blocks[9] = {" ", "▁", "▂", "▃", "▄",
                                      "▅", "▆", "▇", "█"};
      double lo = 1e18, hi = -1e18;
      for (auto& [t_, v] : mids) { lo = std::min(lo, v); hi = std::max(hi, v); }
      std::string spark;
      const int W = 44;
      const size_t n = mids.size();
      for (int i = 0; i < W; ++i) {
        const size_t j = n <= size_t(W) ? size_t(i) : n - W + size_t(i);
        if (j >= n) { spark += ' '; continue; }
        const double v = mids[j].second;
        const int k = hi > lo ? int((v - lo) / (hi - lo) * 8.0) : 4;
        spark += blocks[std::clamp(k, 0, 8)];
      }
      std::snprintf(line, sizeof line, "%s~ mid %9.4f  %s  60s %.2f-%.2f%s\n", c_mid, mid,
                    spark.c_str(), lo, hi, c_off);
      f += line;
    }

    // 1s OFI gauge: signed sum of e_n, bar scaled by the largest |OFI| seen
    {
      double s = 0;
      for (auto& [t_, e] : ofi_ev) s += e;
      ofi_scale = std::max(ofi_scale, std::fabs(s));
      const int half = 14;
      const int k = int(std::fabs(s) / ofi_scale * half + 0.5);
      std::string g(size_t(half), ' ');
      std::string neg = g, pos = g;
      if (s < 0) neg.replace(size_t(half - k), size_t(k), size_t(k), '#');
      else pos.replace(0, size_t(k), size_t(k), '#');
      std::snprintf(line, sizeof line, "O OFI-1s [%s|%s] %+9.0f  (buy pressure ->)%s\n",
                    neg.c_str(), pos.c_str(), s, c_off);
      f.insert(f.size(), c_ofi);
      f += line;
    }
    f += "----------------------------------------------------\n";

    uint32_t max_sh = 1;
    const size_t na = b.ask.depth() < 10 ? b.ask.depth() : 10;
    const size_t nb2 = b.bid.depth() < 10 ? b.bid.depth() : 10;
    for (size_t i = 0; i < na; ++i) max_sh = std::max(max_sh, b.ask.at(i).shares);
    for (size_t i = 0; i < nb2; ++i) max_sh = std::max(max_sh, b.bid.at(i).shares);
    for (size_t i = na; i-- > 0;) {
      const Level& L = b.ask.at(i);
      std::snprintf(line, sizeof line, "%sA %9.2f %7u %-3u %s%s\n", c_ask, L.price / 1e4,
                    L.shares, L.count, bar(L.shares, max_sh, 24).c_str(), c_off);
      f += line;
    }
    if (b.has_bbo()) {
      std::snprintf(line, sizeof line, "%s-------- spread %5.2f --------%s\n", c_spr,
                    b.spread() / 1e4, c_off);
      f += line;
    }
    for (size_t i = 0; i < nb2; ++i) {
      const Level& L = b.bid.at(i);
      std::snprintf(line, sizeof line, "%sB %9.2f %7u %-3u %s%s\n", c_bid, L.price / 1e4,
                    L.shares, L.count, bar(L.shares, max_sh, 24).c_str(), c_off);
      f += line;
    }

    f += "----------------------------------------------------\n";
    for (const auto& t : tape) {
      const char* c = t.dir == '^' ? c_up : t.dir == 'v' ? c_dn : "";
      const char* arrow = t.dir == '^' ? "▲" : t.dir == 'v' ? "▼" : " ";
      std::snprintf(line, sizeof line, "%sT %s %s %9.2f x %-6u%s\n", c, fmt_ts(t.ts).c_str(),
                    arrow, t.px / 1e4, t.sh, c_off);
      f += line;
    }

    if (tty_render) {
      std::fputs("\x1b[H", stdout);
      std::fputs(f.c_str(), stdout);
      std::fputs("\x1b[J", stdout);
      std::fflush(stdout);
    } else {
      char name[512];
      std::snprintf(name, sizeof name, "%s/frame_%05llu.txt", frames_dir.c_str(),
                    (unsigned long long)n_frames++);
      FILE* ff = std::fopen(name, "w");
      if (!ff) { std::perror(name); return 2; }
      std::fwrite(f.data(), 1, f.size(), ff);
      std::fclose(ff);
    }
  }
  if (tty_render) {
    std::fputs("\x1b[?25h\n", stdout);
    std::printf("replay finished: %s played %.0fs of market time from %s (%llu msgs total). "
                "Longer: --duration SECONDS (full day: 23400).\n",
                want.c_str(), duration_s, fmt_ts(start_ns).c_str(),
                (unsigned long long)r.messages());
  }
  if (!frames_dir.empty())
    std::printf("wrote %llu frames to %s\n", (unsigned long long)n_frames, frames_dir.c_str());
  return 0;
}
