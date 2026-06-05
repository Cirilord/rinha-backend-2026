#include "x_score.h"

#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#define PACKED_DIMENSIONS 16
#define DIM_PAIRS 7
#define LANES 8
#define MAX_PARTITIONS 512
#define LEAF_FLAG UINT32_MAX
#define Q_SCALE 10000.0f
#define REFS_RECORD_BYTES (X_SCORE_DIMS * 2 + 1)
#define FILE_NODE_BYTES (X_SCORE_DIMS * 2 * 2 + 4 * 3)
#define FILE_PARTITION_BYTES (4 * 3 + X_SCORE_DIMS * 2 * 2)

typedef struct {
  int16_t aabb_min[X_SCORE_DIMS];
  int16_t aabb_max[X_SCORE_DIMS];
  uint32_t left;
  uint32_t right;
  uint32_t count;
} FileNode;

typedef struct {
  uint32_t key;
  uint32_t root;
  uint32_t count;
  int16_t aabb_min[X_SCORE_DIMS];
  int16_t aabb_max[X_SCORE_DIMS];
} FilePartition;

typedef struct {
  int16_t aabb_min[PACKED_DIMENSIONS];
  int16_t aabb_max[PACKED_DIMENSIONS];
  uint32_t left;
  uint32_t right;
  uint32_t chunk_start;
  uint32_t chunk_end;
  uint16_t parent_diff_mask;
  uint8_t label_mask;
} KdNode;

typedef struct {
  uint32_t key;
  uint32_t root;
  int16_t aabb_min[PACKED_DIMENSIONS];
  int16_t aabb_max[PACKED_DIMENSIONS];
} KdPartition;

typedef struct {
  int16_t pairs[DIM_PAIRS][2 * LANES];
} LeafVecChunk;

typedef struct {
  uint32_t ref_indices[LANES];
  uint8_t labels[LANES];
  uint8_t len;
} LeafMeta;

typedef struct {
  uint32_t idx;
  int64_t bound;
} NodeStackEntry;

typedef struct {
  const XScoreIndexView *index;
  const int32_t *query_q;
  const int16_t *query_q16;
#if defined(__AVX2__)
  __m256i query_pairs[DIM_PAIRS];
#endif
  int64_t top_dist[X_SCORE_TOPK];
  uint8_t top_label[X_SCORE_TOPK];
  uint32_t top_indices[X_SCORE_TOPK];
} Search;

typedef struct {
  int64_t bound;
  uint32_t idx;
} PartitionEntry;

static const int g_c_kd_bound_thread = 1;
static const int g_c_kd_primary_map = 1;
static const int g_c_kd_inline_sort = 1;
static const int g_c_kd_best_first = 0;
static const int g_c_kd_best_first_primary_only = 0;
static const int g_c_kd_delta_bounds = 0;
static const int g_c_kd_pair_leaf = 0;
static const int g_c_kd_prefetch_dist = 3;
static const int g_c_kd_prefetch_meta = 0;
static const int g_c_quant_once = 0;
static const int g_c_kdtree_extra_split = 0;
static const int64_t g_c_kd_early_limit = 0;

static int le16s(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void *xmalloc(size_t size) {
  void *ptr = malloc(size);
  if (ptr == NULL && size != 0) {
    abort();
  }
  return ptr;
}

static void *hot_alloc(size_t size) { return xmalloc(size); }

static void hot_prefetch(void *ptr, size_t size) {
#ifdef MADV_WILLNEED
  if (ptr != NULL && size > 0) {
    (void)madvise(ptr, size, MADV_WILLNEED);
  }
#else
  (void)ptr;
  (void)size;
#endif
}

static void derive_resource_paths(const char *hint, char *refs_path, size_t refs_len,
                                  char *tree_path, size_t tree_len) {
  const char *slash = strrchr(hint, '/');
  size_t dir_len = slash ? (size_t)(slash - hint) : 0;
  if (dir_len == 0) {
    (void)snprintf(refs_path, refs_len, "refs.bin");
    (void)snprintf(tree_path, tree_len, "kdtree.bin");
    return;
  }

  if (dir_len >= refs_len) {
    dir_len = refs_len - 1;
  }
  memcpy(refs_path, hint, dir_len);
  refs_path[dir_len] = '\0';
  memcpy(tree_path, hint, dir_len);
  tree_path[dir_len] = '\0';
  (void)snprintf(refs_path + dir_len, refs_len - dir_len, "/refs.bin");
  (void)snprintf(tree_path + dir_len, tree_len - dir_len, "/kdtree.bin");
}

static FileNode decode_node(const uint8_t buf[FILE_NODE_BYTES]) {
  FileNode n;
  size_t p = 0;
  for (int d = 0; d < X_SCORE_DIMS; d++, p += 2) {
    n.aabb_min[d] = (int16_t)le16s(buf + p);
  }
  for (int d = 0; d < X_SCORE_DIMS; d++, p += 2) {
    n.aabb_max[d] = (int16_t)le16s(buf + p);
  }
  n.left = le32(buf + p);
  n.right = le32(buf + p + 4);
  n.count = le32(buf + p + 8);
  return n;
}

static FilePartition decode_partition(const uint8_t buf[FILE_PARTITION_BYTES]) {
  FilePartition part;
  size_t p = 0;
  part.key = le32(buf + p);
  p += 4;
  part.root = le32(buf + p);
  p += 4;
  part.count = le32(buf + p);
  p += 4;
  for (int d = 0; d < X_SCORE_DIMS; d++, p += 2) {
    part.aabb_min[d] = (int16_t)le16s(buf + p);
  }
  for (int d = 0; d < X_SCORE_DIMS; d++, p += 2) {
    part.aabb_max[d] = (int16_t)le16s(buf + p);
  }
  return part;
}

static inline int32_t q_i32(float v) { return (int32_t)lrintf(v * Q_SCALE); }

static inline int16_t q_i16(float v) {
  int32_t q = q_i32(v);
  if (q < INT16_MIN) {
    q = INT16_MIN;
  }
  if (q > INT16_MAX) {
    q = INT16_MAX;
  }
  return (int16_t)q;
}

static uint32_t partition_key_i16(const int16_t v[X_SCORE_DIMS]) {
  uint32_t key = 0;
  if (v[5] >= 0) {
    key |= 1u << 0;
  }
  if (v[9] > 0) {
    key |= 1u << 1;
  }
  if (v[10] > 0) {
    key |= 1u << 2;
  }
  if (v[11] > 0) {
    key |= 1u << 3;
  }

  {
    uint32_t mcc_bucket = 3;
    if (v[12] <= 2047) {
      mcc_bucket = 0;
    } else if (v[12] <= 4095) {
      mcc_bucket = 1;
    } else if (v[12] <= 6143) {
      mcc_bucket = 2;
    }
    key |= mcc_bucket << 4;
  }

  if (v[2] > 4096) {
    key |= 1u << 6;
  }
  if (v[8] > 2048) {
    key |= 1u << 7;
  }

  switch (g_c_kdtree_extra_split) {
    case 1:
      if (v[7] > 2048) {
        key |= 1u << 8;
      }
      break;
    case 2:
      if (v[0] > 2048) {
        key |= 1u << 8;
      }
      break;
    case 3:
      if (v[6] > 2048) {
        key |= 1u << 8;
      }
      break;
    case 4:
      if (v[3] > 5000) {
        key |= 1u << 8;
      }
      break;
    case 5:
      if (v[13] > 200) {
        key |= 1u << 8;
      }
      break;
    default:
      break;
  }

  return key;
}

static inline int64_t dim_gap_sq_i64(int16_t q, int16_t lo, int16_t hi) {
  int64_t gap = 0;
  if (q < lo) {
    gap = (int64_t)lo - q;
  } else if (q > hi) {
    gap = (int64_t)q - hi;
  }
  return gap * gap;
}

static inline int64_t aabb_lower_bound_i64(const int16_t q[PACKED_DIMENSIONS],
                                           const int16_t lo[PACKED_DIMENSIONS],
                                           const int16_t hi[PACKED_DIMENSIONS]) {
#if defined(__AVX2__)
  __m256i qv = _mm256_loadu_si256((const __m256i *)(const void *)q);
  __m256i lov = _mm256_loadu_si256((const __m256i *)(const void *)lo);
  __m256i hiv = _mm256_loadu_si256((const __m256i *)(const void *)hi);
  __m256i zero = _mm256_setzero_si256();
  __m256i low_gap = _mm256_max_epi16(_mm256_sub_epi16(lov, qv), zero);
  __m256i high_gap = _mm256_max_epi16(_mm256_sub_epi16(qv, hiv), zero);
  __m256i gap = _mm256_max_epi16(low_gap, high_gap);
  __m256i pair_sums = _mm256_madd_epi16(gap, gap);
  int32_t tmp[8];
  _mm256_storeu_si256((__m256i *)(void *)tmp, pair_sums);
  {
    int64_t sum = 0;
    for (int i = 0; i < 8; i++) {
      sum += tmp[i];
    }
    return sum;
  }
#else
  int64_t sum = 0;
  for (int i = 0; i < PACKED_DIMENSIONS; i++) {
    sum += dim_gap_sq_i64(q[i], lo[i], hi[i]);
  }
  return sum;
#endif
}

static inline bool candidate_before(int64_t dist, uint32_t ref_index, int64_t other_dist,
                                    uint32_t other_index) {
  return dist < other_dist || (dist == other_dist && ref_index < other_index);
}

static inline bool can_still_improve(Search *s, int64_t lb) {
  int64_t cutoff = s->top_dist[X_SCORE_TOPK - 1];
  return cutoff == INT64_MAX || lb <= cutoff;
}

static inline void insert_top(Search *s, int64_t dist, uint8_t label, uint32_t ref_index) {
  if (!candidate_before(dist, ref_index, s->top_dist[X_SCORE_TOPK - 1],
                        s->top_indices[X_SCORE_TOPK - 1])) {
    return;
  }

  int i = X_SCORE_TOPK;
  while (i > 0 &&
         candidate_before(dist, ref_index, s->top_dist[i - 1], s->top_indices[i - 1])) {
    i--;
  }
  if (i >= X_SCORE_TOPK) {
    return;
  }

  for (int j = X_SCORE_TOPK - 1; j > i; j--) {
    s->top_dist[j] = s->top_dist[j - 1];
    s->top_label[j] = s->top_label[j - 1];
    s->top_indices[j] = s->top_indices[j - 1];
  }
  s->top_dist[i] = dist;
  s->top_label[i] = label;
  s->top_indices[i] = ref_index;
}

static inline void insert_top_from_meta(Search *s, int64_t dist, const LeafMeta *m, int lane) {
  int64_t cutoff = s->top_dist[X_SCORE_TOPK - 1];
  if (dist > cutoff) {
    return;
  }
  {
    uint32_t ref_index = m->ref_indices[lane];
    if (!candidate_before(dist, ref_index, cutoff, s->top_indices[X_SCORE_TOPK - 1])) {
      return;
    }
    insert_top(s, dist, m->labels[lane], ref_index);
  }
}

static inline bool early_done(Search *s) {
  if (g_c_kd_early_limit <= 0 || s->top_dist[X_SCORE_TOPK - 1] > g_c_kd_early_limit) {
    return false;
  }
  return true;
}

static inline void chunk_distances_madd(Search *s, const LeafVecChunk *c, int64_t dist[LANES]) {
#if defined(__AVX2__)
  __m256i acc = _mm256_setzero_si256();
  for (int p = 0; p < DIM_PAIRS; p++) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)c->pairs[p]);
    __m256i diff = _mm256_sub_epi16(v, s->query_pairs[p]);
    acc = _mm256_add_epi32(acc, _mm256_madd_epi16(diff, diff));
  }
  {
    __m128i lo = _mm256_castsi256_si128(acc);
    __m128i hi = _mm256_extracti128_si256(acc, 1);
    _mm256_storeu_si256((__m256i *)(void *)dist, _mm256_cvtepi32_epi64(lo));
    _mm256_storeu_si256((__m256i *)(void *)(dist + 4), _mm256_cvtepi32_epi64(hi));
  }
#else
  for (int lane = 0; lane < LANES; lane++) {
    int64_t acc = 0;
    for (int d = 0; d < X_SCORE_DIMS; d++) {
      int64_t diff = (int64_t)s->query_q[d] - c->pairs[d >> 1][(lane << 1) | (d & 1)];
      acc += diff * diff;
    }
    dist[lane] = acc;
  }
#endif
}

static inline void chunk_distances_pair_madd(Search *s, const LeafVecChunk *c0,
                                             const LeafVecChunk *c1, int64_t dist0[LANES],
                                             int64_t dist1[LANES]) {
#if defined(__AVX2__)
  __m256i acc0 = _mm256_setzero_si256();
  __m256i acc1 = _mm256_setzero_si256();
  for (int p = 0; p < DIM_PAIRS; p++) {
    __m256i q = s->query_pairs[p];
    __m256i v0 = _mm256_loadu_si256((const __m256i *)(const void *)c0->pairs[p]);
    __m256i d0 = _mm256_sub_epi16(v0, q);
    acc0 = _mm256_add_epi32(acc0, _mm256_madd_epi16(d0, d0));
    __m256i v1 = _mm256_loadu_si256((const __m256i *)(const void *)c1->pairs[p]);
    __m256i d1 = _mm256_sub_epi16(v1, q);
    acc1 = _mm256_add_epi32(acc1, _mm256_madd_epi16(d1, d1));
  }
  {
    __m128i lo0 = _mm256_castsi256_si128(acc0);
    __m128i hi0 = _mm256_extracti128_si256(acc0, 1);
    __m128i lo1 = _mm256_castsi256_si128(acc1);
    __m128i hi1 = _mm256_extracti128_si256(acc1, 1);
    _mm256_storeu_si256((__m256i *)(void *)dist0, _mm256_cvtepi32_epi64(lo0));
    _mm256_storeu_si256((__m256i *)(void *)(dist0 + 4), _mm256_cvtepi32_epi64(hi0));
    _mm256_storeu_si256((__m256i *)(void *)dist1, _mm256_cvtepi32_epi64(lo1));
    _mm256_storeu_si256((__m256i *)(void *)(dist1 + 4), _mm256_cvtepi32_epi64(hi1));
  }
#else
  chunk_distances_madd(s, c0, dist0);
  chunk_distances_madd(s, c1, dist1);
#endif
}

static inline void insert_chunk_distances(Search *s, const LeafMeta *m, const int64_t dist[LANES],
                                          int lane_count) {
  for (int lane = 0; lane < lane_count; lane++) {
    insert_top_from_meta(s, dist[lane], m, lane);
  }
}

static void scan_chunk(Search *s, size_t chunk_id, int lane_count) {
  const LeafVecChunk *c = &((const LeafVecChunk *)s->index->chunks)[chunk_id];
  const LeafMeta *m = &((const LeafMeta *)s->index->metas)[chunk_id];
  int64_t distances[LANES];
  chunk_distances_madd(s, c, distances);
  insert_chunk_distances(s, m, distances, lane_count);
}

static inline int64_t child_lb(Search *s, uint32_t node_idx) {
  const KdNode *n = &((const KdNode *)s->index->nodes)[node_idx];
  return aabb_lower_bound_i64(s->query_q16, n->aabb_min, n->aabb_max);
}

static inline int64_t child_lb_from_parent(Search *s, const KdNode *parent, uint32_t node_idx,
                                           int64_t parent_bound) {
  const KdNode *child = &((const KdNode *)s->index->nodes)[node_idx];
  if (!g_c_kd_delta_bounds) {
    return aabb_lower_bound_i64(s->query_q16, child->aabb_min, child->aabb_max);
  }

  {
    int64_t bound = parent_bound;
    uint16_t mask = child->parent_diff_mask;
    while (mask != 0) {
      int d = __builtin_ctz(mask);
      mask = (uint16_t)(mask - 1);
      bound -= dim_gap_sq_i64(s->query_q16[d], parent->aabb_min[d], parent->aabb_max[d]);
      bound += dim_gap_sq_i64(s->query_q16[d], child->aabb_min[d], child->aabb_max[d]);
    }
    return bound;
  }
}

static inline bool node_entry_before(NodeStackEntry a, NodeStackEntry b) {
  return a.bound < b.bound || (a.bound == b.bound && a.idx < b.idx);
}

static bool node_heap_push(NodeStackEntry *heap, size_t *len, NodeStackEntry e) {
  if (*len >= 8192) {
    return false;
  }

  {
    size_t i = (*len)++;
    while (i > 0) {
      size_t parent = (i - 1) >> 1;
      if (!node_entry_before(e, heap[parent])) {
        break;
      }
      heap[i] = heap[parent];
      i = parent;
    }
    heap[i] = e;
  }
  return true;
}

static NodeStackEntry node_heap_pop(NodeStackEntry *heap, size_t *len) {
  NodeStackEntry out = heap[0];
  NodeStackEntry last = heap[--(*len)];
  if (*len == 0) {
    return out;
  }

  {
    size_t i = 0;
    while (1) {
      size_t left = i * 2 + 1;
      if (left >= *len) {
        break;
      }
      size_t right = left + 1;
      size_t child = left;
      if (right < *len && node_entry_before(heap[right], heap[left])) {
        child = right;
      }
      if (!node_entry_before(heap[child], last)) {
        break;
      }
      heap[i] = heap[child];
      i = child;
    }
    heap[i] = last;
  }
  return out;
}

static void visit_node_dfs(Search *s, uint32_t node_idx, int64_t bound) {
  NodeStackEntry stack[1024];
  size_t sp = 0;
  uint32_t current = node_idx;
  int64_t current_bound = bound;
  const KdNode *nodes = (const KdNode *)s->index->nodes;
  const LeafMeta *metas = (const LeafMeta *)s->index->metas;

  while (1) {
    if (!g_c_kd_bound_thread) {
      current_bound = child_lb(s, current);
    }

    if (can_still_improve(s, current_bound)) {
      const KdNode *n = &nodes[current];
      if (n->left == LEAF_FLAG) {
        for (uint32_t cid = n->chunk_start; cid < n->chunk_end; cid++) {
          if (g_c_kd_prefetch_dist > 0) {
            uint32_t target = cid + (uint32_t)g_c_kd_prefetch_dist;
            if (target < n->chunk_end) {
              __builtin_prefetch(&((const LeafVecChunk *)s->index->chunks)[target], 0, 3);
              if (g_c_kd_prefetch_meta) {
                __builtin_prefetch(&metas[target], 0, 3);
              }
            }
          }

          if (g_c_kd_pair_leaf && cid + 1 < n->chunk_end) {
            int64_t dist0[LANES];
            int64_t dist1[LANES];
            chunk_distances_pair_madd(
              s, &((const LeafVecChunk *)s->index->chunks)[cid],
              &((const LeafVecChunk *)s->index->chunks)[cid + 1], dist0, dist1);
            insert_chunk_distances(s, &metas[cid], dist0, LANES);
            if (early_done(s)) {
              return;
            }
            {
              int lane_count1 = (cid + 2 == n->chunk_end) ? metas[cid + 1].len : LANES;
              insert_chunk_distances(s, &metas[cid + 1], dist1, lane_count1);
            }
            if (early_done(s)) {
              return;
            }
            cid++;
            continue;
          }

          {
            int lane_count = (cid + 1 == n->chunk_end) ? metas[cid].len : LANES;
            scan_chunk(s, cid, lane_count);
            if (early_done(s)) {
              return;
            }
          }
        }
      } else {
        uint32_t l = n->left;
        uint32_t r = n->right;
        int64_t dl = child_lb_from_parent(s, n, l, current_bound);
        int64_t dr = child_lb_from_parent(s, n, r, current_bound);
        uint32_t near = l;
        uint32_t far = r;
        int64_t near_b = dl;
        int64_t far_b = dr;

        if (dr < dl) {
          near = r;
          far = l;
          near_b = dr;
          far_b = dl;
        }

        if (can_still_improve(s, far_b) && sp < 1024) {
          stack[sp++] = (NodeStackEntry){far, far_b};
        }
        current = near;
        current_bound = near_b;
        continue;
      }
    }

    if (sp == 0 || early_done(s)) {
      return;
    }
    {
      NodeStackEntry next = stack[--sp];
      current = next.idx;
      current_bound = next.bound;
    }
  }
}

static void visit_node_best_first(Search *s, uint32_t node_idx, int64_t bound) {
  NodeStackEntry heap[8192];
  size_t len = 0;
  const KdNode *nodes = (const KdNode *)s->index->nodes;
  const LeafMeta *metas = (const LeafMeta *)s->index->metas;

  (void)node_heap_push(heap, &len, (NodeStackEntry){node_idx, bound});
  while (len > 0 && !early_done(s)) {
    NodeStackEntry e = node_heap_pop(heap, &len);
    if (!can_still_improve(s, e.bound)) {
      continue;
    }

    {
      const KdNode *n = &nodes[e.idx];
      if (n->left == LEAF_FLAG) {
        for (uint32_t cid = n->chunk_start; cid < n->chunk_end; cid++) {
          if (g_c_kd_prefetch_dist > 0) {
            uint32_t target = cid + (uint32_t)g_c_kd_prefetch_dist;
            if (target < n->chunk_end) {
              __builtin_prefetch(&((const LeafVecChunk *)s->index->chunks)[target], 0, 3);
              if (g_c_kd_prefetch_meta) {
                __builtin_prefetch(&metas[target], 0, 3);
              }
            }
          }

          if (g_c_kd_pair_leaf && cid + 1 < n->chunk_end) {
            int64_t dist0[LANES];
            int64_t dist1[LANES];
            chunk_distances_pair_madd(
              s, &((const LeafVecChunk *)s->index->chunks)[cid],
              &((const LeafVecChunk *)s->index->chunks)[cid + 1], dist0, dist1);
            insert_chunk_distances(s, &metas[cid], dist0, LANES);
            if (early_done(s)) {
              return;
            }
            {
              int lane_count1 = (cid + 2 == n->chunk_end) ? metas[cid + 1].len : LANES;
              insert_chunk_distances(s, &metas[cid + 1], dist1, lane_count1);
            }
            if (early_done(s)) {
              return;
            }
            cid++;
            continue;
          }

          {
            int lane_count = (cid + 1 == n->chunk_end) ? metas[cid].len : LANES;
            scan_chunk(s, cid, lane_count);
            if (early_done(s)) {
              return;
            }
          }
        }
      } else {
        uint32_t l = n->left;
        uint32_t r = n->right;
        int64_t dl = child_lb_from_parent(s, n, l, e.bound);
        int64_t dr = child_lb_from_parent(s, n, r, e.bound);
        if (can_still_improve(s, dl) && !node_heap_push(heap, &len, (NodeStackEntry){l, dl})) {
          visit_node_dfs(s, l, dl);
        }
        if (early_done(s)) {
          return;
        }
        if (can_still_improve(s, dr) && !node_heap_push(heap, &len, (NodeStackEntry){r, dr})) {
          visit_node_dfs(s, r, dr);
        }
      }
    }
  }
}

static void visit_node_bound(Search *s, uint32_t node_idx, int64_t bound) {
  if (g_c_kd_best_first) {
    visit_node_best_first(s, node_idx, bound);
  } else {
    visit_node_dfs(s, node_idx, bound);
  }
}

static inline bool use_best_first_for_primary_key(uint32_t query_key) {
  (void)query_key;
  return g_c_kd_best_first;
}

static void visit_primary_node(Search *s, uint32_t node_idx, int64_t bound, uint32_t query_key) {
  if (use_best_first_for_primary_key(query_key)) {
    visit_node_best_first(s, node_idx, bound);
  } else {
    visit_node_dfs(s, node_idx, bound);
  }
}

static inline bool partition_before(PartitionEntry a, PartitionEntry b) {
  return a.bound < b.bound || (a.bound == b.bound && a.idx < b.idx);
}

static void sort_partition_entries(PartitionEntry *entries, size_t len) {
  for (size_t i = 1; i < len; i++) {
    PartitionEntry key = entries[i];
    size_t j = i;
    while (j > 0 && partition_before(key, entries[j - 1])) {
      entries[j] = entries[j - 1];
      j--;
    }
    entries[j] = key;
  }
}

static int cmp_partition_entry_qsort(const void *a, const void *b) {
  const PartitionEntry *x = (const PartitionEntry *)a;
  const PartitionEntry *y = (const PartitionEntry *)b;
  if (x->bound != y->bound) {
    return (x->bound > y->bound) ? 1 : -1;
  }
  return (x->idx > y->idx) - (x->idx < y->idx);
}

static void visit_partitions(Search *s, uint32_t query_key) {
  PartitionEntry entries[MAX_PARTITIONS];
  size_t len = 0;
  int32_t primary_idx = -1;
  const KdPartition *partitions = (const KdPartition *)s->index->partitions;

  if (g_c_kd_primary_map) {
    primary_idx = (query_key < MAX_PARTITIONS) ? s->index->part_by_key[query_key] : -1;
  } else {
    for (size_t i = 0; i < s->index->partition_count; i++) {
      if (partitions[i].key == query_key) {
        primary_idx = (int32_t)i;
        break;
      }
    }
  }

  if (primary_idx >= 0) {
    const KdPartition *primary = &partitions[primary_idx];
    int64_t primary_bound =
      aabb_lower_bound_i64(s->query_q16, primary->aabb_min, primary->aabb_max);
    if (can_still_improve(s, primary_bound)) {
      visit_primary_node(s, primary->root, primary_bound, query_key);
    }
    if (early_done(s)) {
      return;
    }
  }

  for (size_t i = 0; i < s->index->partition_count; i++) {
    if ((int32_t)i == primary_idx) {
      continue;
    }
    {
      const KdPartition *p = &partitions[i];
      int64_t bound = aabb_lower_bound_i64(s->query_q16, p->aabb_min, p->aabb_max);
      if (can_still_improve(s, bound)) {
        entries[len++] = (PartitionEntry){bound, (uint32_t)i};
      }
    }
  }

  if (g_c_kd_inline_sort) {
    sort_partition_entries(entries, len);
  } else {
    qsort(entries, len, sizeof(entries[0]), cmp_partition_entry_qsort);
  }

  for (size_t i = 0; i < len; i++) {
    if (!can_still_improve(s, entries[i].bound)) {
      break;
    }
    if (g_c_kd_best_first_primary_only) {
      visit_node_dfs(s, partitions[entries[i].idx].root, entries[i].bound);
    } else {
      visit_node_bound(s, partitions[entries[i].idx].root, entries[i].bound);
    }
    if (early_done(s)) {
      break;
    }
  }
}

static uint8_t finalize_search(Search *s) {
  uint8_t sum = 0;
  for (int i = 0; i < X_SCORE_TOPK; i++) {
    sum = (uint8_t)(sum + s->top_label[i]);
  }
  return sum;
}

static uint8_t kdtree_fraud_count(const XScoreIndexView *idx, const float query[X_SCORE_DIMS]) {
  int32_t q[X_SCORE_DIMS];
  int16_t qi16[PACKED_DIMENSIONS] = {0};
  Search s;
  memset(&s, 0, sizeof(s));
  s.index = idx;
  s.query_q = q;
  s.query_q16 = qi16;

  if (g_c_quant_once) {
    for (int d = 0; d < X_SCORE_DIMS; d++) {
      int32_t qd = q_i32(query[d]);
      q[d] = qd;
      if (qd < INT16_MIN) {
        qd = INT16_MIN;
      }
      if (qd > INT16_MAX) {
        qd = INT16_MAX;
      }
      qi16[d] = (int16_t)qd;
    }
  } else {
    for (int d = 0; d < X_SCORE_DIMS; d++) {
      q[d] = q_i32(query[d]);
      qi16[d] = q_i16(query[d]);
    }
  }

#if defined(__AVX2__)
  for (int p = 0; p < DIM_PAIRS; p++) {
    int32_t packed =
      (int32_t)((uint16_t)qi16[2 * p] | ((uint32_t)(uint16_t)qi16[2 * p + 1] << 16));
    s.query_pairs[p] = _mm256_set1_epi32(packed);
  }
#endif

  for (int i = 0; i < X_SCORE_TOPK; i++) {
    s.top_dist[i] = INT64_MAX;
    s.top_indices[i] = UINT32_MAX;
    s.top_label[i] = 0;
  }

  visit_partitions(&s, partition_key_i16(qi16));
  return finalize_search(&s);
}

bool x_score_open(const char *path, XScoreIndexView *out_view) {
  char refs_path[512];
  char tree_path[512];
  struct stat refs_st;
  struct stat tree_st;
  int refs_fd = -1;
  int tree_fd = -1;
  uint8_t *refs_raw = NULL;
  uint8_t *tree_raw = NULL;
  FilePartition *fparts = NULL;
  FileNode *fnodes = NULL;
  uint32_t *members = NULL;
  KdNode *nodes = NULL;
  uint32_t *slot = NULL;
  uint32_t *leaf_node_by_ref = NULL;
  uint8_t *node_masks = NULL;
  uint8_t *chunk_lens = NULL;
  LeafVecChunk *chunks = NULL;
  LeafMeta *metas = NULL;
  KdPartition *parts = NULL;
  size_t chunk_count = 0;

  if (path == NULL || out_view == NULL) {
    return false;
  }

  memset(out_view, 0, sizeof(*out_view));
  for (int i = 0; i < MAX_PARTITIONS; i++) {
    out_view->part_by_key[i] = -1;
  }

  derive_resource_paths(path, refs_path, sizeof(refs_path), tree_path, sizeof(tree_path));

  refs_fd = open(refs_path, O_RDONLY);
  if (refs_fd < 0) {
    goto fail;
  }
  tree_fd = open(tree_path, O_RDONLY);
  if (tree_fd < 0) {
    goto fail;
  }
  if (fstat(refs_fd, &refs_st) != 0 || refs_st.st_size <= 0) {
    goto fail;
  }
  if (fstat(tree_fd, &tree_st) != 0 || tree_st.st_size <= 0) {
    goto fail;
  }

  refs_raw = (uint8_t *)mmap(NULL, (size_t)refs_st.st_size, PROT_READ, MAP_SHARED, refs_fd, 0);
  tree_raw = (uint8_t *)mmap(NULL, (size_t)tree_st.st_size, PROT_READ, MAP_SHARED, tree_fd, 0);
  close(refs_fd);
  close(tree_fd);
  refs_fd = -1;
  tree_fd = -1;
  if (refs_raw == MAP_FAILED || tree_raw == MAP_FAILED) {
    goto fail;
  }

  if ((size_t)tree_st.st_size < 28 || memcmp(tree_raw, "RKDT", 4) != 0) {
    goto fail;
  }
  if (le32(tree_raw + 4) != 2) {
    goto fail;
  }
  {
    uint32_t count = le32(tree_raw + 8);
    uint32_t dim = le32(tree_raw + 12);
    uint32_t n_nodes = le32(tree_raw + 16);
    uint32_t n_parts = le32(tree_raw + 24);
    size_t offset = 28;

    if (dim != X_SCORE_DIMS || n_parts > MAX_PARTITIONS) {
      goto fail;
    }
    if (memcmp(refs_raw, "RINH", 4) != 0 || le32(refs_raw + 4) != 3 || le32(refs_raw + 8) != count) {
      goto fail;
    }

    if (offset + (size_t)n_parts * FILE_PARTITION_BYTES > (size_t)tree_st.st_size) {
      goto fail;
    }
    fparts = (FilePartition *)xmalloc((size_t)n_parts * sizeof(FilePartition));
    for (uint32_t i = 0; i < n_parts; i++) {
      fparts[i] = decode_partition(tree_raw + offset + (size_t)i * FILE_PARTITION_BYTES);
    }
    offset += (size_t)n_parts * FILE_PARTITION_BYTES;

    if (offset + (size_t)n_nodes * FILE_NODE_BYTES > (size_t)tree_st.st_size) {
      goto fail;
    }
    fnodes = (FileNode *)xmalloc((size_t)n_nodes * sizeof(FileNode));
    for (uint32_t i = 0; i < n_nodes; i++) {
      fnodes[i] = decode_node(tree_raw + offset + (size_t)i * FILE_NODE_BYTES);
    }
    offset += (size_t)n_nodes * FILE_NODE_BYTES;

    if (offset + (size_t)count * sizeof(uint32_t) > (size_t)tree_st.st_size) {
      goto fail;
    }
    members = (uint32_t *)xmalloc((size_t)count * sizeof(uint32_t));
    memcpy(members, tree_raw + offset, (size_t)count * sizeof(uint32_t));

    nodes = (KdNode *)hot_alloc((size_t)n_nodes * sizeof(KdNode));
    slot = (uint32_t *)xmalloc((size_t)count * sizeof(uint32_t));
    leaf_node_by_ref = (uint32_t *)xmalloc((size_t)count * sizeof(uint32_t));
    node_masks = (uint8_t *)xmalloc((size_t)n_nodes);
    memset(slot, 0xff, (size_t)count * sizeof(uint32_t));
    memset(leaf_node_by_ref, 0xff, (size_t)count * sizeof(uint32_t));
    memset(node_masks, 0, (size_t)n_nodes);
    chunk_lens = (uint8_t *)xmalloc((size_t)count);

    for (uint32_t i = 0; i < n_nodes; i++) {
      FileNode *f = &fnodes[i];
      KdNode *n = &nodes[i];
      memcpy(n->aabb_min, f->aabb_min, sizeof(f->aabb_min));
      memcpy(n->aabb_max, f->aabb_max, sizeof(f->aabb_max));
      n->aabb_min[14] = 0;
      n->aabb_min[15] = 0;
      n->aabb_max[14] = 0;
      n->aabb_max[15] = 0;
      n->left = f->left;
      n->right = f->right;
      n->chunk_start = 0;
      n->chunk_end = 0;
      n->parent_diff_mask = 0;
      n->label_mask = 0;

      if (f->left == LEAF_FLAG) {
        size_t member_start = f->right;
        size_t cnt = f->count;
        size_t n_chunks = (cnt + LANES - 1U) / LANES;
        size_t chunk_start = chunk_count;

        for (size_t ci = 0; ci < n_chunks; ci++) {
          size_t take = cnt - ci * LANES;
          if (take > LANES) {
            take = LANES;
          }
          chunk_lens[chunk_count++] = (uint8_t)take;
        }

        for (size_t j = 0; j < cnt; j++) {
          uint32_t ref_idx = members[member_start + j];
          uint32_t chunk_id = (uint32_t)(chunk_start + j / LANES);
          uint32_t lane = (uint32_t)(j % LANES);
          slot[ref_idx] = (chunk_id << 3) | lane;
          leaf_node_by_ref[ref_idx] = i;
        }

        n->right = 0;
        n->chunk_start = (uint32_t)chunk_start;
        n->chunk_end = (uint32_t)chunk_count;
      }
    }

    for (uint32_t i = 0; i < n_nodes; i++) {
      KdNode *n = &nodes[i];
      if (n->left == LEAF_FLAG) {
        continue;
      }
      {
        uint32_t child_ids[2] = {n->left, n->right};
        for (int ci = 0; ci < 2; ci++) {
          KdNode *child = &nodes[child_ids[ci]];
          uint16_t mask = 0;
          for (int d = 0; d < X_SCORE_DIMS; d++) {
            if (child->aabb_min[d] != n->aabb_min[d] || child->aabb_max[d] != n->aabb_max[d]) {
              mask |= (uint16_t)(1u << d);
            }
          }
          child->parent_diff_mask = mask;
        }
      }
    }

    chunks = (LeafVecChunk *)hot_alloc(chunk_count * sizeof(LeafVecChunk));
    metas = (LeafMeta *)hot_alloc(chunk_count * sizeof(LeafMeta));
    memset(chunks, 0, chunk_count * sizeof(LeafVecChunk));
    memset(metas, 0, chunk_count * sizeof(LeafMeta));
    for (size_t i = 0; i < chunk_count; i++) {
      metas[i].len = chunk_lens[i];
    }

    if ((size_t)refs_st.st_size < 12 + (size_t)count * REFS_RECORD_BYTES) {
      goto fail;
    }
    {
      const uint8_t *rec = refs_raw + 12;
      for (uint32_t ref_idx = 0; ref_idx < count; ref_idx++, rec += REFS_RECORD_BYTES) {
        uint32_t packed = slot[ref_idx];
        LeafVecChunk *c = &chunks[packed >> 3];
        LeafMeta *m = &metas[packed >> 3];
        int lane = (int)(packed & 7);
        for (int d = 0; d < X_SCORE_DIMS; d++) {
          c->pairs[d >> 1][(lane << 1) | (d & 1)] = (int16_t)le16s(rec + d * 2);
        }
        m->labels[lane] = rec[X_SCORE_DIMS * 2];
        m->ref_indices[lane] = ref_idx;
        node_masks[leaf_node_by_ref[ref_idx]] |= (uint8_t)(1u << m->labels[lane]);
      }
    }

    for (int64_t i = (int64_t)n_nodes - 1; i >= 0; i--) {
      KdNode *n = &nodes[i];
      if (n->left == LEAF_FLAG) {
        n->label_mask = node_masks[i];
      } else {
        n->label_mask = (uint8_t)(nodes[n->left].label_mask | nodes[n->right].label_mask);
      }
    }

    parts = (KdPartition *)hot_alloc((size_t)n_parts * sizeof(KdPartition));
    for (uint32_t i = 0; i < n_parts; i++) {
      parts[i].key = fparts[i].key;
      parts[i].root = fparts[i].root;
      memcpy(parts[i].aabb_min, fparts[i].aabb_min, sizeof(fparts[i].aabb_min));
      memcpy(parts[i].aabb_max, fparts[i].aabb_max, sizeof(fparts[i].aabb_max));
      parts[i].aabb_min[14] = 0;
      parts[i].aabb_min[15] = 0;
      parts[i].aabb_max[14] = 0;
      parts[i].aabb_max[15] = 0;
      if (parts[i].key < MAX_PARTITIONS) {
        out_view->part_by_key[parts[i].key] = (int32_t)i;
      }
    }

    out_view->refs_raw = refs_raw;
    out_view->refs_size = (size_t)refs_st.st_size;
    out_view->tree_raw = tree_raw;
    out_view->tree_size = (size_t)tree_st.st_size;
    out_view->mapped = 1;
    out_view->nodes = nodes;
    out_view->node_count = n_nodes;
    out_view->partitions = parts;
    out_view->partition_count = n_parts;
    out_view->chunks = chunks;
    out_view->metas = metas;
    out_view->chunk_count = chunk_count;
    out_view->count = count;

    hot_prefetch(nodes, (size_t)n_nodes * sizeof(KdNode));
    hot_prefetch(parts, (size_t)n_parts * sizeof(KdPartition));
    hot_prefetch(chunks, chunk_count * sizeof(LeafVecChunk));
  }

  free(fparts);
  free(fnodes);
  free(members);
  free(slot);
  free(leaf_node_by_ref);
  free(node_masks);
  free(chunk_lens);
  return true;

fail:
  free(fparts);
  free(fnodes);
  free(members);
  free(slot);
  free(leaf_node_by_ref);
  free(node_masks);
  free(chunk_lens);
  free(nodes);
  free(parts);
  free(chunks);
  free(metas);
  if (refs_fd >= 0) {
    close(refs_fd);
  }
  if (tree_fd >= 0) {
    close(tree_fd);
  }
  if (refs_raw != NULL && refs_raw != MAP_FAILED) {
    munmap(refs_raw, (size_t)refs_st.st_size);
  }
  if (tree_raw != NULL && tree_raw != MAP_FAILED) {
    munmap(tree_raw, (size_t)tree_st.st_size);
  }
  memset(out_view, 0, sizeof(*out_view));
  for (int i = 0; i < MAX_PARTITIONS; i++) {
    out_view->part_by_key[i] = -1;
  }
  return false;
}

void x_score_close(XScoreIndexView *view) {
  if (view == NULL) {
    return;
  }

  free(view->nodes);
  free(view->partitions);
  free(view->chunks);
  free(view->metas);

  if (view->mapped) {
    if (view->refs_raw != NULL && view->refs_size > 0) {
      munmap(view->refs_raw, view->refs_size);
    }
    if (view->tree_raw != NULL && view->tree_size > 0) {
      munmap(view->tree_raw, view->tree_size);
    }
  }

  memset(view, 0, sizeof(*view));
}

uint8_t x_score_predict_fraud_count(const XScoreIndexView *view, const double query[X_SCORE_DIMS]) {
  float qf[X_SCORE_DIMS];
  for (int i = 0; i < X_SCORE_DIMS; i++) {
    qf[i] = (float)query[i];
  }
  return kdtree_fraud_count(view, qf);
}

uint8_t x_score_predict_label(const XScoreIndexView *view, const double query[X_SCORE_DIMS]) {
  uint8_t fraud_count = x_score_predict_fraud_count(view, query);
  return (fraud_count >= 3) ? 1 : 0;
}
