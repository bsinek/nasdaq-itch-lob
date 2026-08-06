# nasdaq-itch-lob

A C++ feed handler for **raw Nasdaq TotalView-ITCH 5.0** binary data — full-day
parse, limit-order-book reconstruction, validation against the stream's own
ground truth, and an ML layer (XGBoost) built on the book's output. No LOBSTER,
no FI-2010, no pre-processed datasets: the input is the exchange's binary wire
format, ~368–422M messages per day.

![AAPL replay dashboard, first minutes after the 09:30 open](docs/assets/book.gif)

*Replay dashboard (`itch-replay`): midprice sparkline and live 1 s
order-flow-imbalance gauge (the ML layer's main feature), 5 s candlesticks with
a buy/sell volume profile on a shared price axis, the price ladder, and a trade
tape with aggressor direction — gray `·` prints are hidden-order executions
inside the spread, whose aggressor is unknowable from the feed.*

## Measured results

All numbers below are produced by the commands in **Reproduce**; none are
estimates. Hardware: Apple M4 (10 cores, 16 GB RAM), macOS, Apple clang 17,
`-O3`; the hot path is **single-threaded**. Day 01/30/2019: 368,366,634
messages, 11.2 GB decompressed (streamed — never written to disk).

| Metric | Value | Command |
|---|---|---|
| Throughput, end-to-end (incl. gzip inflate) | **19.7 M msg/s** | `itch-parse bench` (median of 3) |
| Throughput, parse + book update only | **51.8 M msg/s** | `itch-parse bench` (median of 3) |
| Per-message latency, mean | **19.3 ns** | derived: parse+book time / messages |
| Per-message latency, p50 | **< 42 ns** (below timer tick) | `itch-parse latency` |
| Per-message latency, p99 / p99.9 | **250 ns / 1.04 µs** | `itch-parse latency` |
| Execution match rate (both days) | **100.000000 %** (492,275 + 273,503 execs) | `itch-parse export` → `validation.json` |
| Modify match rate (both days) | **100.000000 %** (5.37 M + 5.73 M msgs) | same |
| Crossed-book instants in regular session | **0** (all 8 symbols, both days) | same |
| Closing cross vs official close | **16/16 exact to the cent** | `scripts/check_closes.py` (both days) |
| Midprice model, out-of-sample R² (1 s horizon) | **0.0006** pooled, −0.000–0.002 per symbol | `ml/midprice.py` |
| Fill-probability model, out-of-sample AUC | **0.754** (queue+distance logistic baseline: 0.730) | `ml/fills.py` |

Latency = `Handler::on_message` (field decode + dispatch + book update),
excluding stream inflation; distribution measured per message over all 368 M.
The Apple Silicon counter ticks at 24 MHz (41.7 ns), so p50 sits below one
tick; the mean comes from the unthrottled aggregate. Timer overhead is
reported by the tool, never subtracted.

## What it does

- **Parses all 23 ITCH 5.0 message types** from the official spec — add,
  add-with-MPID, execute, execute-with-price, cancel, delete, replace, trade,
  cross, system events, and the administrative set. Field offsets transcribed
  from Nasdaq's spec PDF, framing verified byte-by-byte (2-byte big-endian
  length prefix per message in the historical files, unlike the live feed's
  MoldUDP64). Unknown type or wrong length = hard error, not a skip.
- **Reconstructs price-level books** for 8 liquid symbols (AAPL MSFT AMZN NVDA
  AMD INTC FB TSLA) while parsing every message on the wire — the
  keep-books-for-what-you-trade pattern real handlers use; memory stays
  bounded via locate-code filtering at add time.
- **Validates against the stream itself**: every execution/cancel/delete/replace
  must apply cleanly to the reconstructed book — order present, shares
  sufficient, removal lands on the order's stored price level (executions are
  implicitly at the stored order's price; no separate price comparison is
  performed). The Nasdaq closing cross *is* the official close for
  Nasdaq-listed names, giving an exact external check — 16/16 across both
  days. Venue volume vs consolidated tape is reported as a ratio (19–37 %)
  because Nasdaq is one venue among ~16; an exact match is impossible by
  construction.
- **Exports the book's own output for ML**: L5 snapshots on every top-5
  change (~7.0–7.2 M/day; replaces emit one atomic post-replace state) and a
  lifecycle record for every resting order near the touch (~3.4–3.7 M/day)
  with its fate — filled, cancelled, replaced, or alive at close — read
  directly off the stream. That labeling is the L3-only part: with
  price-level (L2) data you never learn an individual order's outcome.

## ML results, honestly stated

**Midprice (weak, expectedly — and weaker after an honesty fix).** Order-flow
imbalance features (Cont, Kukanov & Stoikov 2014) at 100 ms/1 s/5 s plus book
shape, sampled every 500 ms, predicting the 1 s mid change. Trained on
01/30/2019, tested on 03/27/2019. Out-of-sample R² = **0.0006** pooled
(per-symbol −0.000 to 0.002). An earlier build measured 0.0017: an export
artifact was decomposing atomic replace messages into remove+re-add snapshot
pairs, and the exaggerated OFI swings encoded quote-update direction. On the
true book states the signal shrinks — kept and reported because that fragility
*is* the result. The plan pre-registered R² ≈ 0.001–0.02 as the honest range
and >0.05 as presumptive leakage; the final number sits at the very bottom of
it.

**Fill probability (the interesting half).** P(fill ≥ 1 share while resting
under its reference) for every recorded order, given its queue position at
insertion — nothing dropped, so sample membership never conditions on the
outcome (orders replaced before filling count as unfilled; the replacement is
its own row). Test-day AUC **0.754** vs 0.730 for a logistic baseline on
queue position + distance alone; feature importances rank distance-from-touch,
level order-count, and order size first, as they should. Calibration is
monotone but uniformly over-confident on the test day — the base fill rate
fell from 9.4 % (train, an FOMC session) to 5.5 %, a real regime shift
reported rather than tuned away. Plots: `docs/assets/fill_model.png`,
`docs/assets/midprice_importance.png`.

## Reproduce

Builds as-is on **macOS/Apple clang only** (mach timer + sysctl; zero C++
dependencies beyond system zlib).

```sh
scripts/get_spec.sh                      # official spec PDF (not committed)
scripts/get_day.sh 01302019              # ~4.8 GB from emi.nasdaq.com
scripts/get_day.sh 03272019              # ~5.5 GB
make all && make test

./build/itch-parse bench   data/01302019.NASDAQ_ITCH50.gz    # run 3x, quote the median
./build/itch-parse latency data/01302019.NASDAQ_ITCH50.gz    # latency distribution
./build/itch-parse export  data/01302019.NASDAQ_ITCH50.gz data/exports/01302019
./build/itch-parse export  data/03272019.NASDAQ_ITCH50.gz data/exports/03272019
python3 scripts/check_closes.py data/exports/01302019/validation.json   # 8/8 …
python3 scripts/check_closes.py data/exports/03272019/validation.json   # … + 8/8 = 16/16

python3 -m venv .venv && .venv/bin/pip install -r ml/requirements.txt
brew install libomp                      # OpenMP runtime for the xgboost wheel
.venv/bin/python ml/midprice.py data/exports/01302019 data/exports/03272019
.venv/bin/python ml/fills.py    data/exports/01302019 data/exports/03272019
```

Or `scripts/run_all.sh` for the whole sequence (~30 min download + ~10 min
compute). The replay demo (entirely separate from the benchmarks):

```sh
./build/itch-replay data/01302019.NASDAQ_ITCH50.gz AAPL --speed 60 --start 09:30:00 --duration 23400
```

Flags: `--speed N` (replay rate multiple), `--duration SEC` of market time
(23400 = full 09:30–16:00 session), `--candle SEC` (candle interval),
`--depth N` (ladder levels per side). The dashboard is ~36 rows; use a tall
terminal window.

## Design notes

- Single-threaded hot path: chunked zlib inflate → framing → switch dispatch →
  book update. Throughput is measured both end-to-end and with inflate time
  excluded (timed per 8 MB chunk), and both are reported.
- Price levels are sorted vectors, best-at-front: activity clusters at the
  touch, so linear scans beat tree maps on real data. Order store is
  `std::unordered_map` (day-unique u64 refs), tracked symbols only.
- Replace = delete + fresh add under the new reference, inheriting side/symbol
  from the original per spec §1.4.5; non-printable executions are excluded
  from volume to avoid double counting the cross prints.
- The validation gate ran before any ML: a book that doesn't reconcile makes
  every downstream number meaningless.

## Limitations

- **Single venue.** Nasdaq is a minority of total US equity volume; the other
  ~15 exchanges plus off-exchange flow are invisible here. Nasdaq-venue share
  of the tracked names measured 19–37 % on these days.
- **Sample dates are month-end** (Nasdaq's free sample days), so they may be
  atypical sessions; 01/30/2019 was additionally an FOMC day.
- One train day, one test day, 8 symbols. Fill-model calibration visibly
  degrades under the day-over-day fill-rate shift; more days would be the
  first improvement.
- Historical-file framing (2-byte length prefix), not the live feed's
  MoldUDP64/SoupBinTCP transport; no gap/heartbeat handling.
- Hidden (non-displayed) liquidity appears only when it executes (P messages);
  the reconstructed book is the displayed book.
- Fill labels are per order *reference*: an order replaced before any fill
  counts as unfilled and its replacement leg becomes a fresh row, so orders
  near the close are right-censored (mechanically cancelled at end of day and
  labeled unfilled).
- The closing-cross external check depends on Yahoo's unauthenticated chart
  API and a hardcoded split-factor table in `scripts/check_closes.py`; both
  can rot (the split table drifts the day any tracked name splits again).
- Builds on macOS/Apple Silicon only as shipped; the latency distribution
  spans all messages including untracked-symbol dispatches (the p50 reflects
  decode+dispatch; tracked book updates dominate the tail).
- Replay pacing is sleep-based and coarse — it is a demo, and it is never the
  thing being benchmarked.

## Layout

```
src/        messages.hpp reader book handler hist nanotime itch_parse itch_replay
tests/      book_test.cpp — scripted sequences with known outcomes
ml/         loaders ofi midprice fills (+ requirements.txt)
scripts/    get_day get_spec check_closes render_gif run_all
docs/       DECISIONS.md, plans/, assets/
```

An independent three-way audit (C++/spec conformance, ML leakage, claim
traceability) ran on 2026-08-06 before release; every finding was fixed or
disclosed above, and the fixes are in the git history.
