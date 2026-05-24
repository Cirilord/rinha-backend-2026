#include "x-score.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#if defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

typedef struct {
  unsigned long long bound;
  uint32_t partition_index;
} PartitionCandidate;

typedef struct {
  uint32_t node_index;
  unsigned long long bound;
} NodeCandidate;

typedef struct {
  int16_t q[X_SCORE_DIMS];
#if defined(__AVX2__)
  __m256i q32[X_SCORE_DIMS];
#elif defined(__ARM_NEON__)
  int16x8_t q16x8[X_SCORE_DIMS];
#endif
} DetectorQuery;

#ifndef DETECTOR_EARLY_DISTANCE_MILLI
#define DETECTOR_EARLY_DISTANCE_MILLI 143
#endif

static const unsigned long long detector_early_distance_sq_limit =
  ((unsigned long long)X_SCORE_SCALE * (unsigned long long)DETECTOR_EARLY_DISTANCE_MILLI / 1000ULL) *
  ((unsigned long long)X_SCORE_SCALE * (unsigned long long)DETECTOR_EARLY_DISTANCE_MILLI / 1000ULL);

static inline int16_t quantize_value(double value) {
  if (value <= -1.0) {
    return (int16_t)-X_SCORE_SCALE;
  }
  if (value <= 0.0) {
    return 0;
  }
  if (value >= 1.0) {
    return (int16_t)X_SCORE_SCALE;
  }

  double scaled = value * (double)X_SCORE_SCALE;
  scaled += (scaled >= 0.0) ? 0.5 : -0.5;
  return (int16_t)scaled;
}

static inline uint32_t compute_partition_key(const int16_t q[X_SCORE_DIMS]) {
  uint32_t key = 0;

  if (q[5] >= 0) {
    key |= 1u << 0;
  }
  if (q[9] > 0) {
    key |= 1u << 1;
  }
  if (q[10] > 0) {
    key |= 1u << 2;
  }
  if (q[11] > 0) {
    key |= 1u << 3;
  }

  uint32_t mcc_bucket = 0;
  if (q[12] <= 2000) {
    mcc_bucket = 0;
  } else if (q[12] <= 3000) {
    mcc_bucket = 1;
  } else if (q[12] <= 7500) {
    mcc_bucket = 2;
  } else {
    mcc_bucket = 3;
  }
  key |= mcc_bucket << 4;

  if (q[2] > 1013) {
    key |= 1u << 6;
  }
  if (q[8] > 2500) {
    key |= 1u << 7;
  }

  return key;
}

static inline unsigned long long lower_bound_partition_sq_cutoff(
  const int16_t q[X_SCORE_DIMS], const XScorePartitionEntry *partition, unsigned long long cutoff) {
  unsigned long long sum = 0;
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    long long qq = (long long)q[d];
    long long lo = (long long)partition->min[d];
    long long hi = (long long)partition->max[d];
    long long diff = 0;

    if (qq < lo) {
      diff = lo - qq;
    } else if (qq > hi) {
      diff = qq - hi;
    }

    sum += (unsigned long long)(diff * diff);
    if (sum > cutoff) {
      return sum;
    }
  }

  return sum;
}

static inline unsigned long long lower_bound_node_sq_cutoff(const int16_t q[X_SCORE_DIMS],
                                                            const XScoreNodeEntry *node,
                                                            unsigned long long cutoff) {
  unsigned long long sum = 0;
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    long long qq = (long long)q[d];
    long long lo = (long long)node->min[d];
    long long hi = (long long)node->max[d];
    long long diff = 0;

    if (qq < lo) {
      diff = lo - qq;
    } else if (qq > hi) {
      diff = qq - hi;
    }

    sum += (unsigned long long)(diff * diff);
    if (sum > cutoff) {
      return sum;
    }
  }

  return sum;
}

static inline void topk_reset(unsigned long long top_dist[X_SCORE_TOPK], uint8_t top_label[X_SCORE_TOPK]) {
  for (int i = 0; i < X_SCORE_TOPK; i++) {
    top_dist[i] = ULLONG_MAX;
    top_label[i] = 0;
  }
}

static inline bool topk_early_done(const unsigned long long top_dist[X_SCORE_TOPK]) {
  return top_dist[X_SCORE_TOPK - 1] <= detector_early_distance_sq_limit;
}

static inline void topk_insert(unsigned long long dist, uint8_t label,
                               unsigned long long top_dist[X_SCORE_TOPK],
                               uint8_t top_label[X_SCORE_TOPK]) {
  if (dist >= top_dist[4]) {
    return;
  }

  if (dist < top_dist[3]) {
    top_dist[4] = top_dist[3];
    top_label[4] = top_label[3];

    if (dist < top_dist[2]) {
      top_dist[3] = top_dist[2];
      top_label[3] = top_label[2];

      if (dist < top_dist[1]) {
        top_dist[2] = top_dist[1];
        top_label[2] = top_label[1];

        if (dist < top_dist[0]) {
          top_dist[1] = top_dist[0];
          top_label[1] = top_label[0];
          top_dist[0] = dist;
          top_label[0] = label;
          return;
        }

        top_dist[1] = dist;
        top_label[1] = label;
        return;
      }

      top_dist[2] = dist;
      top_label[2] = label;
      return;
    }

    top_dist[3] = dist;
    top_label[3] = label;
    return;
  }

  top_dist[4] = dist;
  top_label[4] = label;
}

static inline void insert_partition_candidate_sorted(PartitionCandidate entries[256], uint32_t *entry_len,
                                                     unsigned long long bound, uint32_t partition_index) {
  uint32_t pos = *entry_len;
  while (pos > 0 && entries[pos - 1].bound > bound) {
    entries[pos] = entries[pos - 1];
    pos--;
  }
  entries[pos].bound = bound;
  entries[pos].partition_index = partition_index;
  (*entry_len)++;
}

static inline void prepare_query(const double query[X_SCORE_DIMS], DetectorQuery *ctx) {
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    int16_t qd = quantize_value(query[d]);
    ctx->q[d] = qd;
#if defined(__AVX2__)
    ctx->q32[d] = _mm256_set1_epi32((int)qd);
#elif defined(__ARM_NEON__)
    ctx->q16x8[d] = vdupq_n_s16(qd);
#endif
  }
}

static inline void scan_block(const int16_t *block, const DetectorQuery *ctx,
                              unsigned long long out_dist[X_SCORE_LANES]) {
#if defined(__AVX2__)
  __m256i acc_lo = _mm256_setzero_si256();
  __m256i acc_hi = _mm256_setzero_si256();
  __m256i acc32 = _mm256_setzero_si256();

#define DETECTOR_FLUSH_ACC32()                                                                    \
  do {                                                                                           \
    const __m128i sq_lo = _mm256_castsi256_si128(acc32);                                         \
    const __m128i sq_hi = _mm256_extracti128_si256(acc32, 1);                                    \
    acc_lo = _mm256_add_epi64(acc_lo, _mm256_cvtepi32_epi64(sq_lo));                             \
    acc_hi = _mm256_add_epi64(acc_hi, _mm256_cvtepi32_epi64(sq_hi));                             \
    acc32 = _mm256_setzero_si256();                                                               \
  } while (0)

  int pending = 0;
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    const __m128i packed = _mm_loadu_si128((const __m128i *)(const void *)(block + (size_t)d * X_SCORE_LANES));
    const __m256i values = _mm256_cvtepi16_epi32(packed);
    const __m256i diff = _mm256_sub_epi32(values, ctx->q32[d]);
    const __m256i sq = _mm256_mullo_epi32(diff, diff);
    acc32 = _mm256_add_epi32(acc32, sq);

    pending++;
    if (pending == 4) {
      DETECTOR_FLUSH_ACC32();
      pending = 0;
    }
  }

  if (pending != 0) {
    DETECTOR_FLUSH_ACC32();
  }

#undef DETECTOR_FLUSH_ACC32

  _mm256_storeu_si256((__m256i *)(void *)out_dist, acc_lo);
  _mm256_storeu_si256((__m256i *)(void *)(out_dist + 4), acc_hi);
#elif defined(__ARM_NEON__)
  int64x2_t acc0 = vdupq_n_s64(0);
  int64x2_t acc1 = vdupq_n_s64(0);
  int64x2_t acc2 = vdupq_n_s64(0);
  int64x2_t acc3 = vdupq_n_s64(0);
  int32x4_t acc_lo32 = vdupq_n_s32(0);
  int32x4_t acc_hi32 = vdupq_n_s32(0);

#define DETECTOR_FLUSH_ACC32()                                                                    \
  do {                                                                                           \
    acc0 = vaddq_s64(acc0, vmovl_s32(vget_low_s32(acc_lo32)));                                   \
    acc1 = vaddq_s64(acc1, vmovl_s32(vget_high_s32(acc_lo32)));                                  \
    acc2 = vaddq_s64(acc2, vmovl_s32(vget_low_s32(acc_hi32)));                                   \
    acc3 = vaddq_s64(acc3, vmovl_s32(vget_high_s32(acc_hi32)));                                  \
    acc_lo32 = vdupq_n_s32(0);                                                                    \
    acc_hi32 = vdupq_n_s32(0);                                                                    \
  } while (0)

  int pending = 0;
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    const int16x8_t v = vld1q_s16(block + (size_t)d * X_SCORE_LANES);
    const int16x8_t diff = vsubq_s16(v, ctx->q16x8[d]);
    acc_lo32 = vaddq_s32(acc_lo32, vmull_s16(vget_low_s16(diff), vget_low_s16(diff)));
    acc_hi32 = vaddq_s32(acc_hi32, vmull_s16(vget_high_s16(diff), vget_high_s16(diff)));

    pending++;
    if (pending == 4) {
      DETECTOR_FLUSH_ACC32();
      pending = 0;
    }
  }

  if (pending != 0) {
    DETECTOR_FLUSH_ACC32();
  }

#undef DETECTOR_FLUSH_ACC32

  int64_t tmp0[2];
  int64_t tmp1[2];
  int64_t tmp2[2];
  int64_t tmp3[2];
  vst1q_s64(tmp0, acc0);
  vst1q_s64(tmp1, acc1);
  vst1q_s64(tmp2, acc2);
  vst1q_s64(tmp3, acc3);

  out_dist[0] = (unsigned long long)tmp0[0];
  out_dist[1] = (unsigned long long)tmp0[1];
  out_dist[2] = (unsigned long long)tmp1[0];
  out_dist[3] = (unsigned long long)tmp1[1];
  out_dist[4] = (unsigned long long)tmp2[0];
  out_dist[5] = (unsigned long long)tmp2[1];
  out_dist[6] = (unsigned long long)tmp3[0];
  out_dist[7] = (unsigned long long)tmp3[1];
#else
  for (int lane = 0; lane < X_SCORE_LANES; lane++) {
    out_dist[lane] = 0;
  }

  for (int d = 0; d < X_SCORE_DIMS; d++) {
    long long qq = (long long)ctx->q[d];
    const int16_t *dim = block + (size_t)d * X_SCORE_LANES;
    for (int lane = 0; lane < X_SCORE_LANES; lane++) {
      long long diff = qq - (long long)dim[lane];
      out_dist[lane] += (unsigned long long)(diff * diff);
    }
  }
#endif
}

static inline bool scan_blocks_linear(const XScoreIndexView *view, uint32_t start_block, uint32_t len,
                                      const DetectorQuery *ctx,
                                      unsigned long long top_dist[X_SCORE_TOPK],
                                      uint8_t top_label[X_SCORE_TOPK]) {
  if (len == 0) {
    return false;
  }

  const int16_t *vectors = view->vectors_q16;
  const uint8_t *labels = view->labels;
  unsigned long long dists[X_SCORE_LANES];

  uint32_t blocks = (len + X_SCORE_LANES - 1U) / X_SCORE_LANES;
  uint32_t remaining = len;

  for (uint32_t b = 0; b < blocks; b++) {
    uint32_t block_idx = start_block + b;
    const int16_t *block = vectors + (size_t)block_idx * X_SCORE_DIMS * X_SCORE_LANES;

#if defined(__GNUC__)
    if (b + 2U < blocks) {
      const int16_t *next = vectors + (size_t)(start_block + b + 2U) * X_SCORE_DIMS * X_SCORE_LANES;
      __builtin_prefetch(next, 0, 1);
    }
#endif

    scan_block(block, ctx, dists);

    uint32_t lane_count = (remaining >= X_SCORE_LANES) ? X_SCORE_LANES : remaining;
    remaining -= lane_count;

    size_t label_base = (size_t)block_idx * X_SCORE_LANES;
    unsigned long long worst = top_dist[X_SCORE_TOPK - 1];
    for (uint32_t lane = 0; lane < lane_count; lane++) {
      unsigned long long d = dists[lane];
      if (d < worst) {
        topk_insert(d, labels[label_base + lane], top_dist, top_label);
        worst = top_dist[X_SCORE_TOPK - 1];
      }
    }

    if (topk_early_done(top_dist)) {
      return true;
    }
  }

  return false;
}

static inline bool scan_partition_leaf(const XScoreIndexView *view, uint32_t partition_index,
                                       const DetectorQuery *ctx,
                                       unsigned long long top_dist[X_SCORE_TOPK],
                                       uint8_t top_label[X_SCORE_TOPK]) {
  if (!view->partition_start_blocks || !view->partition_lens || partition_index >= view->partition_count) {
    return false;
  }

  uint32_t start_block = view->partition_start_blocks[partition_index];
  uint32_t len = view->partition_lens[partition_index];
  if (len == 0) {
    return false;
  }

  return scan_blocks_linear(view, start_block, len, ctx, top_dist, top_label);
}

static inline bool search_tree_iterative(const XScoreIndexView *view, uint32_t root,
                                         unsigned long long root_bound,
                                         const DetectorQuery *ctx,
                                         unsigned long long top_dist[X_SCORE_TOPK],
                                         uint8_t top_label[X_SCORE_TOPK]) {
  if (!view->nodes || root >= view->node_count) {
    return false;
  }

  const XScoreNodeEntry *nodes = view->nodes;
  NodeCandidate stack[256];
  uint32_t stack_len = 0;

  uint32_t current = root;
  unsigned long long current_bound = root_bound;

  for (;;) {
    unsigned long long worst = top_dist[X_SCORE_TOPK - 1];
    if (current_bound <= worst) {
      const XScoreNodeEntry *node = &nodes[current];

      if (node->left < 0 || node->right < 0) {
        if (node->start_block >= 0 && node->len > 0) {
          if (scan_blocks_linear(view, (uint32_t)node->start_block, (uint32_t)node->len, ctx,
                                 top_dist, top_label)) {
            return true;
          }
        }
      } else {
        uint32_t left = (uint32_t)node->left;
        uint32_t right = (uint32_t)node->right;
        if (left < view->node_count && right < view->node_count) {
          unsigned long long lb = lower_bound_node_sq_cutoff(ctx->q, &nodes[left], worst);
          unsigned long long rb = lower_bound_node_sq_cutoff(ctx->q, &nodes[right], worst);

          uint32_t near_idx = left;
          uint32_t far_idx = right;
          unsigned long long near_bound = lb;
          unsigned long long far_bound = rb;

          if (rb < lb) {
            near_idx = right;
            far_idx = left;
            near_bound = rb;
            far_bound = lb;
          }

          if (far_bound <= worst && stack_len < (uint32_t)(sizeof(stack) / sizeof(stack[0]))) {
            stack[stack_len].node_index = far_idx;
            stack[stack_len].bound = far_bound;
            stack_len++;
          }

          if (near_bound <= worst) {
            current = near_idx;
            current_bound = near_bound;
            continue;
          }
        }
      }
    }

    if (stack_len == 0) {
      break;
    }

    stack_len--;
    current = stack[stack_len].node_index;
    current_bound = stack[stack_len].bound;
  }

  return false;
}

static inline void search_exact(const XScoreIndexView *view, const DetectorQuery *ctx,
                                unsigned long long top_dist[X_SCORE_TOPK],
                                uint8_t top_label[X_SCORE_TOPK]) {
  const int16_t *vectors = view->vectors_q16;
  const uint8_t *labels = view->labels;
  unsigned long long dists[X_SCORE_LANES];

  size_t remaining = view->count;
  for (uint32_t b = 0; b < view->block_count && remaining > 0; b++) {
    const int16_t *block = vectors + (size_t)b * X_SCORE_DIMS * X_SCORE_LANES;
    scan_block(block, ctx, dists);

    uint32_t lane_count = (remaining >= X_SCORE_LANES) ? X_SCORE_LANES : (uint32_t)remaining;
    size_t label_base = (size_t)b * X_SCORE_LANES;
    for (uint32_t lane = 0; lane < lane_count; lane++) {
      topk_insert(dists[lane], labels[label_base + lane], top_dist, top_label);
    }

    remaining -= lane_count;
  }
}

static inline bool size_advance_ok(size_t total, size_t *offset, size_t amount) {
  if (*offset > total || amount > (total - *offset)) {
    return false;
  }
  *offset += amount;
  return true;
}

bool x_score_open(const char *path, XScoreIndexView *out_view) {
  if (!path || !out_view) {
    return false;
  }

  memset(out_view, 0, sizeof(*out_view));
  for (int i = 0; i < 256; i++) {
    out_view->key_partition_direct[i] = -1;
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return false;
  }

  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return false;
  }

  size_t size = (size_t)st.st_size;
  uint8_t *raw = (uint8_t *)mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (raw == MAP_FAILED) {
    return false;
  }

  if (size < sizeof(XScoreIndexHeader)) {
    munmap(raw, size);
    return false;
  }

  const XScoreIndexHeader *header = (const XScoreIndexHeader *)raw;
  if (memcmp(header->magic, X_SCORE_MAGIC, 8) != 0 || header->scale != X_SCORE_SCALE ||
      header->dims != X_SCORE_DIMS || header->count < 0 || header->partition_count < 0 ||
      header->node_count < 0 || header->block_count < 0) {
    munmap(raw, size);
    return false;
  }

  uint32_t count = (uint32_t)header->count;
  uint32_t partition_count = (uint32_t)header->partition_count;
  uint32_t node_count = (uint32_t)header->node_count;
  uint32_t block_count = (uint32_t)header->block_count;

  size_t offset = sizeof(XScoreIndexHeader);
  size_t partitions_bytes = (size_t)partition_count * sizeof(XScorePartitionEntry);
  size_t nodes_bytes = (size_t)node_count * sizeof(XScoreNodeEntry);
  size_t vectors_len = (size_t)block_count * X_SCORE_DIMS * X_SCORE_LANES;
  size_t vectors_bytes = vectors_len * sizeof(int16_t);
  size_t labels_bytes = (size_t)block_count * X_SCORE_LANES;

  if (!size_advance_ok(size, &offset, partitions_bytes)) {
    munmap(raw, size);
    return false;
  }
  const XScorePartitionEntry *partitions = (const XScorePartitionEntry *)(raw + sizeof(XScoreIndexHeader));

  size_t nodes_off = sizeof(XScoreIndexHeader) + partitions_bytes;
  if (!size_advance_ok(size, &offset, nodes_bytes)) {
    munmap(raw, size);
    return false;
  }
  const XScoreNodeEntry *nodes = (const XScoreNodeEntry *)(raw + nodes_off);

  size_t vectors_off = nodes_off + nodes_bytes;
  if (!size_advance_ok(size, &offset, vectors_bytes)) {
    munmap(raw, size);
    return false;
  }
  const int16_t *vectors_q16 = (const int16_t *)(raw + vectors_off);

  size_t labels_off = vectors_off + vectors_bytes;
  if (!size_advance_ok(size, &offset, labels_bytes)) {
    munmap(raw, size);
    return false;
  }
  const uint8_t *labels = raw + labels_off;

  if ((size_t)count > labels_bytes) {
    munmap(raw, size);
    return false;
  }

  uint32_t *key_partition_indices = NULL;
  if (partition_count > 0) {
    key_partition_indices = (uint32_t *)malloc((size_t)partition_count * sizeof(uint32_t));
    if (key_partition_indices == NULL) {
      munmap(raw, size);
      return false;
    }
  }

  uint32_t key_counts[256];
  memset(key_counts, 0, sizeof(key_counts));
  for (uint32_t i = 0; i < partition_count; i++) {
    uint32_t key = partitions[i].key & 255u;
    key_counts[key]++;
  }

  uint32_t running = 0;
  for (uint32_t k = 0; k < 256; k++) {
    out_view->key_partition_offsets[k] = running;
    running += key_counts[k];
  }
  out_view->key_partition_offsets[256] = running;

  uint32_t key_pos[256];
  memcpy(key_pos, out_view->key_partition_offsets, sizeof(key_pos));

  for (uint32_t i = 0; i < partition_count; i++) {
    uint32_t key = partitions[i].key & 255u;
    key_partition_indices[key_pos[key]++] = i;

    if (out_view->key_partition_direct[key] == -1) {
      out_view->key_partition_direct[key] = (int32_t)i;
    } else {
      out_view->key_partition_direct[key] = -2;
    }
  }

  // Fast path layout: each partition root is a direct leaf scan region.
  bool direct_leaf_mode = (partition_count > 0 && nodes != NULL);
  uint32_t *partition_start_blocks = NULL;
  uint32_t *partition_lens = NULL;

  if (direct_leaf_mode) {
    partition_start_blocks = (uint32_t *)malloc((size_t)partition_count * sizeof(uint32_t));
    partition_lens = (uint32_t *)malloc((size_t)partition_count * sizeof(uint32_t));

    if (!partition_start_blocks || !partition_lens) {
      direct_leaf_mode = false;
    } else {
      for (uint32_t i = 0; i < partition_count; i++) {
        int32_t root = partitions[i].root;
        if (root < 0 || (uint32_t)root >= node_count) {
          direct_leaf_mode = false;
          break;
        }

        const XScoreNodeEntry *node = &nodes[(uint32_t)root];
        if (node->left >= 0 || node->right >= 0 || node->start_block < 0 || node->len < 0) {
          direct_leaf_mode = false;
          break;
        }

        uint32_t start_block = (uint32_t)node->start_block;
        uint32_t plen = (uint32_t)node->len;
        uint32_t blocks = (plen + X_SCORE_LANES - 1U) / X_SCORE_LANES;
        if ((size_t)start_block + (size_t)blocks > (size_t)block_count) {
          direct_leaf_mode = false;
          break;
        }

        partition_start_blocks[i] = start_block;
        partition_lens[i] = plen;
      }
    }
  }

  if (!direct_leaf_mode) {
    free(partition_start_blocks);
    free(partition_lens);
    partition_start_blocks = NULL;
    partition_lens = NULL;
  }

  out_view->raw = raw;
  out_view->size = size;
  out_view->mapped = 1;
  out_view->header = header;
  out_view->partitions = partitions;
  out_view->nodes = nodes;
  out_view->vectors_q16 = vectors_q16;
  out_view->labels = labels;

  out_view->key_partition_indices = key_partition_indices;
  out_view->partition_start_blocks = partition_start_blocks;
  out_view->partition_lens = partition_lens;
  out_view->partition_group_offsets = NULL;
  out_view->groups = NULL;
  out_view->total_groups = 0;
  out_view->max_groups_per_partition = 0;
  out_view->direct_leaf_mode = direct_leaf_mode ? 1 : 0;

  out_view->count = count;
  out_view->partition_count = partition_count;
  out_view->node_count = node_count;
  out_view->block_count = block_count;

#ifdef MADV_HUGEPAGE
  (void)madvise(raw + vectors_off, labels_off + labels_bytes - vectors_off, MADV_HUGEPAGE);
#endif
#ifdef MADV_WILLNEED
  (void)madvise(raw + vectors_off, labels_off + labels_bytes - vectors_off, MADV_WILLNEED);
#endif

  // Pre-fault mapped pages to reduce first-request latency variance.
  {
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < size; i += 4096) {
      sink ^= raw[i];
    }
    sink ^= raw[size - 1];
    (void)sink;
  }

  return true;
}

void x_score_close(XScoreIndexView *view) {
  if (!view) {
    return;
  }

  free(view->groups);
  free(view->partition_group_offsets);
  free(view->partition_lens);
  free(view->partition_start_blocks);
  free(view->key_partition_indices);

  if (view->mapped && view->raw && view->size > 0) {
    munmap(view->raw, view->size);
  }

  memset(view, 0, sizeof(*view));
}

uint8_t x_score_predict_fraud_count(const XScoreIndexView *view, const double query[X_SCORE_DIMS]) {
  if (!view || !query || !view->header || !view->vectors_q16 || !view->labels || view->count == 0) {
    return 0;
  }

  DetectorQuery qctx;
  prepare_query(query, &qctx);

  unsigned long long top_dist[X_SCORE_TOPK];
  uint8_t top_label[X_SCORE_TOPK];
  topk_reset(top_dist, top_label);

  if (view->partition_count == 0 || !view->partitions || !view->key_partition_indices ||
      view->partition_count > 256U) {
    search_exact(view, &qctx, top_dist, top_label);
  } else if (view->direct_leaf_mode && view->partition_start_blocks && view->partition_lens) {
    uint32_t qkey = compute_partition_key(qctx.q) & 255u;
    int32_t direct = view->key_partition_direct[qkey];

    if (direct >= 0) {
      (void)scan_partition_leaf(view, (uint32_t)direct, &qctx, top_dist, top_label);
    } else {
      uint32_t begin = view->key_partition_offsets[qkey];
      uint32_t end = view->key_partition_offsets[qkey + 1];
      for (uint32_t pos = begin; pos < end && !topk_early_done(top_dist); pos++) {
        uint32_t pidx = view->key_partition_indices[pos];
        (void)scan_partition_leaf(view, pidx, &qctx, top_dist, top_label);
      }
    }

    PartitionCandidate candidates[256];
    uint32_t candidate_len = 0;
    unsigned long long cutoff = top_dist[X_SCORE_TOPK - 1];

    for (uint32_t i = 0; i < view->partition_count && !topk_early_done(top_dist); i++) {
      const XScorePartitionEntry *p = &view->partitions[i];
      if ((p->key & 255u) == qkey) {
        continue;
      }

      unsigned long long bound = lower_bound_partition_sq_cutoff(qctx.q, p, cutoff);
      if (bound < cutoff) {
        insert_partition_candidate_sorted(candidates, &candidate_len, bound, i);
      }
    }

    for (uint32_t i = 0; i < candidate_len && !topk_early_done(top_dist); i++) {
      if (candidates[i].bound >= top_dist[X_SCORE_TOPK - 1]) {
        break;
      }
      (void)scan_partition_leaf(view, candidates[i].partition_index, &qctx, top_dist, top_label);
    }
  } else if (view->nodes) {
    uint32_t qkey = compute_partition_key(qctx.q) & 255u;
    int32_t direct = view->key_partition_direct[qkey];

    if (direct >= 0) {
      const XScorePartitionEntry *p = &view->partitions[(uint32_t)direct];
      if (p->root >= 0) {
        (void)search_tree_iterative(view, (uint32_t)p->root, 0, &qctx, top_dist, top_label);
      }
    } else {
      uint32_t begin = view->key_partition_offsets[qkey];
      uint32_t end = view->key_partition_offsets[qkey + 1];
      for (uint32_t pos = begin; pos < end && !topk_early_done(top_dist); pos++) {
        uint32_t pidx = view->key_partition_indices[pos];
        const XScorePartitionEntry *p = &view->partitions[pidx];
        if (p->root < 0) {
          continue;
        }
        (void)search_tree_iterative(view, (uint32_t)p->root, 0, &qctx, top_dist, top_label);
      }
    }

    PartitionCandidate candidates[256];
    uint32_t candidate_len = 0;
    unsigned long long cutoff = top_dist[X_SCORE_TOPK - 1];

    for (uint32_t i = 0; i < view->partition_count && !topk_early_done(top_dist); i++) {
      const XScorePartitionEntry *p = &view->partitions[i];
      if ((p->key & 255u) == qkey || p->root < 0) {
        continue;
      }

      unsigned long long bound = lower_bound_partition_sq_cutoff(qctx.q, p, cutoff);
      if (bound < cutoff) {
        insert_partition_candidate_sorted(candidates, &candidate_len, bound, i);
      }
    }

    for (uint32_t i = 0; i < candidate_len && !topk_early_done(top_dist); i++) {
      if (candidates[i].bound >= top_dist[X_SCORE_TOPK - 1]) {
        break;
      }

      const XScorePartitionEntry *p = &view->partitions[candidates[i].partition_index];
      (void)search_tree_iterative(view, (uint32_t)p->root, candidates[i].bound, &qctx,
                                  top_dist, top_label);
    }
  } else {
    search_exact(view, &qctx, top_dist, top_label);
  }

  uint8_t fraud_count = 0;
  for (int i = 0; i < X_SCORE_TOPK; i++) {
    if (top_dist[i] == ULLONG_MAX) {
      continue;
    }
    if (top_label[i] == 1) {
      fraud_count++;
    }
  }

  return fraud_count;
}

uint8_t x_score_predict_label(const XScoreIndexView *view, const double query[X_SCORE_DIMS]) {
  return (x_score_predict_fraud_count(view, query) >= 3) ? 1 : 0;
}
