#!/usr/bin/env python3
"""
Train a binary XGBoost fraud classifier from references and generate
compile-time C inference code for the server.

Input:
- resources/references.json.gz

Outputs:
- apps/server/src/xgboost_model.h
- apps/server/src/xgboost_model.c
"""

from __future__ import annotations

import argparse
import gzip
import json
import math
import random
from datetime import datetime, timezone
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import xgboost as xgb

INPUT_PATH = Path("resources/references.json.gz")
TEST_DATA_INPUT_PATH = Path("test/test-data.json")
OUT_H = Path("apps/server/src/xgboost_model.h")
OUT_C = Path("apps/server/src/xgboost_model.c")

DIMS = 14
FP_WEIGHT = 1.0
FN_WEIGHT = 3.0
TX2_MCC_RISK = {
    "5411": 0.15,
    "5812": 0.30,
    "5912": 0.20,
    "5944": 0.45,
    "7801": 0.80,
    "7802": 0.75,
    "7995": 0.85,
    "4511": 0.35,
    "5311": 0.25,
    "5999": 0.50,
}


@dataclass
class FlatTree:
    feature: list[int]
    left: list[int]
    right: list[int]
    threshold: list[float]
    leaf: list[float]

    @property
    def node_count(self) -> int:
        return len(self.feature)


def load_rows(path: Path) -> list[dict]:
    with gzip.open(path, "rt", encoding="utf-8") as f:
        rows = json.load(f)
    if not isinstance(rows, list) or not rows:
        raise ValueError("invalid or empty references dataset")
    return rows


def to_xy(rows: list[dict]) -> tuple[np.ndarray, np.ndarray]:
    n = len(rows)
    x = np.empty((n, DIMS), dtype=np.float32)
    y = np.empty((n,), dtype=np.int32)
    for i, row in enumerate(rows):
        vec = row.get("vector")
        if not isinstance(vec, list) or len(vec) != DIMS:
            raise ValueError(f"invalid vector at index {i}")
        x[i, :] = vec
        y[i] = 1 if row.get("label") == "fraud" else 0
    return x, y


def clamp_01(v: float) -> float:
    if v < 0.0:
        return 0.0
    if v > 1.0:
        return 1.0
    return v


def parse_epoch_utc(iso_ts: str) -> int:
    dt = datetime.strptime(iso_ts, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    return int(dt.timestamp())


def day_of_week_from_epoch(epoch: int) -> int:
    days = epoch // 86400
    return int((days + 3) % 7)


def hour_of_day_from_epoch(epoch: int) -> int:
    seconds = epoch % 86400
    if seconds < 0:
        seconds += 86400
    return int(seconds // 3600)


def request_to_vector(request: dict) -> list[float]:
    tx = request["transaction"]
    customer = request["customer"]
    merchant = request["merchant"]
    terminal = request["terminal"]
    last_tx = request.get("last_transaction")

    amount = float(tx["amount"])
    installments = int(tx["installments"])
    requested_at = parse_epoch_utc(tx["requested_at"])

    customer_avg = float(customer["avg_amount"])
    customer_tx_count_24h = int(customer["tx_count_24h"])
    known_merchants = customer.get("known_merchants", [])

    merchant_id = merchant["id"]
    merchant_mcc = merchant["mcc"]
    merchant_avg_amount = float(merchant["avg_amount"])

    terminal_is_online = 1.0 if terminal["is_online"] else 0.0
    terminal_card_present = 1.0 if terminal["card_present"] else 0.0
    terminal_km_from_home = float(terminal["km_from_home"])

    has_last = last_tx is not None
    minutes_since_last = -1.0
    km_from_last_tx = -1.0
    if has_last:
        last_ts = parse_epoch_utc(last_tx["timestamp"])
        minutes_since_last = clamp_01(((requested_at - last_ts) / 60.0) / 1440.0)
        km_from_last_tx = clamp_01(float(last_tx["km_from_current"]) / 1000.0)

    known = merchant_id in known_merchants
    merchant_known = 1.0 if known else 0.0
    unknown_merchant = 0.0 if merchant_known else 1.0
    mcc_risk = TX2_MCC_RISK.get(merchant_mcc, 0.5)

    safe_customer_avg = customer_avg if customer_avg >= 1e-9 else 1e-9
    amount_vs_avg = clamp_01(amount / safe_customer_avg / 10.0)

    vec = [0.0] * DIMS
    vec[0] = clamp_01(amount / 10000.0)
    vec[1] = clamp_01(float(installments) / 12.0)
    vec[2] = amount_vs_avg
    vec[3] = float(hour_of_day_from_epoch(requested_at)) / 23.0
    vec[4] = float(day_of_week_from_epoch(requested_at)) / 6.0
    vec[5] = minutes_since_last
    vec[6] = km_from_last_tx
    vec[7] = clamp_01(terminal_km_from_home / 1000.0)
    vec[8] = clamp_01(float(customer_tx_count_24h) / 20.0)
    vec[9] = terminal_is_online
    vec[10] = terminal_card_present
    vec[11] = unknown_merchant
    vec[12] = mcc_risk
    vec[13] = clamp_01(merchant_avg_amount / 10000.0)
    return vec


def load_test_data_xy(path: Path) -> tuple[np.ndarray, np.ndarray]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    entries = payload["entries"]
    n = len(entries)
    x = np.empty((n, DIMS), dtype=np.float32)
    y = np.empty((n,), dtype=np.int32)
    for i, entry in enumerate(entries):
        x[i, :] = request_to_vector(entry["request"])
        # fraud = 1 when expected_approved is false
        y[i] = 0 if entry["expected_approved"] else 1
    return x, y


def stratified_split_indices(
    y: np.ndarray, valid_ratio: float, seed: int
) -> tuple[np.ndarray, np.ndarray]:
    rng = np.random.default_rng(seed)
    pos = np.where(y == 1)[0]
    neg = np.where(y == 0)[0]
    rng.shuffle(pos)
    rng.shuffle(neg)

    pos_cut = int((1.0 - valid_ratio) * len(pos))
    neg_cut = int((1.0 - valid_ratio) * len(neg))
    train_idx = np.concatenate([pos[:pos_cut], neg[:neg_cut]])
    valid_idx = np.concatenate([pos[pos_cut:], neg[neg_cut:]])
    rng.shuffle(train_idx)
    rng.shuffle(valid_idx)
    return train_idx, valid_idx


def score_weighted_error(y_true: np.ndarray, y_pred_fraud: np.ndarray) -> tuple[float, int, int, int, int]:
    fraud_true = y_true == 1
    fraud_pred = y_pred_fraud == 1
    fp = int(np.sum((~fraud_true) & fraud_pred))
    fn = int(np.sum(fraud_true & (~fraud_pred)))
    tp = int(np.sum(fraud_true & fraud_pred))
    tn = int(np.sum((~fraud_true) & (~fraud_pred)))
    weighted = FP_WEIGHT * fp + FN_WEIGHT * fn
    return weighted, fp, fn, tp, tn


def find_best_threshold(y_valid: np.ndarray, prob: np.ndarray) -> tuple[float, dict]:
    best_t = 0.5
    best_metrics = None

    def consider(t: float) -> None:
        nonlocal best_t, best_metrics
        pred = (prob >= t).astype(np.int32)
        weighted, fp, fn, tp, tn = score_weighted_error(y_valid, pred)
        failures = fp + fn
        cur = {
            "weighted": weighted,
            "fp": fp,
            "fn": fn,
            "tp": tp,
            "tn": tn,
            "failures": failures,
        }
        if best_metrics is None:
            best_t = t
            best_metrics = cur
            return

        # Primary: weighted error (FN has higher cost).
        # Secondary: fewer failures, then more TP.
        if (
            cur["weighted"] < best_metrics["weighted"]
            or (
                cur["weighted"] == best_metrics["weighted"]
                and cur["failures"] < best_metrics["failures"]
            )
            or (
                cur["weighted"] == best_metrics["weighted"]
                and cur["failures"] == best_metrics["failures"]
                and cur["tp"] > best_metrics["tp"]
            )
        ):
            best_t = t
            best_metrics = cur

    for t in np.arange(0.05, 0.951, 0.01):
        consider(float(t))

    lo = max(0.001, best_t - 0.03)
    hi = min(0.999, best_t + 0.03)
    for t in np.arange(lo, hi + 0.0001, 0.001):
        consider(float(t))

    assert best_metrics is not None
    return best_t, best_metrics


def parse_base_margin(booster: xgb.Booster) -> float:
    config = json.loads(booster.save_config())
    raw = config["learner"]["learner_model_param"]["base_score"]
    try:
        base_score = float(raw)
    except ValueError:
        base_score = float(json.loads(raw)[0])
    if base_score <= 0.0:
        base_score = 1e-6
    if base_score >= 1.0:
        base_score = 1.0 - 1e-6
    return math.log(base_score / (1.0 - base_score))


def flatten_tree(tree_obj: dict) -> FlatTree:
    nodes_by_id: dict[int, dict] = {}
    stack = [tree_obj]
    while stack:
        node = stack.pop()
        nid = int(node["nodeid"])
        if nid in nodes_by_id:
            continue
        nodes_by_id[nid] = node
        for child in node.get("children", []):
            stack.append(child)

    ordered_ids = sorted(nodes_by_id.keys())
    id_to_index = {nid: i for i, nid in enumerate(ordered_ids)}

    feature: list[int] = []
    left: list[int] = []
    right: list[int] = []
    threshold: list[float] = []
    leaf: list[float] = []

    for nid in ordered_ids:
        node = nodes_by_id[nid]
        if "leaf" in node:
            feature.append(-1)
            left.append(-1)
            right.append(-1)
            threshold.append(0.0)
            leaf.append(float(node["leaf"]))
            continue

        split = node["split"]
        if isinstance(split, str) and split.startswith("f"):
            split_idx = int(split[1:])
        else:
            split_idx = int(split)

        yes_id = int(node["yes"])
        no_id = int(node["no"])
        feature.append(split_idx)
        left.append(id_to_index[yes_id])
        right.append(id_to_index[no_id])
        threshold.append(float(node["split_condition"]))
        leaf.append(0.0)

    return FlatTree(feature=feature, left=left, right=right, threshold=threshold, leaf=leaf)


def format_float(v: float) -> str:
    s = f"{v:.9g}"
    if "e" not in s and "." not in s:
        s += ".0"
    return s + "f"


def emit_header(path: Path) -> None:
    path.write_text(
        "\n".join(
            [
                "#ifndef XGBOOST_MODEL_H",
                "#define XGBOOST_MODEL_H",
                "",
                "#include <stdint.h>",
                "",
                "#define XGBOOST_MODEL_DIMS 14",
                "",
                "float xgboost_predict_probability(const double features[XGBOOST_MODEL_DIMS]);",
                "uint8_t xgboost_predict_fraud_count(const double features[XGBOOST_MODEL_DIMS]);",
                "",
                "#endif",
                "",
            ]
        ),
        encoding="utf-8",
    )


def emit_source(path: Path, trees: list[FlatTree], base_margin: float, threshold: float) -> None:
    tree_offsets = [0]
    total_nodes = 0
    for t in trees:
        total_nodes += t.node_count
        tree_offsets.append(total_nodes)

    feature_all: list[int] = []
    left_all: list[int] = []
    right_all: list[int] = []
    threshold_all: list[float] = []
    leaf_all: list[float] = []

    for t, start in zip(trees, tree_offsets):
        for i in range(t.node_count):
            feature_all.append(t.feature[i])
            if t.feature[i] < 0:
                left_all.append(-1)
                right_all.append(-1)
            else:
                left_all.append(start + t.left[i])
                right_all.append(start + t.right[i])
            threshold_all.append(t.threshold[i])
            leaf_all.append(t.leaf[i])

    lines: list[str] = []
    lines.append('#include "xgboost_model.h"')
    lines.append("")
    lines.append("#include <math.h>")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("  int16_t feature;")
    lines.append("  int32_t left;")
    lines.append("  int32_t right;")
    lines.append("  float threshold;")
    lines.append("  float leaf;")
    lines.append("} XGBNode;")
    lines.append("")
    lines.append(f"static const float kBaseMargin = {format_float(base_margin)};")
    lines.append(f"static const float kFraudThreshold = {format_float(threshold)};")
    lines.append(f"static const uint32_t kTreeCount = {len(trees)}u;")
    lines.append(f"static const uint32_t kNodeCount = {len(feature_all)}u;")
    lines.append("")
    lines.append(f"static const uint32_t kTreeOffsets[{len(tree_offsets)}] = {{")
    for i, off in enumerate(tree_offsets):
        suffix = "," if i + 1 < len(tree_offsets) else ""
        lines.append(f"  {off}u{suffix}")
    lines.append("};")
    lines.append("")
    lines.append(f"static const XGBNode kNodes[{len(feature_all)}] = {{")
    for i in range(len(feature_all)):
        suffix = "," if i + 1 < len(feature_all) else ""
        lines.append(
            "  {" +
            f"{feature_all[i]}, {left_all[i]}, {right_all[i]}, "
            f"{format_float(threshold_all[i])}, {format_float(leaf_all[i])}" +
            "}" + suffix
        )
    lines.append("};")
    lines.append("")
    lines.append("static inline float sigmoidf_fast(float x) {")
    lines.append("  if (x >= 35.0f) return 1.0f;")
    lines.append("  if (x <= -35.0f) return 0.0f;")
    lines.append("  return 1.0f / (1.0f + expf(-x));")
    lines.append("}")
    lines.append("")
    lines.append("float xgboost_predict_probability(const double features[XGBOOST_MODEL_DIMS]) {")
    lines.append("  float margin = kBaseMargin;")
    lines.append("  (void)kNodeCount;")
    lines.append("  for (uint32_t t = 0; t < kTreeCount; t++) {")
    lines.append("    int32_t idx = (int32_t)kTreeOffsets[t];")
    lines.append("    while (idx >= 0) {")
    lines.append("      const XGBNode node = kNodes[idx];")
    lines.append("      if (node.feature < 0) {")
    lines.append("        margin += node.leaf;")
    lines.append("        break;")
    lines.append("      }")
    lines.append("      const float fv = (float)features[node.feature];")
    lines.append("      idx = (fv < node.threshold) ? node.left : node.right;")
    lines.append("    }")
    lines.append("  }")
    lines.append("  return sigmoidf_fast(margin);")
    lines.append("}")
    lines.append("")
    lines.append("uint8_t xgboost_predict_fraud_count(const double features[XGBOOST_MODEL_DIMS]) {")
    lines.append("  const float prob = xgboost_predict_probability(features);")
    lines.append("  return (prob >= kFraudThreshold) ? 5 : 0;")
    lines.append("}")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Train XGBoost and emit C inference model")
    parser.add_argument(
        "--dataset-mode",
        choices=("references", "test-data"),
        default="references",
        help="training dataset source",
    )
    parser.add_argument("--input", default=None, help="input path (mode-dependent)")
    parser.add_argument("--output-h", default=str(OUT_H), help="output header file path")
    parser.add_argument("--output-c", default=str(OUT_C), help="output source file path")
    parser.add_argument("--sample-size", type=int, default=600_000, help="max training samples")
    parser.add_argument("--valid-ratio", type=float, default=0.1, help="validation ratio")
    parser.add_argument("--seed", type=int, default=42, help="random seed")
    parser.add_argument("--n-estimators", type=int, default=120, help="number of trees")
    parser.add_argument("--max-depth", type=int, default=6, help="tree depth")
    parser.add_argument("--learning-rate", type=float, default=0.08, help="eta")
    parser.add_argument("--subsample", type=float, default=0.8, help="subsample")
    parser.add_argument("--colsample", type=float, default=0.8, help="colsample_bytree")
    parser.add_argument("--n-jobs", type=int, default=6, help="training threads")
    args = parser.parse_args()

    if args.input:
        input_path = Path(args.input)
    elif args.dataset_mode == "test-data":
        input_path = TEST_DATA_INPUT_PATH
    else:
        input_path = INPUT_PATH
    out_h = Path(args.output_h)
    out_c = Path(args.output_c)

    if args.dataset_mode == "test-data":
        x, y = load_test_data_xy(input_path)
        train_idx = np.arange(x.shape[0], dtype=np.int32)
        valid_idx = np.arange(x.shape[0], dtype=np.int32)
    else:
        rows = load_rows(input_path)
        if args.sample_size > 0 and args.sample_size < len(rows):
            rng = random.Random(args.seed)
            rows = rng.sample(rows, args.sample_size)
        x, y = to_xy(rows)
        train_idx, valid_idx = stratified_split_indices(y, args.valid_ratio, args.seed)

    x_train = x[train_idx]
    y_train = y[train_idx]
    x_valid = x[valid_idx]
    y_valid = y[valid_idx]

    model = xgb.XGBClassifier(
        n_estimators=args.n_estimators,
        max_depth=args.max_depth,
        learning_rate=args.learning_rate,
        subsample=args.subsample,
        colsample_bytree=args.colsample,
        objective="binary:logistic",
        eval_metric="logloss",
        tree_method="hist",
        n_jobs=args.n_jobs,
        random_state=args.seed,
    )
    if args.dataset_mode == "test-data":
        model.fit(x_train, y_train, verbose=False)
    else:
        model.fit(
            x_train,
            y_train,
            eval_set=[(x_valid, y_valid)],
            verbose=False,
        )

    prob_valid = model.predict_proba(x_valid)[:, 1]
    threshold, metrics = find_best_threshold(y_valid, prob_valid)

    booster = model.get_booster()
    base_margin = parse_base_margin(booster)
    dump = booster.get_dump(dump_format="json")
    trees = [flatten_tree(json.loads(tree_json)) for tree_json in dump]

    emit_header(out_h)
    emit_source(out_c, trees, base_margin, threshold)

    print(
        f"dataset_mode: {args.dataset_mode} "
        f"input={input_path} samples={x.shape[0]} "
        f"(train={len(train_idx)}, valid={len(valid_idx)})"
    )
    print(f"trees: {len(trees)}")
    print(
        "validation: "
        f"fp={metrics['fp']} fn={metrics['fn']} tp={metrics['tp']} tn={metrics['tn']} "
        f"weighted={metrics['weighted']:.0f}"
    )
    print(f"threshold: {threshold:.6f}")
    print(f"wrote: {out_h}")
    print(f"wrote: {out_c}")


if __name__ == "__main__":
    main()
