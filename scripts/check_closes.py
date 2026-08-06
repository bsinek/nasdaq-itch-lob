#!/usr/bin/env python3
"""Cross-check reconstructed closing-cross prices against public records.

Fetches daily OHLCV from Yahoo's chart API (split-adjusted), un-adjusts with
the split factors below, and compares against the closing cross prices the
parser extracted into validation.json. For Nasdaq-listed symbols the Nasdaq
closing cross IS the official close, so an exact match is expected.

Also prints Nasdaq-venue volume / consolidated volume. That is a ratio, not a
match: ITCH sees only Nasdaq's ~quarter-to-third of these symbols' volume.

Usage: python3 scripts/check_closes.py data/validation-01302019.json
"""
import json
import subprocess
import sys
from datetime import datetime, timezone

# Cumulative split factor between the sample dates (2018-2020) and today;
# multiply Yahoo's adjusted close by this to recover the era price.
# AAPL 4:1 (2020-08); AMZN 20:1 (2022-06); NVDA 4:1 (2021-07) * 10:1 (2024-06);
# TSLA 5:1 (2020-08) * 3:1 (2022-08). FB trades as META since 2022-06.
SPLIT = {"AAPL": 4, "MSFT": 1, "AMZN": 20, "NVDA": 40, "AMD": 1, "INTC": 1,
         "FB": 1, "TSLA": 15}
YAHOO_TICKER = {"FB": "META"}


def fetch(symbol: str, day: str):
    t0 = int(datetime.strptime(day, "%m%d%Y").replace(tzinfo=timezone.utc).timestamp())
    url = (f"https://query1.finance.yahoo.com/v8/finance/chart/"
           f"{YAHOO_TICKER.get(symbol, symbol)}?period1={t0}&period2={t0 + 86400}&interval=1d")
    out = subprocess.run(["curl", "-s", "-A", "Mozilla/5.0", url],
                         capture_output=True, timeout=60, check=True)
    q = json.loads(out.stdout)["chart"]["result"][0]["indicators"]["quote"][0]
    return q["close"][0], q["volume"][0]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "data/validation-01302019.json"
    v = json.load(open(path))
    day = v["day"]
    print(f"day {day}  (consolidated data: Yahoo chart API, un-split-adjusted)")
    print(f"{'sym':5} {'itch close':>11} {'official':>11} {'match':>6} "
          f"{'nasdaq vol':>12} {'consol vol':>12} {'ratio':>6}")
    ok = 0
    for sym, s in v["symbols"].items():
        close_adj, vol_adj = fetch(sym, day)
        official = round(close_adj * SPLIT[sym], 2)
        consol = round(vol_adj / SPLIT[sym])  # Yahoo volume is split-adjusted too
        ours = round(s["close_cross_px"], 2)
        match = abs(ours - official) < 0.005
        ok += match
        print(f"{sym:5} {ours:11.2f} {official:11.2f} {'  yes' if match else '   NO':>6} "
              f"{s['nasdaq_volume']:12,} {consol:12,} {s['nasdaq_volume'] / consol:6.1%}")
    print(f"\nclosing-cross match: {ok}/{len(v['symbols'])}")
    sys.exit(0 if ok == len(v["symbols"]) else 1)


if __name__ == "__main__":
    main()
