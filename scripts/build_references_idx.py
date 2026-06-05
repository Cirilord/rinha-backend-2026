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

# Second-level split bits (inside each primary partition key)
SUBINDEX_SPLITS: tuple[tuple[int, int], ...] = (
    (4, 2500),   # day_of_week
    (4, 5000),   # day_of_week
    (4, 7500),   # day_of_week
    (0, 2500),   # amount
    (8, 2500),   # tx_count_24h
    (13, 2000),  # merchant_avg_amount
)


@dataclass
class BucketData:
    count: int = 0
    minv: list[int] = field(default_factory=lambda: [32767] * DIMS)
    maxv: list[int] = field(default_factory=lambda: [-32768] * DIMS)
    vectors: array = field(default_factory=lambda: array("h"))
    labels: bytearray = field(default_factory=bytearray)


@dataclass
class PartitionData:
    key: int
    count: int = 0
    minv: list[int] = field(default_factory=lambda: [32767] * DIMS)
    maxv: list[int] = field(default_factory=lambda: [-32768] * DIMS)
    buckets: dict[int, BucketData] = field(default_factory=dict)


@dataclass
class TreeNode:
    minv: list[int]
    maxv: list[int]
    length: int
    left: TreeNode | None = None
    right: TreeNode | None = None
    bucket: BucketData | None = None


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


def compute_subindex_key(qvec: list[int]) -> int:
    key = 0
    for bit, (dim, cutoff) in enumerate(SUBINDEX_SPLITS):
        if qvec[dim] > cutoff:
            key |= 1 << bit
    return key


def update_minmax(minv: list[int], maxv: list[int], qvec: list[int]) -> None:
    for d, v in enumerate(qvec):
        if v < minv[d]:
            minv[d] = v
        if v > maxv[d]:
            maxv[d] = v


def merge_bounds(a_min: list[int], a_max: list[int], b_min: list[int], b_max: list[int]) -> tuple[list[int], list[int]]:
    out_min = [0] * DIMS
    out_max = [0] * DIMS
    for d in range(DIMS):
        out_min[d] = a_min[d] if a_min[d] < b_min[d] else b_min[d]
        out_max[d] = a_max[d] if a_max[d] > b_max[d] else b_max[d]
    return out_min, out_max


def build_tree_from_leaves(leaves: list[TreeNode]) -> TreeNode:
    if len(leaves) == 1:
        return leaves[0]

    gmin = [32767] * DIMS
    gmax = [-32768] * DIMS
    for leaf in leaves:
        for d in range(DIMS):
            if leaf.minv[d] < gmin[d]:
                gmin[d] = leaf.minv[d]
            if leaf.maxv[d] > gmax[d]:
                gmax[d] = leaf.maxv[d]

    split_dim = 0
    split_span = gmax[0] - gmin[0]
    for d in range(1, DIMS):
        span = gmax[d] - gmin[d]
        if span > split_span:
            split_span = span
            split_dim = d

    ordered = sorted(leaves, key=lambda n: n.minv[split_dim] + n.maxv[split_dim])
    mid = len(ordered) // 2
    left = build_tree_from_leaves(ordered[:mid])
    right = build_tree_from_leaves(ordered[mid:])

    merged_min, merged_max = merge_bounds(left.minv, left.maxv, right.minv, right.maxv)
    return TreeNode(
        minv=merged_min,
        maxv=merged_max,
        length=left.length + right.length,
        left=left,
        right=right,
        bucket=None,
    )


def emit_bucket(bucket: BucketData, vectors_blob: array, labels_blob: bytearray, running_blocks: int) -> tuple[int, int]:
    start_block = running_blocks
    count = bucket.count
    blocks = (count + LANES - 1) // LANES

    data = bucket.vectors
    labels = bucket.labels

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

    return start_block, blocks


def emit_tree(
    node: TreeNode,
    vectors_blob: array,
    labels_blob: bytearray,
    node_entries: list[tuple[int, int, int, int, list[int], list[int]]],
    running_blocks: int,
) -> tuple[int, int]:
    if node.bucket is not None:
        start_block, blocks = emit_bucket(node.bucket, vectors_blob, labels_blob, running_blocks)
        running_blocks += blocks
        node_idx = len(node_entries)
        node_entries.append((-1, -1, start_block, node.length, node.minv, node.maxv))
        return node_idx, running_blocks

    if node.left is None or node.right is None:
        raise RuntimeError("invalid internal tree node")

    left_idx, running_blocks = emit_tree(node.left, vectors_blob, labels_blob, node_entries, running_blocks)
    right_idx, running_blocks = emit_tree(node.right, vectors_blob, labels_blob, node_entries, running_blocks)

    node_idx = len(node_entries)
    node_entries.append((left_idx, right_idx, -1, node.length, node.minv, node.maxv))
    return node_idx, running_blocks


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
        pkey = compute_partition_key(qvec)

        p = partitions.get(pkey)
        if p is None:
            p = PartitionData(key=pkey)
            partitions[pkey] = p

        p.count += 1
        update_minmax(p.minv, p.maxv, qvec)

        skey = compute_subindex_key(qvec)
        bucket = p.buckets.get(skey)
        if bucket is None:
            bucket = BucketData()
            p.buckets[skey] = bucket

        bucket.count += 1
        bucket.labels.append(lbl)
        bucket.vectors.extend(qvec)
        update_minmax(bucket.minv, bucket.maxv, qvec)

        total_count += 1

    sorted_keys = sorted(partitions.keys())
    partition_count = len(sorted_keys)

    partition_entries: list[tuple[int, int, int, list[int], list[int], int]] = []
    node_entries: list[tuple[int, int, int, int, list[int], list[int]]] = []

    vectors_blob = array("h")
    labels_blob = bytearray()
    running_blocks = 0
    max_leaves_per_partition = 0

    for key in sorted_keys:
        p = partitions[key]

        leaves: list[TreeNode] = []
        for _, bucket in sorted(p.buckets.items(), key=lambda kv: kv[0]):
            if bucket.count <= 0:
                continue
            leaves.append(
                TreeNode(
                    minv=bucket.minv.copy(),
                    maxv=bucket.maxv.copy(),
                    length=bucket.count,
                    left=None,
                    right=None,
                    bucket=bucket,
                )
            )

        if not leaves:
            continue

        if len(leaves) > max_leaves_per_partition:
            max_leaves_per_partition = len(leaves)

        root = build_tree_from_leaves(leaves)
        root_idx, running_blocks = emit_tree(root, vectors_blob, labels_blob, node_entries, running_blocks)
        partition_entries.append((key, root_idx, p.count, p.minv, p.maxv, len(leaves)))

        # release partition payload after emitting
        for bucket in p.buckets.values():
            bucket.vectors = array("h")
            bucket.labels = bytearray()
        p.buckets.clear()

    node_count = len(node_entries)
    total_blocks = running_blocks

    if len(vectors_blob) != total_blocks * DIMS * LANES:
        raise RuntimeError("invalid vectors blob size")
    if len(labels_blob) != total_blocks * LANES:
        raise RuntimeError("invalid labels blob size")
    if partition_count != len(partition_entries):
        raise RuntimeError("partition count mismatch")

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

    for key, root, length, minv, maxv, _leaf_count in partition_entries:
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
    print(f"max leaves per partition: {max_leaves_per_partition}")
    print(f"wrote: {OUTPUT_IDX_PATH}")
    print(f"size: {len(out)} bytes")


if __name__ == "__main__":
    main()
