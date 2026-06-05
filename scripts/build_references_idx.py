#!/usr/bin/env python3
"""
Build `resources/refs.bin` and `resources/kdtree.bin` for the KD-tree search.
"""

from __future__ import annotations

import gzip
import json
import struct
from dataclasses import dataclass, field
from pathlib import Path

INPUT_PATH = Path("resources/references.json.gz")
REFS_OUTPUT_PATH = Path("resources/refs.bin")
TREE_OUTPUT_PATH = Path("resources/kdtree.bin")

DIMS = 14
LANES = 8
LEAF_SIZE = 80
LEAF_FLAG = 0xFFFFFFFF


@dataclass
class Ref:
    qvec: list[int]
    label: int


@dataclass
class Node:
    minv: list[int]
    maxv: list[int]
    indices: list[int] | None = None
    left: "Node | None" = None
    right: "Node | None" = None


def quantize(value: float) -> int:
    if value <= -1.0:
        return -10000
    if value <= 0.0:
        return 0
    if value >= 1.0:
        return 10000
    return int(round(value * 10000.0))


def partition_key(qvec: list[int]) -> int:
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


def compute_bounds(refs: list[Ref], indices: list[int]) -> tuple[list[int], list[int]]:
    minv = [32767] * DIMS
    maxv = [-32768] * DIMS
    for idx in indices:
        qvec = refs[idx].qvec
        for d, value in enumerate(qvec):
            if value < minv[d]:
                minv[d] = value
            if value > maxv[d]:
                maxv[d] = value
    return minv, maxv


def build_tree(refs: list[Ref], indices: list[int]) -> Node:
    minv, maxv = compute_bounds(refs, indices)
    if len(indices) <= LEAF_SIZE:
        return Node(minv=minv, maxv=maxv, indices=indices.copy())

    split_dim = max(range(DIMS), key=lambda d: maxv[d] - minv[d])
    ordered = sorted(indices, key=lambda idx: refs[idx].qvec[split_dim])
    mid = len(ordered) // 2
    if mid <= 0 or mid >= len(ordered):
        return Node(minv=minv, maxv=maxv, indices=ordered)

    left = build_tree(refs, ordered[:mid])
    right = build_tree(refs, ordered[mid:])
    return Node(minv=minv, maxv=maxv, left=left, right=right)


def emit_tree(node: Node, node_records: list[bytes], members: list[int]) -> int:
    if node.indices is not None:
        member_start = len(members)
        members.extend(node.indices)
        record = (
            struct.pack("<" + "h" * DIMS, *node.minv)
            + struct.pack("<" + "h" * DIMS, *node.maxv)
            + struct.pack("<III", LEAF_FLAG, member_start, len(node.indices))
        )
        node_records.append(record)
        return len(node_records) - 1

    assert node.left is not None
    assert node.right is not None
    left_idx = emit_tree(node.left, node_records, members)
    right_idx = emit_tree(node.right, node_records, members)
    record = (
        struct.pack("<" + "h" * DIMS, *node.minv)
        + struct.pack("<" + "h" * DIMS, *node.maxv)
        + struct.pack("<III", left_idx, right_idx, subtree_len(node))
    )
    node_records.append(record)
    return len(node_records) - 1


def subtree_len(node: Node) -> int:
    if node.indices is not None:
        return len(node.indices)
    assert node.left is not None
    assert node.right is not None
    return subtree_len(node.left) + subtree_len(node.right)


def main() -> None:
    if not INPUT_PATH.exists():
        raise FileNotFoundError(f"input not found: {INPUT_PATH}")

    with gzip.open(INPUT_PATH, "rt", encoding="utf-8") as f:
        rows = json.load(f)

    refs: list[Ref] = []
    partitions: dict[int, list[int]] = {}

    for i, row in enumerate(rows):
        vector = row.get("vector")
        label = row.get("label")
        if not isinstance(vector, list) or len(vector) != DIMS:
            raise ValueError(f"invalid vector at index {i}")

        qvec = [quantize(float(x)) for x in vector]
        lbl = 1 if label == "fraud" else 0
        refs.append(Ref(qvec=qvec, label=lbl))
        key = partition_key(qvec)
        partitions.setdefault(key, []).append(i)

    refs_blob = bytearray()
    refs_blob.extend(b"RINH")
    refs_blob.extend(struct.pack("<II", 3, len(refs)))
    for ref in refs:
        refs_blob.extend(struct.pack("<" + "h" * DIMS, *ref.qvec))
        refs_blob.append(ref.label)

    partition_entries: list[bytes] = []
    node_records: list[bytes] = []
    members: list[int] = []

    for key in sorted(partitions.keys()):
        indices = partitions[key]
        tree = build_tree(refs, indices)
        root_idx = emit_tree(tree, node_records, members)
        part_record = (
            struct.pack("<III", key, root_idx, len(indices))
            + struct.pack("<" + "h" * DIMS, *tree.minv)
            + struct.pack("<" + "h" * DIMS, *tree.maxv)
        )
        partition_entries.append(part_record)

    tree_blob = bytearray()
    tree_blob.extend(b"RKDT")
    tree_blob.extend(struct.pack("<IIIII", 2, len(refs), DIMS, len(node_records), 0))
    tree_blob.extend(struct.pack("<I", len(partition_entries)))
    for part in partition_entries:
        tree_blob.extend(part)
    for record in node_records:
        tree_blob.extend(record)
    for ref_idx in members:
        tree_blob.extend(struct.pack("<I", ref_idx))

    REFS_OUTPUT_PATH.write_bytes(refs_blob)
    TREE_OUTPUT_PATH.write_bytes(tree_blob)

    print(f"done: {len(refs)} vectors")
    print(f"partitions: {len(partition_entries)}")
    print(f"nodes: {len(node_records)}")
    print(f"members: {len(members)}")
    print(f"wrote: {REFS_OUTPUT_PATH}")
    print(f"wrote: {TREE_OUTPUT_PATH}")


if __name__ == "__main__":
    main()
