#!/usr/bin/env python3
"""Target 2: fill probability conditional on queue position at insertion.

One row per resting limit order added within 5 ticks of the same-side touch
(see OrderRec in src/handler.hpp). Label = the order executed >= 1 share
before dying / end of day — ground truth read directly off the stream, which
is only possible with order-level (L3) data. Replaced orders are dropped
(continuation ambiguous); orders alive at EOD count as unfilled.

Features are strictly from insertion time; the OFI join is backward-only and
asserted as such. Train and test are separate days.

Usage: .venv/bin/python ml/fills.py data/exports/01302019 data/exports/03272019
"""
import sys

import numpy as np
import xgboost as xgb
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score

from loaders import SYMBOLS, TICK, load_orders, load_snapshots
from ofi import OfiSeries

FEATURES = ["queue_ahead", "dist_ticks", "size", "spread_ticks", "level_ct",
            "same_depth5", "opp_depth5", "d5_imb", "is_bid", "hour", "ofi_1s_signed", "sym"]


def build_day(export_dir: str):
    orders = load_orders(export_dir)
    orders = orders[orders["outcome"] != ord("R")]  # drop replaced (ambiguous)
    snaps = load_snapshots(export_dir)

    X = np.empty((len(orders), len(FEATURES)), dtype=np.float64)
    y = (orders["outcome"] == ord("F")).astype(np.int8)
    row = 0
    for s in range(len(SYMBOLS)):
        o = orders[orders["sym"] == s]
        if not len(o):
            continue
        sn = snaps[snaps["sym"] == s]
        sn = sn[(sn["bid_px"][:, 0] > 0) & (sn["ask_px"][:, 0] > 0)]
        series = OfiSeries(sn)
        t = o["ts_add"].astype(np.int64)
        # leakage guard: the OFI window ends at the last event <= t
        idx = np.searchsorted(series.ts, t, side="right") - 1
        assert (series.ts[np.maximum(idx, 0)] <= t).all()
        ofi1 = series.ofi(t, 1_000_000_000)
        is_bid = (o["side"] == ord("B")).astype(np.float64)
        # signed OFI: positive = flow supports the order's side
        ofi_signed = np.where(is_bid == 1, ofi1, -ofi1)
        d5b = o["same_depth5"] * is_bid + o["opp_depth5"] * (1 - is_bid)
        d5a = o["opp_depth5"] * is_bid + o["same_depth5"] * (1 - is_bid)
        cols = {
            "queue_ahead": o["queue_ahead"], "dist_ticks": o["dist_ticks"],
            "size": o["sz"], "spread_ticks": o["spread"] / TICK,
            "level_ct": o["level_ct"], "same_depth5": o["same_depth5"],
            "opp_depth5": o["opp_depth5"],
            "d5_imb": (d5b - d5a) / np.maximum(d5b + d5a, 1),
            "is_bid": is_bid, "hour": t / 3.6e12,
            "ofi_1s_signed": ofi_signed, "sym": np.full(len(o), s),
        }
        m = np.where(orders["sym"] == s)[0]
        for j, f in enumerate(FEATURES):
            X[m, j] = np.asarray(cols[f], dtype=np.float64)
        row += len(o)
    return X, np.asarray(y), orders


def main():
    train_dir, test_dir = sys.argv[1], sys.argv[2]
    Xtr, ytr, otr = build_day(train_dir)
    Xte, yte, _ = build_day(test_dir)
    print(f"train {len(ytr):,} orders (fill rate {ytr.mean():.3f}), "
          f"test {len(yte):,} orders (fill rate {yte.mean():.3f})")

    n = len(ytr)
    cut = int(n * 0.8)
    model = xgb.XGBClassifier(
        n_estimators=800, max_depth=7, learning_rate=0.08, subsample=0.8,
        colsample_bytree=0.8, early_stopping_rounds=30, eval_metric="auc",
        n_jobs=8, random_state=7)
    model.fit(Xtr[:cut], ytr[:cut], eval_set=[(Xtr[cut:], ytr[cut:])], verbose=False)
    p = model.predict_proba(Xte)[:, 1]
    auc = roc_auc_score(yte, p)

    # context baseline: logistic regression on queue position + distance alone
    qd = [FEATURES.index("queue_ahead"), FEATURES.index("dist_ticks")]
    base = LogisticRegression(max_iter=1000)
    base.fit(np.log1p(Xtr[:, qd]), ytr)
    p_base = base.predict_proba(np.log1p(Xte[:, qd]))[:, 1]
    auc_base = roc_auc_score(yte, p_base)

    print(f"\nout-of-sample ROC-AUC: XGBoost {auc:.4f}   "
          f"logistic(queue,dist) baseline {auc_base:.4f}")

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 2, figsize=(11, 4))
    bins = np.linspace(0, 1, 21)
    which = np.digitize(p, bins) - 1
    centers, obs, cnt = [], [], []
    for b in range(20):
        m = which == b
        if m.sum() > 200:
            centers.append(p[m].mean())
            obs.append(yte[m].mean())
            cnt.append(m.sum())
    axes[0].plot([0, 1], [0, 1], "--", color="#999", lw=1, label="perfect")
    axes[0].plot(centers, obs, "o-", color="#4a7dbd", label="XGBoost")
    axes[0].set_xlabel("predicted fill probability")
    axes[0].set_ylabel("observed fill rate (test day)")
    axes[0].set_title(f"Calibration — test-day AUC {auc:.3f}")
    axes[0].legend()
    imp = model.feature_importances_
    order = np.argsort(imp)
    axes[1].barh([FEATURES[i] for i in order], imp[order], color="#4a7dbd")
    axes[1].set_title("Feature importance (gain)")
    fig.tight_layout()
    fig.savefig("docs/assets/fill_model.png", dpi=120)
    print("wrote docs/assets/fill_model.png")


if __name__ == "__main__":
    main()
