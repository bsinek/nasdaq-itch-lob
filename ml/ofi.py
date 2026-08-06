"""Order-flow imbalance per Cont, Kukanov & Stoikov (2014), from L1 snapshots.

Event contribution at snapshot n (P=best price, q=best size):
    e_n =  1{Pb_n >= Pb_{n-1}} qb_n - 1{Pb_n <= Pb_{n-1}} qb_{n-1}
         - 1{Pa_n <= Pa_{n-1}} qa_n + 1{Pa_n >= Pa_{n-1}} qa_{n-1}
OFI(t, h) = sum of e_n over events in (t-h, t].

All lookups here are backward-only (searchsorted side='right' on event
timestamps <= t), so joined features can never see past the query time.
"""
import numpy as np


class OfiSeries:
    """Per-symbol L1 event series with O(log n) windowed-OFI queries."""

    def __init__(self, snaps: np.ndarray):
        """snaps: SNAP_DTYPE records for ONE symbol, time-ordered, with a
        non-empty L1 on both sides (caller filters)."""
        self.ts = snaps["ts"].astype(np.int64)
        pb = snaps["bid_px"][:, 0].astype(np.int64)
        qb = snaps["bid_sz"][:, 0].astype(np.int64)
        pa = snaps["ask_px"][:, 0].astype(np.int64)
        qa = snaps["ask_sz"][:, 0].astype(np.int64)
        e = np.zeros(len(snaps))
        e[1:] = ((pb[1:] >= pb[:-1]) * qb[1:] - (pb[1:] <= pb[:-1]) * qb[:-1]
                 - (pa[1:] <= pa[:-1]) * qa[1:] + (pa[1:] >= pa[:-1]) * qa[:-1])
        self.csum = np.concatenate([[0.0], np.cumsum(e)])
        self.mid = (pb + pa) / 2.0

    def ofi(self, t: np.ndarray, horizon_ns: int) -> np.ndarray:
        """OFI over (t - horizon, t] for each query time t (ns)."""
        hi = np.searchsorted(self.ts, t, side="right")
        lo = np.searchsorted(self.ts, t - horizon_ns, side="right")
        return self.csum[hi] - self.csum[lo]

    def mid_at(self, t: np.ndarray) -> np.ndarray:
        """Midprice of the last event <= t; NaN before the first event."""
        i = np.searchsorted(self.ts, t, side="right") - 1
        out = np.where(i >= 0, self.mid[np.maximum(i, 0)], np.nan)
        return out
