#!/bin/sh
# Reproduce every number in the README from scratch. Approximate total time on
# an Apple M4 with a fast connection: ~30 min download + ~10 min compute.
# Prereqs: clang++, zlib (system), python3, and for the ML layer:
#   python3 -m venv .venv && .venv/bin/pip install -r ml/requirements.txt
#   brew install libomp   # OpenMP runtime, required by the xgboost wheel
set -eux

TRAIN=01302019
TEST=03272019

[ -f "data/$TRAIN.NASDAQ_ITCH50.gz" ] || scripts/get_day.sh $TRAIN   # ~4.8 GB
[ -f "data/$TEST.NASDAQ_ITCH50.gz" ]  || scripts/get_day.sh $TEST    # ~5.0 GB

make all
make test                                                     # book unit tests

# benchmarks (unthrottled; run 3x and quote the median)
./build/itch-parse bench   "data/$TRAIN.NASDAQ_ITCH50.gz"
./build/itch-parse bench   "data/$TRAIN.NASDAQ_ITCH50.gz"
./build/itch-parse bench   "data/$TRAIN.NASDAQ_ITCH50.gz"
./build/itch-parse latency "data/$TRAIN.NASDAQ_ITCH50.gz"

# validation + ML exports (validation.json lands next to the exports)
mkdir -p "data/exports/$TRAIN" "data/exports/$TEST"
./build/itch-parse export "data/$TRAIN.NASDAQ_ITCH50.gz" "data/exports/$TRAIN"
./build/itch-parse export "data/$TEST.NASDAQ_ITCH50.gz"  "data/exports/$TEST"
python3 scripts/check_closes.py "data/exports/$TRAIN/validation.json"
python3 scripts/check_closes.py "data/exports/$TEST/validation.json"

# ML (venv required)
.venv/bin/python ml/midprice.py "data/exports/$TRAIN" "data/exports/$TEST"
.venv/bin/python ml/fills.py    "data/exports/$TRAIN" "data/exports/$TEST"

# demo GIF (separate from benchmarks; paced replay)
mkdir -p data/frames
./build/itch-replay "data/$TRAIN.NASDAQ_ITCH50.gz" AAPL --speed 30 \
  --start 09:30:00 --duration 240 --frames-dir data/frames
.venv/bin/python scripts/render_gif.py data/frames docs/assets/book.gif
