#!/usr/bin/env python3
"""Target 1: short-horizon midprice change from OFI + book-shape features.

Pre-registered expectation (docs/plans/2026-08-06-itch-lob.md): out-of-sample
R^2 ~ 0.001-0.02. Anything above 0.05 is treated as evidence of leakage, not
success. Train and test are separate days; every feature is computed from
data at or before the sample time (backward-only searchsorted).

Usage: .venv/bin/python ml/midprice.py data/exports/01302019 data/exports/03272019
"""
import sys

import numpy as np
import xgboost as xgb

from loaders import SYMBOLS, TICK, load_snapshots
from ofi import OfiSeries

SAMPLE_MS = 500
SESSION = (int(9.583 * 3.6e12), int(15.917 * 3.6e12))  # 09:35 - 15:55
HORIZON_NS = 1_000_000_000  # predict mid(t+1s) - mid(t)
OFI_WINDOWS = {"ofi_100ms": 100_000_000, "ofi_1s": 1_000_000_000, "ofi_5s": 5_000_000_000}

FEATURES = ["spread_ticks", "l1_imb", "d5_imb", "ret_1s", "ret_5s",
            "ofi_100ms", "ofi_1s", "ofi_5s", "sym"]


def build_day(export_dir: str):
    snaps = load_snapshots(export_dir)
    rows, targets = [], []
    for s in range(len(SYMBOLS)):
        sn = snaps[snaps["sym"] == s]
        sn = sn[(sn["bid_px"][:, 0] > 0) & (sn["ask_px"][:, 0] > 0)]
        series = OfiSeries(sn)
        t = np.arange(SESSION[0], SESSION[1], SAMPLE_MS * 1_000_000, dtype=np.int64)

        i = np.searchsorted(series.ts, t, side="right") - 1
        valid = i >= 0
        t, i = t[valid], i[valid]
        pb = sn["bid_px"][i, 0].astype(np.float64)
        qb = sn["bid_sz"][i, 0].astype(np.float64)
        pa = sn["ask_px"][i, 0].astype(np.float64)
        qa = sn["ask_sz"][i, 0].astype(np.float64)
        mid = (pb + pa) / 2
        d5b = sn["bid_sz"][i].sum(axis=1).astype(np.float64)
        d5a = sn["ask_sz"][i].sum(axis=1).astype(np.float64)

        feat = {
            "spread_ticks": (pa - pb) / TICK,
            "l1_imb": (qb - qa) / (qb + qa),
            "d5_imb": (d5b - d5a) / (d5b + d5a),
            "ret_1s": (mid - series.mid_at(t - 1_000_000_000)) / TICK,
            "ret_5s": (mid - series.mid_at(t - 5_000_000_000)) / TICK,
            **{k: series.ofi(t, h) for k, h in OFI_WINDOWS.items()},
            "sym": np.full(len(t), s, dtype=np.float64),
        }
        y = (series.mid_at(t + HORIZON_NS) - mid) / TICK

        keep = ~np.isnan(y)
        for v in feat.values():
            keep &= ~np.isnan(np.asarray(v, dtype=np.float64))
        rows.append(np.column_stack([np.asarray(feat[f], dtype=np.float64)[keep]
                                     for f in FEATURES]))
        targets.append(y[keep])
    return np.vstack(rows), np.concatenate(targets), rows, targets


def r2(y, p):
    ss = np.sum((y - p) ** 2)
    return 1 - ss / np.sum((y - np.mean(y)) ** 2)


def main():
    train_dir, test_dir = sys.argv[1], sys.argv[2]
    _, _, tr_rows, tr_targets = build_day(train_dir)
    Xte, yte, sym_rows, sym_targets = build_day(test_dir)

    # early stopping on a chronological tail: the last 20% of EACH symbol's
    # day (rows are time-ordered within a symbol), so every symbol appears in
    # both fit and eval — the test day is never touched during fit
    fit_X, fit_y, ev_X, ev_y = [], [], [], []
    for Xs, ys in zip(tr_rows, tr_targets):
        c = int(len(ys) * 0.8)
        fit_X.append(Xs[:c]); fit_y.append(ys[:c])
        ev_X.append(Xs[c:]); ev_y.append(ys[c:])
    fit_X, fit_y = np.vstack(fit_X), np.concatenate(fit_y)
    ev_X, ev_y = np.vstack(ev_X), np.concatenate(ev_y)
    print(f"train {len(fit_y):,}+{len(ev_y):,} samples ({train_dir}), "
          f"test {Xte.shape[0]:,} samples ({test_dir})")

    model = xgb.XGBRegressor(
        n_estimators=600, max_depth=6, learning_rate=0.05, subsample=0.8,
        colsample_bytree=0.8, early_stopping_rounds=30, eval_metric="rmse",
        n_jobs=8, random_state=7)
    model.fit(fit_X, fit_y, eval_set=[(ev_X, ev_y)], verbose=False)

    pred = model.predict(Xte)
    print(f"\nout-of-sample R^2 (mid change over next 1s, in ticks): "
          f"{r2(yte, pred):.4f}")
    print("reference: the zero predictor scores R^2 ~ 0 here only because the "
          "mean 1s mid change is ~0 (it is not exactly 0 by construction)")
    print("\nper-symbol out-of-sample R^2:")
    for s, (Xs, ys) in enumerate(zip(sym_rows, sym_targets)):
        ps = model.predict(Xs)
        print(f"  {SYMBOLS[s]:5} {r2(ys, ps):8.4f}   ({len(ys):,} samples)")

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    imp = model.feature_importances_
    order = np.argsort(imp)
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.barh([FEATURES[i] for i in order], imp[order], color="#4a7dbd")
    ax.set_title("Midprice model: XGBoost feature importance (gain)")
    fig.tight_layout()
    fig.savefig("docs/assets/midprice_importance.png", dpi=120)
    print("\nwrote docs/assets/midprice_importance.png")


if __name__ == "__main__":
    main()
