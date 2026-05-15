#!/usr/bin/env python3
"""
Build `resources/references.idx` in a specialist exact-kNN format.

Input:
- resources/references.json.gz

Output:
- resources/references.idx

Layout (little-endian):
- header: magic(8) + scale(i32) + dims(i32) + ref_count(i32) +
          partition_count(i32) + node_count(i32) + block_count(i32)
- partition directory (72 bytes each)
- node directory (72 bytes each)
- vectors in AoSoA blocks (LANES=8): for each block -> dims -> lanes -> i16
- labels in block order: for each block -> lanes -> u8
"""

from __future__ import annotations

import gzip
import json
import struct
from array import array
from dataclasses import dataclass, field
from pathlib import Path

INPUT_PATH = Path("resources/references.json.gz")
OUTPUT_IDX_PATH = Path("resources/references.idx")

DIMS = 14
LANES = 8
SCALE = 10000
MAGIC = b"RNSPCST1"


@dataclass
class PartitionData:
    key: int
    count: int = 0
    minv: list[int] = field(default_factory=lambda: [32767] * DIMS)
    maxv: list[int] = field(default_factory=lambda: [-32768] * DIMS)
    vectors: array = field(default_factory=lambda: array("h"))
    labels: bytearray = field(default_factory=bytearray)


def quantize(value: float) -> int:
    if value <= -1.0:
        return -SCALE
    if value <= 0.0:
        return 0
    if value >= 1.0:
        return SCALE
    return int(round(value * SCALE))


def compute_partition_key(qvec: list[int]) -> int:
    key = 0

    if qvec[5] >= 0:
        key |= 1 << 0
    if qvec[9] > 0:
        key |= 1 << 1
    if qvec[10] > 0:
        key |= 1 << 2
    if qvec[11] > 0:
        key |= 1 << 3

    if qvec[12] <= 2000:
        mcc_bucket = 0
    elif qvec[12] <= 3000:
        mcc_bucket = 1
    elif qvec[12] <= 7500:
        mcc_bucket = 2
    else:
        mcc_bucket = 3
    key |= mcc_bucket << 4

    if qvec[2] > 1013:
        key |= 1 << 6
    if qvec[8] > 2500:
        key |= 1 << 7

    return key


def main() -> None:
    if not INPUT_PATH.exists():
        raise FileNotFoundError(f"input not found: {INPUT_PATH}")

    with gzip.open(INPUT_PATH, "rt", encoding="utf-8") as f:
        rows = json.load(f)

    if not isinstance(rows, list) or len(rows) == 0:
        raise ValueError("invalid or empty dataset")

    partitions: dict[int, PartitionData] = {}
    total_count = 0

    for i, row in enumerate(rows):
        vector = row.get("vector")
        label = row.get("label")
        if not isinstance(vector, list) or len(vector) != DIMS:
            raise ValueError(f"invalid vector at index {i}")

        qvec = [quantize(float(x)) for x in vector]
        lbl = 1 if label == "fraud" else 0
        key = compute_partition_key(qvec)

        p = partitions.get(key)
        if p is None:
            p = PartitionData(key=key)
            partitions[key] = p

        p.count += 1
        p.labels.append(lbl)
        p.vectors.extend(qvec)
        for d in range(DIMS):
            v = qvec[d]
            if v < p.minv[d]:
                p.minv[d] = v
            if v > p.maxv[d]:
                p.maxv[d] = v

        total_count += 1

    sorted_keys = sorted(partitions.keys())
    partition_count = len(sorted_keys)
    node_count = partition_count

    partition_entries: list[tuple[int, int, int, list[int], list[int]]] = []
    node_entries: list[tuple[int, int, int, int, list[int], list[int]]] = []
    running_blocks = 0

    for node_idx, key in enumerate(sorted_keys):
        p = partitions[key]
        blocks = (p.count + LANES - 1) // LANES
        start_block = running_blocks
        running_blocks += blocks

        partition_entries.append((key, node_idx, p.count, p.minv, p.maxv))
        node_entries.append((-1, -1, start_block, p.count, p.minv, p.maxv))

    total_blocks = running_blocks

    vectors_blob = array("h")
    labels_blob = bytearray()

    for key in sorted_keys:
        p = partitions[key]
        count = p.count
        blocks = (count + LANES - 1) // LANES
        data = p.vectors
        labels = p.labels

        for b in range(blocks):
            base_idx = b * LANES
            for d in range(DIMS):
                for lane in range(LANES):
                    i = base_idx + lane
                    if i < count:
                        vectors_blob.append(data[i * DIMS + d])
                    else:
                        vectors_blob.append(0)
            for lane in range(LANES):
                i = base_idx + lane
                if i < count:
                    labels_blob.append(labels[i])
                else:
                    labels_blob.append(0)

        # release partition payload after emitting
        p.vectors = array("h")
        p.labels = bytearray()

    if len(vectors_blob) != total_blocks * DIMS * LANES:
        raise RuntimeError("invalid vectors blob size")
    if len(labels_blob) != total_blocks * LANES:
        raise RuntimeError("invalid labels blob size")

    out = bytearray()
    out.extend(
        struct.pack(
            "<8siiiiii",
            MAGIC,
            SCALE,
            DIMS,
            total_count,
            partition_count,
            node_count,
            total_blocks,
        )
    )

    for key, root, length, minv, maxv in partition_entries:
        out.extend(struct.pack("<Iiii", key, root, 0, length))
        out.extend(struct.pack("<" + "h" * DIMS, *minv))
        out.extend(struct.pack("<" + "h" * DIMS, *maxv))

    for left, right, start_block, length, minv, maxv in node_entries:
        out.extend(struct.pack("<iiii", left, right, start_block, length))
        out.extend(struct.pack("<" + "h" * DIMS, *minv))
        out.extend(struct.pack("<" + "h" * DIMS, *maxv))

    out.extend(vectors_blob.tobytes())
    out.extend(labels_blob)

    OUTPUT_IDX_PATH.write_bytes(out)

    print(f"done: {total_count} vectors")
    print(f"partitions: {partition_count}")
    print(f"nodes: {node_count}")
    print(f"blocks: {total_blocks}")
    print(f"wrote: {OUTPUT_IDX_PATH}")
    print(f"size: {len(out)} bytes")


if __name__ == "__main__":
    main()
