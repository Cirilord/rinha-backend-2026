#!/usr/bin/env python3
"""
Build `resources/references.idx` in IVF format (centroids + inverted lists).

Input:
- resources/references.json.gz

Output:
- resources/references.idx

Layout (little-endian):
- header: magic(8) + scale(i32) + dims(i32) + ref_count(i32) +
          centroid_count(i32) + block_count(i32) + reserved(i32)
- centroid table: centroid_count * dims * i16
- list directory (72 bytes each):
  - start_block(i32)
  - block_count(i32)
  - len(i32)
  - reserved(i32)
  - min[dims] (i16)
  - max[dims] (i16)
- vectors in AoSoA blocks (LANES=8): for each block -> dims -> lanes -> i16
- labels in block order: for each block -> lanes -> u8
"""

from __future__ import annotations

import argparse
import gzip
import json
import random
import struct
from array import array
from pathlib import Path

try:
    import numpy as np
except ImportError:  # pragma: no cover - fallback path
    np = None

INPUT_PATH = Path("resources/references.json.gz")
OUTPUT_IDX_PATH = Path("resources/references.idx")

DIMS = 14
LANES = 8
SCALE = 10000
MAGIC = b"RNSPIVF1"

DEFAULT_NLIST = 64
DEFAULT_MAX_ITER = 10
DEFAULT_SEED = 2026
DEFAULT_CHUNK = 4096


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build IVF binary index for fraud scoring")
    parser.add_argument("--input", type=Path, default=INPUT_PATH, help="path to references.json.gz")
    parser.add_argument("--output", type=Path, default=OUTPUT_IDX_PATH, help="path to output .idx")
    parser.add_argument("--nlist", type=int, default=DEFAULT_NLIST, help="number of IVF centroids")
    parser.add_argument(
        "--max-iter", type=int, default=DEFAULT_MAX_ITER, help="max k-means iterations"
    )
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="random seed")
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=DEFAULT_CHUNK,
        help="chunk size used during vectorized assignment",
    )
    return parser.parse_args()


def quantize(value: float) -> int:
    if value <= -1.0:
        return -SCALE
    if value <= 0.0:
        return 0
    if value >= 1.0:
        return SCALE
    return int(round(value * SCALE))


def clamp_i16(value: int) -> int:
    if value < -32768:
        return -32768
    if value > 32767:
        return 32767
    return value


def assign_points_numpy(vectors: "np.ndarray", centroids: "np.ndarray", chunk_size: int) -> "np.ndarray":
    n = vectors.shape[0]
    assignments = np.empty(n, dtype=np.int32)

    centroid_sq = np.sum(centroids * centroids, axis=1, dtype=np.float32)

    for start in range(0, n, chunk_size):
        end = min(start + chunk_size, n)
        chunk = vectors[start:end]
        chunk_sq = np.sum(chunk * chunk, axis=1, dtype=np.float32)[:, None]

        # ||x-c||^2 = ||x||^2 + ||c||^2 - 2x.c
        dists = chunk_sq + centroid_sq[None, :] - 2.0 * (chunk @ centroids.T)
        assignments[start:end] = np.argmin(dists, axis=1).astype(np.int32)

    return assignments


def fit_kmeans_numpy(
    vectors_q16: "np.ndarray", nlist: int, max_iter: int, seed: int, chunk_size: int
) -> tuple["np.ndarray", "np.ndarray"]:
    rng = np.random.default_rng(seed)
    n = vectors_q16.shape[0]

    seed_indices = rng.choice(n, size=nlist, replace=False)
    centroids = vectors_q16[seed_indices].astype(np.float32, copy=True)

    prev_assignments = None

    for it in range(max_iter):
        assignments = assign_points_numpy(vectors_q16, centroids, chunk_size)
        changes = (
            int(assignments.size)
            if prev_assignments is None
            else int(np.count_nonzero(assignments != prev_assignments))
        )

        counts = np.bincount(assignments, minlength=nlist).astype(np.int64)
        sums = np.zeros((nlist, DIMS), dtype=np.float64)
        for d in range(DIMS):
            sums[:, d] = np.bincount(assignments, weights=vectors_q16[:, d], minlength=nlist)

        non_empty = counts > 0
        centroids[non_empty] = (sums[non_empty] / counts[non_empty, None]).astype(np.float32)

        empty_idx = np.flatnonzero(~non_empty)
        if empty_idx.size > 0:
            refill = rng.choice(n, size=empty_idx.size, replace=False)
            centroids[empty_idx] = vectors_q16[refill].astype(np.float32)

        prev_assignments = assignments
        print(f"kmeans iter {it + 1}/{max_iter}: changed={changes}")
        if changes == 0:
            break

    final_assignments = assign_points_numpy(vectors_q16, centroids, chunk_size)
    return centroids, final_assignments


def l2_sq_int(a: list[int], b: list[int]) -> int:
    total = 0
    for d in range(DIMS):
        diff = a[d] - b[d]
        total += diff * diff
    return total


def nearest_centroid_int(vec: list[int], centroids: list[list[int]]) -> int:
    best_idx = 0
    best_dist = l2_sq_int(vec, centroids[0])
    for cidx in range(1, len(centroids)):
        dist = l2_sq_int(vec, centroids[cidx])
        if dist < best_dist:
            best_idx = cidx
            best_dist = dist
    return best_idx


def fit_kmeans_fallback(vectors: list[list[int]], nlist: int, max_iter: int, seed: int) -> tuple[list[list[int]], list[int]]:
    # Python-only fallback (no numpy): keep a conservative setup to avoid very long build times.
    n = len(vectors)
    rng = random.Random(seed)
    seeds = rng.sample(range(n), nlist)
    centroids = [vectors[idx][:] for idx in seeds]

    assignments = [-1] * n
    for it in range(max_iter):
        sums = [[0] * DIMS for _ in range(nlist)]
        counts = [0] * nlist
        changes = 0

        for i, vec in enumerate(vectors):
            cidx = nearest_centroid_int(vec, centroids)
            if assignments[i] != cidx:
                assignments[i] = cidx
                changes += 1

            counts[cidx] += 1
            dst = sums[cidx]
            for d in range(DIMS):
                dst[d] += vec[d]

        for cidx in range(nlist):
            ccount = counts[cidx]
            if ccount == 0:
                centroids[cidx] = vectors[(cidx * 1301081 + it * 7919) % n][:]
                continue
            src = sums[cidx]
            dst = centroids[cidx]
            inv = float(ccount)
            for d in range(DIMS):
                dst[d] = int(round(src[d] / inv))

        print(f"kmeans iter {it + 1}/{max_iter}: changed={changes}")
        if changes == 0:
            break

    final_assignments = [nearest_centroid_int(vec, centroids) for vec in vectors]
    return centroids, final_assignments


def main() -> None:
    args = parse_args()

    if not args.input.exists():
        raise FileNotFoundError(f"input not found: {args.input}")

    with gzip.open(args.input, "rt", encoding="utf-8") as f:
        rows = json.load(f)

    if not isinstance(rows, list) or len(rows) == 0:
        raise ValueError("invalid or empty dataset")

    vectors: list[list[int]] = []
    labels: list[int] = []

    for i, row in enumerate(rows):
        vector = row.get("vector")
        label = row.get("label")
        if not isinstance(vector, list) or len(vector) != DIMS:
            raise ValueError(f"invalid vector at index {i}")

        vectors.append([quantize(float(x)) for x in vector])
        labels.append(1 if label == "fraud" else 0)

    total_count = len(vectors)
    nlist = max(1, min(args.nlist, total_count))

    if np is not None:
        vectors_np = np.asarray(vectors, dtype=np.float32)
        centroids_np, assignments_np = fit_kmeans_numpy(
            vectors_np, nlist=nlist, max_iter=args.max_iter, seed=args.seed, chunk_size=max(256, args.chunk_size)
        )
        centroids_int = np.rint(centroids_np).astype(np.int16)
        assignments = assignments_np.tolist()
    else:  # pragma: no cover - fallback path
        if nlist > 32:
            print("numpy not available; clamping nlist to 32 for manageable build time")
            nlist = 32
        fallback_iter = min(args.max_iter, 6)
        centroids_py, assignments = fit_kmeans_fallback(vectors, nlist, fallback_iter, args.seed)
        centroids_int = [[clamp_i16(v) for v in c] for c in centroids_py]

    postings: list[list[int]] = [[] for _ in range(nlist)]
    for vidx, cidx in enumerate(assignments):
        postings[cidx].append(vidx)

    vectors_blob = array("h")
    labels_blob = bytearray()
    list_entries: list[tuple[int, int, int, list[int], list[int]]] = []

    running_blocks = 0
    for cidx in range(nlist):
        ids = postings[cidx]
        llen = len(ids)
        lblocks = (llen + LANES - 1) // LANES
        start_block = running_blocks

        minv = [32767] * DIMS
        maxv = [-32768] * DIMS

        if llen == 0:
            minv = [0] * DIMS
            maxv = [0] * DIMS

        for b in range(lblocks):
            base = b * LANES
            for d in range(DIMS):
                for lane in range(LANES):
                    pos = base + lane
                    if pos < llen:
                        vec = vectors[ids[pos]]
                        val = vec[d]
                        vectors_blob.append(val)
                        if val < minv[d]:
                            minv[d] = val
                        if val > maxv[d]:
                            maxv[d] = val
                    else:
                        vectors_blob.append(0)
            for lane in range(LANES):
                pos = base + lane
                if pos < llen:
                    labels_blob.append(labels[ids[pos]])
                else:
                    labels_blob.append(0)

        running_blocks += lblocks
        list_entries.append((start_block, lblocks, llen, minv, maxv))

    total_blocks = running_blocks

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
            nlist,
            total_blocks,
            0,
        )
    )

    for cidx in range(nlist):
        centroid = centroids_int[cidx]
        if np is not None:
            packed = [clamp_i16(int(v)) for v in centroid.tolist()]
        else:  # pragma: no cover - fallback path
            packed = [clamp_i16(int(v)) for v in centroid]
        out.extend(struct.pack("<" + "h" * DIMS, *packed))

    for start_block, block_count, llen, minv, maxv in list_entries:
        out.extend(struct.pack("<iiii", start_block, block_count, llen, 0))
        out.extend(struct.pack("<" + "h" * DIMS, *minv))
        out.extend(struct.pack("<" + "h" * DIMS, *maxv))

    out.extend(vectors_blob.tobytes())
    out.extend(labels_blob)

    args.output.write_bytes(out)

    empty_lists = sum(1 for ids in postings if len(ids) == 0)
    largest_list = max((len(ids) for ids in postings), default=0)

    print(f"done: {total_count} vectors")
    print(f"nlist: {nlist}")
    print(f"empty lists: {empty_lists}")
    print(f"largest list: {largest_list}")
    print(f"blocks: {total_blocks}")
    print(f"wrote: {args.output}")
    print(f"size: {len(out)} bytes")


if __name__ == "__main__":
    main()
