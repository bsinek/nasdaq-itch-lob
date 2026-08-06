// itch-parse: parse a gzipped historical TotalView-ITCH 5.0 day.
//
//   itch-parse count    FILE.gz          per-type message counts
//   itch-parse bench    FILE.gz          throughput (end-to-end + parse/book-only)
//   itch-parse latency  FILE.gz          per-message latency distribution
//   itch-parse validate FILE.gz OUT.json validation counters -> json
//   itch-parse export   FILE.gz DIR      validation + snapshots.bin/orders.bin
//
// Every README number comes from one of these modes; nothing is throttled.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <sys/sysctl.h>
#include <vector>

#include "handler.hpp"
#include "hist.hpp"
#include "nanotime.hpp"
#include "reader.hpp"

using namespace itch;

static std::string hw_string() {
  char brand[128] = {0};
  size_t len = sizeof brand;
  sysctlbyname("machdep.cpu.brand_string", brand, &len, nullptr, 0);
  int ncpu = 0;
  len = sizeof ncpu;
  sysctlbyname("hw.ncpu", &ncpu, &len, nullptr, 0);
  uint64_t mem = 0;
  len = sizeof mem;
  sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
  char out[256];
  std::snprintf(out, sizeof out, "%s, %d cores, %llu GB RAM", brand, ncpu,
                (unsigned long long)(mem >> 30));
  return out;
}

// Median gap between consecutive timer reads: the effective timer resolution
// + call overhead, reported alongside latency percentiles (never subtracted).
static uint64_t timer_overhead_ns() {
  std::vector<uint64_t> d;
  d.reserve(100000);
  for (int i = 0; i < 100000; ++i) {
    const uint64_t a = now_ns();
    const uint64_t b = now_ns();
    d.push_back(b - a);
  }
  std::sort(d.begin(), d.end());
  return d[d.size() / 2];
}

static void print_counts(const Handler& h, uint64_t total) {
  static const char* names[] = {"S sys-event", "R directory", "H trade-action", "Y reg-sho",
                                "L mpp",       "V mwcb-lvl",  "W mwcb-status",  "K ipo-quote",
                                "J luld",      "h op-halt",   "A add",          "F add-mpid",
                                "E exec",      "C exec-px",   "X cancel",       "D delete",
                                "U replace",   "P trade",     "Q cross",        "B broken",
                                "I noii",      "N rpii",      "O dlcr"};
  static const char types[] = "SRHYLVWKJhAFECXDUPQBINO";
  std::printf("%-16s %15s\n", "type", "count");
  for (size_t i = 0; i < sizeof(types) - 1; ++i)
    std::printf("%-16s %15llu\n", names[i], (unsigned long long)h.counts()[uint8_t(types[i])]);
  std::printf("%-16s %15llu\n", "TOTAL", (unsigned long long)total);
}

static void write_validation_json(const Handler& h, const std::string& path,
                                  const std::string& day, uint64_t total_msgs) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) { std::perror(path.c_str()); std::exit(2); }
  const uint64_t exec_all = h.exec_ok() + h.exec_bad();
  const uint64_t mod_all = h.mod_ok() + h.mod_bad();
  std::fprintf(f, "{\n  \"day\": \"%s\",\n  \"total_messages\": %llu,\n", day.c_str(),
               (unsigned long long)total_msgs);
  std::fprintf(f, "  \"tracked_exec_msgs\": %llu,\n  \"tracked_exec_mismatches\": %llu,\n",
               (unsigned long long)exec_all, (unsigned long long)h.exec_bad());
  std::fprintf(f, "  \"execution_match_rate\": %.8f,\n",
               exec_all ? double(h.exec_ok()) / double(exec_all) : 0.0);
  std::fprintf(f, "  \"tracked_modify_msgs\": %llu,\n  \"tracked_modify_mismatches\": %llu,\n",
               (unsigned long long)mod_all, (unsigned long long)h.mod_bad());
  std::fprintf(f, "  \"modify_match_rate\": %.8f,\n",
               mod_all ? double(h.mod_ok()) / double(mod_all) : 0.0);
  std::fprintf(f, "  \"book_level_missing\": %llu,\n", (unsigned long long)h.book_missing());
  std::fprintf(f, "  \"symbols\": {\n");
  for (int s = 0; s < kNSyms; ++s) {
    std::fprintf(f,
                 "    \"%s\": {\"open_cross_px\": %.4f, \"close_cross_px\": %.4f, "
                 "\"nasdaq_volume\": %llu, \"book_exec_shares\": %llu, "
                 "\"crossed_instants\": %llu}%s\n",
                 kSymbols[s], h.open_cross_px(s) / 1e4, h.close_cross_px(s) / 1e4,
                 (unsigned long long)h.nasdaq_volume(s),
                 (unsigned long long)h.book_exec_shares(s),
                 (unsigned long long)h.crossed_instants(s), s + 1 < kNSyms ? "," : "");
  }
  std::fprintf(f, "  }\n}\n");
  std::fclose(f);
  std::printf("wrote %s\n", path.c_str());
}

static int run(int argc, char** argv);

// Reader throws on unopenable/corrupt/truncated input; catch so those exit
// cleanly with status 2 like the framing errors, instead of via std::terminate.
int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 2;
  }
}

static int run(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s count|bench|latency|validate|export FILE.gz [OUT]\n", argv[0]);
    return 1;
  }
  const std::string mode = argv[1], path = argv[2];
  std::string day = path;
  if (auto p = day.find_last_of('/'); p != std::string::npos) day = day.substr(p + 1);
  day = day.substr(0, day.find('.'));

  Reader r(path);
  Handler h;

  if (mode == "export") {
    if (argc < 4) { std::fprintf(stderr, "export needs DIR\n"); return 1; }
    h.enable_export(argv[3]);
  }

  const uint64_t t0 = now_ns();
  uint16_t len;

  if (mode == "latency") {
    Hist hist;
    while (const uint8_t* m = r.next(len)) {
      const uint64_t a = now_ns();
      h.on_message(m);
      const uint64_t b = now_ns();
      hist.add(b - a);
    }
    const uint64_t wall = now_ns() - t0;
    h.finish();
    std::printf("hardware: %s\n", hw_string().c_str());
    std::printf("messages: %llu\n", (unsigned long long)r.messages());
    std::printf("timer overhead+resolution (median back-to-back read): %llu ns\n",
                (unsigned long long)timer_overhead_ns());
    std::printf("latency of Handler::on_message (decode+dispatch+book), ns:\n");
    std::printf("  mean %.1f  p50 %llu  p99 %llu  p99.9 %llu  max %llu  >16us %llu\n",
                hist.mean(), (unsigned long long)hist.pct(0.50),
                (unsigned long long)hist.pct(0.99), (unsigned long long)hist.pct(0.999),
                (unsigned long long)hist.max(), (unsigned long long)hist.overflow());
    std::printf("note: run wall time %.1f s (timing every message slows the run; "
                "throughput numbers come from bench mode)\n", wall / 1e9);
    return 0;
  }

  while (const uint8_t* m = r.next(len)) h.on_message(m);
  const uint64_t wall = now_ns() - t0;
  h.finish();
  const uint64_t work_ns = wall - r.inflate_ns();

  if (mode == "count" || mode == "bench") {
    if (mode == "count") print_counts(h, r.messages());
    std::printf("hardware: %s (single-threaded hot path)\n", hw_string().c_str());
    std::printf("messages: %llu, decompressed bytes: %llu\n", (unsigned long long)r.messages(),
                (unsigned long long)r.bytes_out());
    std::printf("wall: %.3f s  (gzip inflate: %.3f s, parse+book: %.3f s)\n", wall / 1e9,
                r.inflate_ns() / 1e9, work_ns / 1e9);
    std::printf("throughput end-to-end (incl. inflate): %.2f M msg/s\n",
                double(r.messages()) / (wall / 1e3) );
    std::printf("throughput parse+book only:            %.2f M msg/s\n",
                double(r.messages()) / (work_ns / 1e3));
  } else if (mode == "validate" || mode == "export") {
    const uint64_t exec_all = h.exec_ok() + h.exec_bad();
    const uint64_t mod_all = h.mod_ok() + h.mod_bad();
    std::printf("day %s: %llu messages\n", day.c_str(), (unsigned long long)r.messages());
    std::printf("execution match: %llu/%llu (%.6f%%), modify match: %llu/%llu (%.6f%%), "
                "book-level-missing: %llu\n",
                (unsigned long long)h.exec_ok(), (unsigned long long)exec_all,
                exec_all ? 100.0 * double(h.exec_ok()) / double(exec_all) : 0.0,
                (unsigned long long)h.mod_ok(), (unsigned long long)mod_all,
                mod_all ? 100.0 * double(h.mod_ok()) / double(mod_all) : 0.0,
                (unsigned long long)h.book_missing());
    for (int s = 0; s < kNSyms; ++s)
      std::printf("  %-5s open %.4f close %.4f nasdaq_vol %llu crossed_instants %llu\n",
                  kSymbols[s], h.open_cross_px(s) / 1e4, h.close_cross_px(s) / 1e4,
                  (unsigned long long)h.nasdaq_volume(s),
                  (unsigned long long)h.crossed_instants(s));
    if (mode == "export")
      std::printf("exported %llu snapshots, %llu order rows to %s\n",
                  (unsigned long long)h.snapshots_written(), (unsigned long long)h.order_rows(),
                  argv[3]);
    const std::string out = mode == "validate"
                                ? (argc >= 4 ? argv[3] : "validation.json")
                                : std::string(argv[3]) + "/validation.json";
    write_validation_json(h, out, day, r.messages());
  } else {
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 1;
  }
  return 0;
}
