#!/usr/bin/env python3
"""
Simple cutoff suggester for partition key.

It reads `resources/references.idx` and suggests:
- cutoff for vector[2] (amount_vs_avg_high)
- cutoff for vector[8] (tx_count_24h_high)
- optional mcc bucket boundaries for vector[12]

The two main cutoffs are chosen by best binary split (lowest weighted Gini)
against the reference labels (fraud vs legit).
"""

from __future__ import annotations

import argparse
import json
import math
import mmap
import struct
from pathlib import Path


MAGIC = b"RNSPCST1"
SCALE = 10000
DIMS = 14
LANES = 8
HIST_SIZE = SCALE + 1


def gini_binary(pos: int, neg: int) -> float:
    total = pos + neg
    if total <= 0:
        return 0.0
    p = pos / total
    n = neg / total
    return 1.0 - (p * p + n * n)


def best_threshold_by_gini(fraud_hist: list[int], legit_hist: list[int]) -> tuple[int, float]:
    total_fraud = sum(fraud_hist)
    total_legit = sum(legit_hist)
    total = total_fraud + total_legit
    if total <= 1:
        return 0, 1.0

    left_fraud = 0
    left_legit = 0
    best_t = 0
    best_score = float("inf")

    # threshold semantics: "value > t"
    # so left is <= t, right is > t
    for t in range(HIST_SIZE - 1):
        left_fraud += fraud_hist[t]
        left_legit += legit_hist[t]

        right_fraud = total_fraud - left_fraud
        right_legit = total_legit - left_legit
        left_n = left_fraud + left_legit
        right_n = right_fraud + right_legit

        if left_n == 0 or right_n == 0:
            continue

        score = (left_n / total) * gini_binary(left_fraud, left_legit) + (right_n / total) * gini_binary(
            right_fraud, right_legit
        )

        if score < best_score:
            best_score = score
            best_t = t

    if best_score == float("inf"):
        return 0, 1.0
    return best_t, best_score


def percentile_from_hist(hist: list[int], p: float) -> int:
    total = sum(hist)
    if total == 0:
        return 0
    target = int(math.ceil(total * p))
    acc = 0
    for v, c in enumerate(hist):
        acc += c
        if acc >= target:
            return v
    return SCALE


def next_nonzero(hist: list[int], start: int) -> int:
    for i in range(max(0, start), len(hist)):
        if hist[i] > 0:
            return i
    return SCALE


def clamp_q16(v: int) -> int:
    if v < 0:
        return 0
    if v > SCALE:
        return SCALE
    return v


def analyze_index(idx_path: Path) -> dict:
    with idx_path.open("rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            if len(mm) < 32:
                raise ValueError("index file too short")

            magic, scale, dims, count, partition_count, node_count, block_count = struct.unpack_from(
                "<8siiiiii", mm, 0
            )
            if magic != MAGIC:
                raise ValueError(f"unexpected magic: {magic!r}")
            if scale != SCALE:
                raise ValueError(f"unexpected scale: {scale}")
            if dims != DIMS:
                raise ValueError(f"unexpected dims: {dims}")
            if count <= 0 or block_count <= 0:
                raise ValueError("empty index")

            vectors_offset = 32 + partition_count * 72 + node_count * 72
            vectors_bytes = block_count * DIMS * LANES * 2
            labels_offset = vectors_offset + vectors_bytes
            labels_bytes = block_count * LANES

            if labels_offset + labels_bytes > len(mm):
                raise ValueError("truncated index sections")

            fraud2 = [0] * HIST_SIZE
            legit2 = [0] * HIST_SIZE
            fraud8 = [0] * HIST_SIZE
            legit8 = [0] * HIST_SIZE
            hist12 = [0] * HIST_SIZE

            idx = 0
            block_stride = DIMS * LANES * 2
            for b in range(block_count):
                base = vectors_offset + b * block_stride
                v2 = struct.unpack_from("<8h", mm, base + 2 * LANES * 2)
                v8 = struct.unpack_from("<8h", mm, base + 8 * LANES * 2)
                v12 = struct.unpack_from("<8h", mm, base + 12 * LANES * 2)
                labels = struct.unpack_from("<8B", mm, labels_offset + b * LANES)

                for lane in range(LANES):
                    if idx >= count:
                        break
                    q2 = clamp_q16(v2[lane])
                    q8 = clamp_q16(v8[lane])
                    q12 = clamp_q16(v12[lane])
                    lbl = labels[lane]

                    if lbl == 1:
                        fraud2[q2] += 1
                        fraud8[q8] += 1
                    else:
                        legit2[q2] += 1
                        legit8[q8] += 1
                    hist12[q12] += 1

                    idx += 1

            if idx != count:
                raise ValueError(f"decoded {idx} refs, expected {count}")

            c2, g2 = best_threshold_by_gini(fraud2, legit2)
            c8, g8 = best_threshold_by_gini(fraud8, legit8)

            c1 = percentile_from_hist(hist12, 0.25)
            c2_m = percentile_from_hist(hist12, 0.50)
            c3 = percentile_from_hist(hist12, 0.75)
            if c2_m <= c1:
                c2_m = next_nonzero(hist12, c1 + 1)
            if c3 <= c2_m:
                c3 = next_nonzero(hist12, c2_m + 1)

            total2 = sum(fraud2) + sum(legit2)
            total8 = sum(fraud8) + sum(legit8)
            above2 = sum(fraud2[c2 + 1 :]) + sum(legit2[c2 + 1 :])
            above8 = sum(fraud8[c8 + 1 :]) + sum(legit8[c8 + 1 :])

            return {
                "count": count,
                "partition_count": partition_count,
                "node_count": node_count,
                "block_count": block_count,
                "cutoff_vector2_q16": c2,
                "cutoff_vector8_q16": c8,
                "cutoff_vector2_float": c2 / SCALE,
                "cutoff_vector8_float": c8 / SCALE,
                "gini_vector2": g2,
                "gini_vector8": g8,
                "share_vector2_above_cutoff": (above2 / total2) if total2 else 0.0,
                "share_vector8_above_cutoff": (above8 / total8) if total8 else 0.0,
                "mcc_cutoffs_q16": [c1, c2_m, c3],
                "mcc_cutoffs_float": [c1 / SCALE, c2_m / SCALE, c3 / SCALE],
            }
        finally:
            mm.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="Suggest simple cutoff values for index partition key")
    parser.add_argument("--index", default="resources/references.idx", help="path to references.idx")
    parser.add_argument("--json", default="", help="optional output json file path")
    args = parser.parse_args()

    result = analyze_index(Path(args.index))

    print(f"references: {result['count']}")
    print("")
    print("Suggested cutoffs:")
    print(
        f"- vector[2] cutoff: {result['cutoff_vector2_q16']} (float ~ {result['cutoff_vector2_float']:.4f}), "
        f"above share ~ {result['share_vector2_above_cutoff']:.2%}"
    )
    print(
        f"- vector[8] cutoff: {result['cutoff_vector8_q16']} (float ~ {result['cutoff_vector8_float']:.4f}), "
        f"above share ~ {result['share_vector8_above_cutoff']:.2%}"
    )
    print(
        f"- vector[12] (mcc) buckets: {result['mcc_cutoffs_q16'][0]}, {result['mcc_cutoffs_q16'][1]}, {result['mcc_cutoffs_q16'][2]}"
        f" (float ~ {result['mcc_cutoffs_float'][0]:.4f}, {result['mcc_cutoffs_float'][1]:.4f}, {result['mcc_cutoffs_float'][2]:.4f})"
    )
    print("")
    print("Use in build_binary_references.py:")
    print(f"if qvec[2] > {result['cutoff_vector2_q16']}:")
    print("    key |= 1 << 6")
    print(f"if qvec[8] > {result['cutoff_vector8_q16']}:")
    print("    key |= 1 << 7")
    print(
        f"# mcc buckets: <= {result['mcc_cutoffs_q16'][0]}, <= {result['mcc_cutoffs_q16'][1]}, <= {result['mcc_cutoffs_q16'][2]}, else"
    )

    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(f"\nSaved JSON: {args.json}")


if __name__ == "__main__":
    main()
