#!/bin/sh
# Download one historical ITCH day into data/. Usage: scripts/get_day.sh 01302019
# Days available (verified 2026-08-06): 15 dates Jan 2018 - Jan 2020, ~5 GB each.
set -eu
day="${1:?usage: get_day.sh MMDDYYYY}"
mkdir -p data
curl -L --fail --continue-at - -o "data/${day}.NASDAQ_ITCH50.gz" \
  "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${day}.NASDAQ_ITCH50.gz"
