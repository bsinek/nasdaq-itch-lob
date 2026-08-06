#!/bin/sh
# Fetch the official TotalView-ITCH 5.0 specification (not committed: Nasdaq's document).
set -eu
mkdir -p docs/spec
curl -sL -o docs/spec/NQTVITCHSpecification.pdf \
  "https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf"
