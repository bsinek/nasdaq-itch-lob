"""Readers for the two binary exports written by `itch-parse export`.

Record layouts mirror SnapRec/OrderRec in src/handler.hpp (native little-endian,
packed). Prices are Price(4): 1 unit = $0.0001; one tick ($0.01) = 100 units.
"""
import numpy as np

TICK = 100  # Price(4) units per $0.01

SYMBOLS = ["AAPL", "MSFT", "AMZN", "NVDA", "AMD", "INTC", "FB", "TSLA"]

SNAP_DTYPE = np.dtype([
    ("ts", "<u8"), ("sym", "<u2"), ("reason", "u1"), ("_pad", "V5"),
    ("bid_px", "<u4", (5,)), ("bid_sz", "<u4", (5,)),
    ("ask_px", "<u4", (5,)), ("ask_sz", "<u4", (5,)),
])
assert SNAP_DTYPE.itemsize == 96

ORDER_DTYPE = np.dtype([
    ("ts_add", "<u8"), ("ref", "<u8"), ("sym", "<u2"), ("side", "u1"),
    ("dist_ticks", "u1"), ("px", "<u4"), ("sz", "<u4"), ("queue_ahead", "<u4"),
    ("level_ct", "<u4"), ("same_depth5", "<u4"), ("opp_depth5", "<u4"),
    ("spread", "<u4"), ("outcome", "u1"), ("_pad", "V3"), ("ts_outcome", "<u8"),
    ("exec_shares", "<u4"),
])
assert ORDER_DTYPE.itemsize == 64


def load_snapshots(export_dir: str) -> np.ndarray:
    return np.fromfile(f"{export_dir}/snapshots.bin", dtype=SNAP_DTYPE)


def load_orders(export_dir: str) -> np.ndarray:
    return np.fromfile(f"{export_dir}/orders.bin", dtype=ORDER_DTYPE)


def hours(ts_ns) -> np.ndarray:
    """ns-since-midnight -> fractional hours (ET session clock)."""
    return np.asarray(ts_ns, dtype=np.float64) / 3.6e12
