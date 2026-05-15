#!/usr/bin/env python3
"""
Build `resources/references.idx` in the binary layout expected by x-score.c.

Input:
- resources/references.json.gz

Output:
- resources/references.idx

Current strategy:
- Uses fixed 3D bucketing clusters (k=128):
  - amount_bin: 8 buckets from vector[0]
  - hour_bin: 4 buckets from vector[3]
  - is_online_bin: 2 buckets from vector[9]
  - card_present_bin: 2 buckets from vector[10]
  - cluster_id = (((amount_bin * 4) + hour_bin) * 2 + is_online_bin) * 2 + card_present_bin
- Reorders vectors by cluster to enable fast contiguous range scans.
- Labels are bit-packed (fraud=1, legit=0), lsb-first.
- Vectors are quantized int16 in range [-10000, 10000], from float range [-1, 1].
"""

from __future__ import annotations

import gzip
import json
import struct
from pathlib import Path


INPUT_PATH = Path("resources/references.json.gz")
OUTPUT_IDX_PATH = Path("resources/references.idx")

DIMS = 14
MAGIC = 0x3145564F43535852  # must match X_SCORE_MAGIC
AMOUNT_BUCKETS = 8
HOUR_BUCKETS = 4
ONLINE_BUCKETS = 2
CARD_BUCKETS = 2
CENTROIDS_COUNT = AMOUNT_BUCKETS * HOUR_BUCKETS * ONLINE_BUCKETS * CARD_BUCKETS


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def qround(v: float) -> int:
    if v < -1.0:
        v = -1.0
    if v > 1.0:
        v = 1.0
    return int(round(v * 10000.0))


def main() -> None:
    if not INPUT_PATH.exists():
        raise FileNotFoundError(f"input not found: {INPUT_PATH}")

    with gzip.open(INPUT_PATH, "rt", encoding="utf-8") as f:
        rows = json.load(f)

    count = len(rows)
    if count == 0:
        raise ValueError("empty dataset")

    clusters: list[list[tuple[list[int], int, list[float]]]] = [[] for _ in range(CENTROIDS_COUNT)]

    for i, row in enumerate(rows):
        vector = row.get("vector")
        label = row.get("label")

        if not isinstance(vector, list) or len(vector) != DIMS:
            raise ValueError(f"invalid vector at index {i}")

        fvec = [float(x) for x in vector]
        qvec = [qround(x) for x in fvec]
        lbl = 1 if label == "fraud" else 0

        amount = fvec[0]
        hour = fvec[3]
        amount_bin = int(amount * AMOUNT_BUCKETS)
        hour_bin = int(hour * HOUR_BUCKETS)
        is_online_bin = 1 if int(fvec[9]) != 0 else 0
        card_present_bin = 1 if int(fvec[10]) != 0 else 0
        if amount_bin < 0:
            amount_bin = 0
        if amount_bin >= AMOUNT_BUCKETS:
            amount_bin = AMOUNT_BUCKETS - 1
        if hour_bin < 0:
            hour_bin = 0
        if hour_bin >= HOUR_BUCKETS:
            hour_bin = HOUR_BUCKETS - 1
        cluster_id = (
            ((amount_bin * HOUR_BUCKETS) + hour_bin) * ONLINE_BUCKETS + is_online_bin
        ) * CARD_BUCKETS + card_present_bin
        clusters[cluster_id].append((qvec, lbl, fvec))

    centroids_q16: list[int] = []
    cluster_counts: list[int] = []
    cluster_offsets: list[int] = [0]
    vectors_q16: list[int] = []
    labels_in_order: list[int] = []

    for c in range(CENTROIDS_COUNT):
        items = clusters[c]
        ccount = len(items)
        cluster_counts.append(ccount)
        cluster_offsets.append(cluster_offsets[-1] + ccount)

        if ccount == 0:
            centroids_q16.extend([0] * DIMS)
        else:
            sums = [0.0] * DIMS
            for _, _, fvec in items:
                for d in range(DIMS):
                    sums[d] += fvec[d]
            centroids_q16.extend([qround(sums[d] / ccount) for d in range(DIMS)])

        for qvec, lbl, _ in items:
            vectors_q16.extend(qvec)
            labels_in_order.append(lbl)

    if len(vectors_q16) != count * DIMS or len(labels_in_order) != count:
        raise RuntimeError("internal error: reordered dataset size mismatch")

    labels_bits = bytearray((count + 7) // 8)
    for i, lbl in enumerate(labels_in_order):
        if lbl == 1:
            labels_bits[i // 8] |= 1 << (i % 8)

    # Header layout in C:
    # uint64 magic;
    # uint32 count;
    # uint32 dims;
    # uint32 centroids_count;
    # uint32 reserved[12];
    header = struct.pack(
        "<QIII12I",
        MAGIC,
        count,
        DIMS,
        CENTROIDS_COUNT,
        *([0] * 12),
    )

    centroids_blob = struct.pack("<" + "h" * (CENTROIDS_COUNT * DIMS), *centroids_q16)
    offsets_blob = struct.pack("<" + "I" * (CENTROIDS_COUNT + 1), *cluster_offsets)
    counts_blob = struct.pack("<" + "I" * CENTROIDS_COUNT, *cluster_counts)
    labels_blob = bytes(labels_bits)
    vectors_blob = struct.pack("<" + "h" * (count * DIMS), *vectors_q16)

    out = bytearray()
    out += header
    out += centroids_blob

    out += b"\x00" * (align_up(len(out), 4) - len(out))
    out += offsets_blob
    out += counts_blob
    out += labels_blob

    out += b"\x00" * (align_up(len(out), 2) - len(out))
    out += vectors_blob

    OUTPUT_IDX_PATH.write_bytes(out)

    print(f"done: {count} vectors")
    print(f"wrote: {OUTPUT_IDX_PATH}")
    print(f"size: {len(out)} bytes")


if __name__ == "__main__":
    main()
