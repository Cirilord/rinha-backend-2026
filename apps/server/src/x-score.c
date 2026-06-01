#include "x-score.h"

#include <errno.h>
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
  uint32_t index;
  unsigned long long dist;
} ProbeCandidate;

typedef struct {
  uint32_t index;
  unsigned long long lb;
} RepairCandidate;

typedef struct {
  int16_t q[X_SCORE_DIMS];
#if defined(__AVX2__)
  __m256i q32[X_SCORE_DIMS];
  __m256i qpair[X_SCORE_DIMS / 2];
#elif defined(__ARM_NEON__)
  int16x8_t q16x8[X_SCORE_DIMS];
#endif
} XScoreQueryContext;

#define X_SCORE_MAX_NPROBE 256U
#define X_SCORE_DIM_PAIRS (X_SCORE_DIMS / 2)
#define X_SCORE_REPAIR_CANDIDATE_CAP 1024U

static int16_t quantize_value(double value) {
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
  if (scaled >= 0.0) {
    scaled += 0.5;
  } else {
    scaled -= 0.5;
  }
  return (int16_t)scaled;
}

#if defined(__AVX2__)
static inline __m256i make_qpair(const int16_t q[X_SCORE_DIMS], int pair_idx) {
  uint32_t lo = (uint32_t)(uint16_t)q[pair_idx * 2];
  uint32_t hi = (uint32_t)(uint16_t)q[pair_idx * 2 + 1];
  return _mm256_set1_epi32((int32_t)(lo | (hi << 16)));
}
#endif

static inline bool is_borderline_query(const int16_t q[X_SCORE_DIMS]) {
  bool safe_mcc = (q[12] == 1500 || q[12] == 3000 || q[12] == 2000 || q[12] == 2500);
  bool obvious_legit = (q[0] <= 500 && q[2] <= 500 && q[1] <= 2500 && q[8] <= 2500 && q[7] <= 500 &&
                        safe_mcc && q[11] == 0);
  if (obvious_legit) {
    return false;
  }

  bool risky_mcc = (q[12] == 8500 || q[12] == 8000 || q[12] == 7500);
  bool obvious_fraud =
    (q[0] >= 5000 && q[1] >= 4167 && q[8] >= 3000 && q[7] >= 1500 && risky_mcc && q[11] == 10000);

  if (obvious_fraud) {
    return false;
  }

  return true;
}

static uint32_t parse_env_u32(const char *name, uint32_t default_value, bool allow_zero) {
  const char *env = getenv(name);
  if (env == NULL || *env == '\0') {
    return default_value;
  }

  errno = 0;
  char *end = NULL;
  unsigned long parsed = strtoul(env, &end, 10);
  if (errno != 0 || end == env || *end != '\0') {
    return default_value;
  }

  if (parsed > (unsigned long)UINT32_MAX) {
    return default_value;
  }

  uint32_t out = (uint32_t)parsed;
  if (out == 0 && !allow_zero) {
    return default_value;
  }

  return out;
}

static inline void prepare_query_context(const double query[X_SCORE_DIMS], XScoreQueryContext *ctx) {
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    int16_t qd = quantize_value(query[d]);
    ctx->q[d] = qd;
#if defined(__AVX2__)
    ctx->q32[d] = _mm256_set1_epi32((int)qd);
#elif defined(__ARM_NEON__)
    ctx->q16x8[d] = vdupq_n_s16(qd);
#endif
  }

#if defined(__AVX2__)
  for (int p = 0; p < X_SCORE_DIM_PAIRS; p++) {
    ctx->qpair[p] = make_qpair(ctx->q, p);
  }
#endif
}

static inline void insert_best(unsigned long long dist, uint8_t label,
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

static inline void scan_block(const int16_t *block, const XScoreQueryContext *ctx,
                              unsigned long long out_dist[X_SCORE_LANES]) {
#if defined(__AVX2__)
  __m256i acc_lo = _mm256_setzero_si256();
  __m256i acc_hi = _mm256_setzero_si256();
  __m256i acc32 = _mm256_setzero_si256();

#define X_SCORE_FLUSH_ACC32()                                                                      \
  do {                                                                                             \
    const __m128i sq_lo = _mm256_castsi256_si128(acc32);                                           \
    const __m128i sq_hi = _mm256_extracti128_si256(acc32, 1);                                      \
    acc_lo = _mm256_add_epi64(acc_lo, _mm256_cvtepi32_epi64(sq_lo));                               \
    acc_hi = _mm256_add_epi64(acc_hi, _mm256_cvtepi32_epi64(sq_hi));                               \
    acc32 = _mm256_setzero_si256();                                                                 \
  } while (0)

  for (int d = 0; d < 5; d++) {
    const __m128i packed = _mm_loadu_si128((const __m128i *)(const void *)(block + (size_t)d * 8));
    const __m256i values = _mm256_cvtepi16_epi32(packed);
    const __m256i diff = _mm256_sub_epi32(values, ctx->q32[d]);
    const __m256i sq = _mm256_mullo_epi32(diff, diff);
    acc32 = _mm256_add_epi32(acc32, sq);
  }
  X_SCORE_FLUSH_ACC32();

  for (int d = 5; d < 10; d++) {
    const __m128i packed = _mm_loadu_si128((const __m128i *)(const void *)(block + (size_t)d * 8));
    const __m256i values = _mm256_cvtepi16_epi32(packed);
    const __m256i diff = _mm256_sub_epi32(values, ctx->q32[d]);
    const __m256i sq = _mm256_mullo_epi32(diff, diff);
    acc32 = _mm256_add_epi32(acc32, sq);
  }
  X_SCORE_FLUSH_ACC32();

  for (int d = 10; d < X_SCORE_DIMS; d++) {
    const __m128i packed = _mm_loadu_si128((const __m128i *)(const void *)(block + (size_t)d * 8));
    const __m256i values = _mm256_cvtepi16_epi32(packed);
    const __m256i diff = _mm256_sub_epi32(values, ctx->q32[d]);
    const __m256i sq = _mm256_mullo_epi32(diff, diff);
    acc32 = _mm256_add_epi32(acc32, sq);
  }
  X_SCORE_FLUSH_ACC32();

#undef X_SCORE_FLUSH_ACC32

  _mm256_storeu_si256((__m256i *)(void *)out_dist, acc_lo);
  _mm256_storeu_si256((__m256i *)(void *)(out_dist + 4), acc_hi);
#elif defined(__ARM_NEON__)
  int64x2_t acc0 = vdupq_n_s64(0);
  int64x2_t acc1 = vdupq_n_s64(0);
  int64x2_t acc2 = vdupq_n_s64(0);
  int64x2_t acc3 = vdupq_n_s64(0);
  int32x4_t acc_lo32 = vdupq_n_s32(0);
  int32x4_t acc_hi32 = vdupq_n_s32(0);

#define X_SCORE_FLUSH_ACC32()                                                                      \
  do {                                                                                             \
    acc0 = vaddq_s64(acc0, vmovl_s32(vget_low_s32(acc_lo32)));                                     \
    acc1 = vaddq_s64(acc1, vmovl_s32(vget_high_s32(acc_lo32)));                                    \
    acc2 = vaddq_s64(acc2, vmovl_s32(vget_low_s32(acc_hi32)));                                     \
    acc3 = vaddq_s64(acc3, vmovl_s32(vget_high_s32(acc_hi32)));                                    \
    acc_lo32 = vdupq_n_s32(0);                                                                      \
    acc_hi32 = vdupq_n_s32(0);                                                                      \
  } while (0)

  for (int d = 0; d < 5; d++) {
    const int16x8_t v = vld1q_s16(block + (size_t)d * 8);
    const int16x8_t diff = vsubq_s16(v, ctx->q16x8[d]);
    acc_lo32 = vaddq_s32(acc_lo32, vmull_s16(vget_low_s16(diff), vget_low_s16(diff)));
    acc_hi32 = vaddq_s32(acc_hi32, vmull_s16(vget_high_s16(diff), vget_high_s16(diff)));
  }
  X_SCORE_FLUSH_ACC32();

  for (int d = 5; d < 10; d++) {
    const int16x8_t v = vld1q_s16(block + (size_t)d * 8);
    const int16x8_t diff = vsubq_s16(v, ctx->q16x8[d]);
    acc_lo32 = vaddq_s32(acc_lo32, vmull_s16(vget_low_s16(diff), vget_low_s16(diff)));
    acc_hi32 = vaddq_s32(acc_hi32, vmull_s16(vget_high_s16(diff), vget_high_s16(diff)));
  }
  X_SCORE_FLUSH_ACC32();

  for (int d = 10; d < X_SCORE_DIMS; d++) {
    const int16x8_t v = vld1q_s16(block + (size_t)d * 8);
    const int16x8_t diff = vsubq_s16(v, ctx->q16x8[d]);
    acc_lo32 = vaddq_s32(acc_lo32, vmull_s16(vget_low_s16(diff), vget_low_s16(diff)));
    acc_hi32 = vaddq_s32(acc_hi32, vmull_s16(vget_high_s16(diff), vget_high_s16(diff)));
  }
  X_SCORE_FLUSH_ACC32();

#undef X_SCORE_FLUSH_ACC32

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

static inline unsigned long long centroid_distance_sq(const int16_t q[X_SCORE_DIMS],
                                                      const int16_t *centroid) {
  unsigned long long sum = 0;
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    long long diff = (long long)q[d] - (long long)centroid[d];
    sum += (unsigned long long)(diff * diff);
  }
  return sum;
}

static inline unsigned long long lower_bound_list_sq_cutoff(const int16_t q[X_SCORE_DIMS],
                                                             const XScoreListEntry *list,
                                                             unsigned long long cutoff) {
  unsigned long long sum = 0;
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    long long qq = (long long)q[d];
    long long lo = (long long)list->min[d];
    long long hi = (long long)list->max[d];
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

static inline void insert_probe(ProbeCandidate *probes, uint32_t *probe_len, uint32_t probe_cap,
                                unsigned long long dist, uint32_t index) {
  if (probe_cap == 0) {
    return;
  }

  if (*probe_len == probe_cap && dist >= probes[probe_cap - 1].dist) {
    return;
  }

  uint32_t pos = (*probe_len < probe_cap) ? *probe_len : (probe_cap - 1);
  while (pos > 0 && probes[pos - 1].dist > dist) {
    probes[pos] = probes[pos - 1];
    pos--;
  }
  probes[pos].dist = dist;
  probes[pos].index = index;
  if (*probe_len < probe_cap) {
    (*probe_len)++;
  }
}

static inline uint32_t count_top_labels(const unsigned long long top_dist[X_SCORE_TOPK],
                                        const uint8_t top_label[X_SCORE_TOPK]) {
  uint32_t count = 0;
  for (int i = 0; i < X_SCORE_TOPK; i++) {
    if (top_dist[i] == ULLONG_MAX) {
      continue;
    }
    count += (uint32_t)top_label[i];
  }
  return count;
}

static inline bool probe_contains(const ProbeCandidate *probes, uint32_t probe_len, uint32_t index) {
  for (uint32_t i = 0; i < probe_len; i++) {
    if (probes[i].index == index) {
      return true;
    }
  }
  return false;
}

static inline void insert_repair_candidate_sorted(RepairCandidate *cands, uint32_t *cand_len,
                                                  uint32_t cap, unsigned long long lb,
                                                  uint32_t index) {
  if (cap == 0) {
    return;
  }

  if (*cand_len == cap && lb >= cands[cap - 1].lb) {
    return;
  }

  uint32_t pos = (*cand_len < cap) ? *cand_len : (cap - 1);
  while (pos > 0 && cands[pos - 1].lb > lb) {
    cands[pos] = cands[pos - 1];
    pos--;
  }
  cands[pos].lb = lb;
  cands[pos].index = index;

  if (*cand_len < cap) {
    (*cand_len)++;
  }
}

static uint32_t parse_nprobe(uint32_t centroid_count) {
  uint32_t nprobe = parse_env_u32("X_SCORE_NPROBE", X_SCORE_DEFAULT_NPROBE, false);

  if (nprobe > centroid_count) {
    nprobe = centroid_count;
  }
  if (nprobe == 0) {
    nprobe = 1;
  }
  if (nprobe > X_SCORE_MAX_NPROBE) {
    nprobe = X_SCORE_MAX_NPROBE;
  }

  return nprobe;
}

static uint32_t parse_nprobe_borderline(uint32_t centroid_count) {
  uint32_t nprobe =
    parse_env_u32("X_SCORE_NPROBE_BORDERLINE", X_SCORE_DEFAULT_NPROBE_BORDERLINE, true);

  if (nprobe == 0) {
    return 0;
  }

  if (nprobe > centroid_count) {
    nprobe = centroid_count;
  }
  if (nprobe > X_SCORE_MAX_NPROBE) {
    nprobe = X_SCORE_MAX_NPROBE;
  }

  return nprobe;
}

static void parse_repair_window(uint32_t *out_min, uint32_t *out_max) {
  uint32_t minv = parse_env_u32("X_SCORE_REPAIR_MIN", X_SCORE_DEFAULT_REPAIR_MIN, true);
  uint32_t maxv = parse_env_u32("X_SCORE_REPAIR_MAX", X_SCORE_DEFAULT_REPAIR_MAX, true);

  if (minv == 0 || maxv == 0) {
    *out_min = 0;
    *out_max = 0;
    return;
  }

  if (minv > X_SCORE_TOPK) {
    minv = X_SCORE_TOPK;
  }
  if (maxv > X_SCORE_TOPK) {
    maxv = X_SCORE_TOPK;
  }

  if (minv > maxv) {
    uint32_t tmp = minv;
    minv = maxv;
    maxv = tmp;
  }

  *out_min = minv;
  *out_max = maxv;
}

static bool build_centroid_pair_soa(XScoreIndexView *view) {
  view->centroid_pair_soa = NULL;
  view->centroid_groups = 0;

#if !defined(__AVX2__)
  (void)view;
  return true;
#else
  if (view->centroid_count == 0 || view->centroids_q16 == NULL) {
    return false;
  }

  uint32_t groups = (view->centroid_count + X_SCORE_LANES - 1U) / X_SCORE_LANES;
  size_t elem_count = (size_t)groups * X_SCORE_DIM_PAIRS * (X_SCORE_LANES * 2U);
  if (elem_count == 0 || elem_count > (SIZE_MAX / sizeof(int16_t))) {
    return false;
  }

  int16_t *soa = (int16_t *)malloc(elem_count * sizeof(int16_t));
  if (soa == NULL) {
    return false;
  }

  memset(soa, 0, elem_count * sizeof(int16_t));

  for (uint32_t g = 0; g < groups; g++) {
    for (int p = 0; p < X_SCORE_DIM_PAIRS; p++) {
      int16_t *dst = soa + ((size_t)g * X_SCORE_DIM_PAIRS + (size_t)p) * (X_SCORE_LANES * 2U);
      for (uint32_t lane = 0; lane < X_SCORE_LANES; lane++) {
        uint32_t cidx = g * X_SCORE_LANES + lane;
        if (cidx >= view->centroid_count) {
          continue;
        }

        const int16_t *src = view->centroids_q16 + (size_t)cidx * X_SCORE_DIMS;
        dst[(size_t)lane * 2U] = src[p * 2];
        dst[(size_t)lane * 2U + 1U] = src[p * 2 + 1U];
      }
    }
  }

  view->centroid_pair_soa = soa;
  view->centroid_groups = groups;
  return true;
#endif
}

static void find_top_centroids(const XScoreIndexView *view, const XScoreQueryContext *ctx,
                               ProbeCandidate *probes, uint32_t *probe_len, uint32_t probe_cap) {
  *probe_len = 0;

#if defined(__AVX2__)
  if (view->centroid_pair_soa != NULL && view->centroid_groups > 0) {
    for (uint32_t g = 0; g < view->centroid_groups; g++) {
      const int16_t *src = view->centroid_pair_soa +
                           (size_t)g * X_SCORE_DIM_PAIRS * (X_SCORE_LANES * 2U);
      __m256i acc = _mm256_setzero_si256();

      for (int p = 0; p < X_SCORE_DIM_PAIRS; p++) {
        __m256i vc = _mm256_loadu_si256((const __m256i *)(const void *)(src + p * 16));
        __m256i diff = _mm256_sub_epi16(ctx->qpair[p], vc);
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(diff, diff));
      }

      int32_t dists[8];
      _mm256_storeu_si256((__m256i *)(void *)dists, acc);

      uint32_t base = g * X_SCORE_LANES;
      uint32_t lim = view->centroid_count - base;
      if (lim > X_SCORE_LANES) {
        lim = X_SCORE_LANES;
      }

      for (uint32_t lane = 0; lane < lim; lane++) {
        insert_probe(probes, probe_len, probe_cap, (unsigned long long)(uint32_t)dists[lane], base + lane);
      }
    }
    return;
  }
#endif

  for (uint32_t cidx = 0; cidx < view->centroid_count; cidx++) {
    const int16_t *centroid = view->centroids_q16 + (size_t)cidx * X_SCORE_DIMS;
    unsigned long long dist = centroid_distance_sq(ctx->q, centroid);
    insert_probe(probes, probe_len, probe_cap, dist, cidx);
  }
}

static inline void scan_list(const XScoreIndexView *view, const XScoreListEntry *list,
                             const XScoreQueryContext *ctx, unsigned long long top_dist[X_SCORE_TOPK],
                             uint8_t top_label[X_SCORE_TOPK]) {
  if (list->len <= 0 || list->block_count <= 0 || list->start_block < 0) {
    return;
  }

  const int16_t *vectors = view->vectors_q16;
  const uint8_t *labels = view->labels;

  uint32_t total_len = (uint32_t)list->len;
  uint32_t total_blocks = (uint32_t)list->block_count;
  uint32_t start_block = (uint32_t)list->start_block;
  uint32_t remaining = total_len;

#if defined(__AVX2__)
  for (uint32_t b = 0; b < total_blocks; b++) {
    uint32_t block_idx = start_block + b;
    const int16_t *block = vectors + (size_t)block_idx * X_SCORE_DIMS * X_SCORE_LANES;
#if defined(__GNUC__)
    if (b + 2 < total_blocks) {
      const int16_t *next = vectors + (size_t)(start_block + b + 2) * X_SCORE_DIMS * X_SCORE_LANES;
      __builtin_prefetch(next, 0, 1);
    }
#endif

    int32_t max_top_i32 = (top_dist[X_SCORE_TOPK - 1] > (unsigned long long)INT32_MAX)
                            ? INT32_MAX
                            : (int32_t)top_dist[X_SCORE_TOPK - 1];
    __m256i vmax = _mm256_set1_epi32(max_top_i32);

    __m256i acc0 = _mm256_setzero_si256();
    __m256i acc1 = _mm256_setzero_si256();

#define X_SCORE_SPAIR(pair_idx, acc)                                                                 \
  do {                                                                                                \
    __m256i vc = _mm256_loadu_si256((const __m256i *)(const void *)(block + (pair_idx) * 16));      \
    __m256i diff = _mm256_sub_epi16(ctx->qpair[(pair_idx)], vc);                                     \
    acc = _mm256_add_epi32(acc, _mm256_madd_epi16(diff, diff));                                      \
  } while (0)

    X_SCORE_SPAIR(0, acc0);
    X_SCORE_SPAIR(1, acc1);
    X_SCORE_SPAIR(2, acc0);

    __m256i partial = _mm256_add_epi32(acc0, acc1);
    if (_mm256_movemask_epi8(_mm256_cmpgt_epi32(vmax, partial)) == 0) {
      uint32_t lane_count = (remaining >= X_SCORE_LANES) ? X_SCORE_LANES : remaining;
      remaining -= lane_count;
      continue;
    }

    X_SCORE_SPAIR(3, acc1);
    X_SCORE_SPAIR(4, acc0);

    partial = _mm256_add_epi32(acc0, acc1);
    if (_mm256_movemask_epi8(_mm256_cmpgt_epi32(vmax, partial)) == 0) {
      uint32_t lane_count = (remaining >= X_SCORE_LANES) ? X_SCORE_LANES : remaining;
      remaining -= lane_count;
      continue;
    }

    X_SCORE_SPAIR(5, acc1);
    X_SCORE_SPAIR(6, acc0);

#undef X_SCORE_SPAIR

    int32_t dists32[8];
    _mm256_storeu_si256((__m256i *)(void *)dists32, _mm256_add_epi32(acc0, acc1));

    uint32_t lane_count = (remaining >= X_SCORE_LANES) ? X_SCORE_LANES : remaining;
    remaining -= lane_count;

    size_t label_base = (size_t)block_idx * X_SCORE_LANES;
    unsigned long long worst = top_dist[X_SCORE_TOPK - 1];
    for (uint32_t lane = 0; lane < lane_count; lane++) {
      unsigned long long d = (unsigned long long)(uint32_t)dists32[lane];
      if (d < worst) {
        insert_best(d, labels[label_base + lane], top_dist, top_label);
        worst = top_dist[X_SCORE_TOPK - 1];
      }
    }

    if (top_dist[X_SCORE_TOPK - 1] == 0) {
      break;
    }
  }
#else
  unsigned long long dists[X_SCORE_LANES];
  for (uint32_t b = 0; b < total_blocks; b++) {
    uint32_t block_idx = start_block + b;
    const int16_t *block = vectors + (size_t)block_idx * X_SCORE_DIMS * X_SCORE_LANES;
#if defined(__GNUC__)
    if (b + 2 < total_blocks) {
      const int16_t *next = vectors + (size_t)(start_block + b + 2) * X_SCORE_DIMS * X_SCORE_LANES;
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
        insert_best(d, labels[label_base + lane], top_dist, top_label);
        worst = top_dist[X_SCORE_TOPK - 1];
      }
    }

    if (top_dist[X_SCORE_TOPK - 1] == 0) {
      break;
    }
  }
#endif
}

static void repair_lists_if_ambiguous(const XScoreIndexView *view, const XScoreQueryContext *ctx,
                                      const ProbeCandidate *probes, uint32_t probe_len,
                                      unsigned long long top_dist[X_SCORE_TOPK],
                                      uint8_t top_label[X_SCORE_TOPK]) {
  if (view->repair_min == 0 || view->repair_max == 0 || view->repair_min > view->repair_max) {
    return;
  }

  uint32_t fraud_count = count_top_labels(top_dist, top_label);
  if (fraud_count < view->repair_min || fraud_count > view->repair_max) {
    return;
  }

  RepairCandidate cands[X_SCORE_REPAIR_CANDIDATE_CAP];
  uint32_t cand_len = 0;
  unsigned long long cutoff = top_dist[X_SCORE_TOPK - 1];

  for (uint32_t cidx = 0; cidx < view->centroid_count; cidx++) {
    if (probe_contains(probes, probe_len, cidx)) {
      continue;
    }

    const XScoreListEntry *list = &view->lists[cidx];
    if (list->len <= 0) {
      continue;
    }

    unsigned long long lb = lower_bound_list_sq_cutoff(ctx->q, list, cutoff);
    if (lb >= cutoff) {
      continue;
    }

    insert_repair_candidate_sorted(cands, &cand_len, X_SCORE_REPAIR_CANDIDATE_CAP, lb, cidx);
  }

  for (uint32_t i = 0; i < cand_len; i++) {
    if (cands[i].lb >= top_dist[X_SCORE_TOPK - 1]) {
      break;
    }

    const XScoreListEntry *list = &view->lists[cands[i].index];
    scan_list(view, list, ctx, top_dist, top_label);

    fraud_count = count_top_labels(top_dist, top_label);
    if (fraud_count < view->repair_min || fraud_count > view->repair_max) {
      break;
    }
  }
}

bool x_score_open(const char *path, XScoreIndexView *out_view) {
  if (!path || !out_view) {
    return false;
  }

  memset(out_view, 0, sizeof(*out_view));

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
  if (memcmp(header->magic, X_SCORE_MAGIC, 8) != 0) {
    munmap(raw, size);
    return false;
  }

  if (header->scale != X_SCORE_SCALE || header->dims != X_SCORE_DIMS) {
    munmap(raw, size);
    return false;
  }

  if (header->count < 0 || header->centroid_count <= 0 || header->block_count < 0) {
    munmap(raw, size);
    return false;
  }

  uint32_t count = (uint32_t)header->count;
  uint32_t centroid_count = (uint32_t)header->centroid_count;
  uint32_t block_count = (uint32_t)header->block_count;

  size_t offset = sizeof(XScoreIndexHeader);
  size_t centroids_bytes = (size_t)centroid_count * X_SCORE_DIMS * sizeof(int16_t);
  size_t lists_bytes = (size_t)centroid_count * sizeof(XScoreListEntry);
  size_t vectors_bytes = (size_t)block_count * X_SCORE_DIMS * X_SCORE_LANES * sizeof(int16_t);
  size_t labels_bytes = (size_t)block_count * X_SCORE_LANES;

  if (offset + centroids_bytes > size) {
    munmap(raw, size);
    return false;
  }
  const int16_t *centroids_q16 = (const int16_t *)(raw + offset);
  offset += centroids_bytes;

  if (offset + lists_bytes > size) {
    munmap(raw, size);
    return false;
  }
  const XScoreListEntry *lists = (const XScoreListEntry *)(raw + offset);
  offset += lists_bytes;

  if (offset + vectors_bytes > size) {
    munmap(raw, size);
    return false;
  }
  const int16_t *vectors_q16 = (const int16_t *)(raw + offset);
  offset += vectors_bytes;

  if (offset + labels_bytes > size) {
    munmap(raw, size);
    return false;
  }
  const uint8_t *labels = raw + offset;

  uint64_t sum_len = 0;
  for (uint32_t i = 0; i < centroid_count; i++) {
    const XScoreListEntry *lst = &lists[i];
    if (lst->start_block < 0 || lst->block_count < 0 || lst->len < 0) {
      munmap(raw, size);
      return false;
    }

    uint32_t sblk = (uint32_t)lst->start_block;
    uint32_t bcnt = (uint32_t)lst->block_count;
    uint32_t llen = (uint32_t)lst->len;

    if ((size_t)sblk + (size_t)bcnt > (size_t)block_count) {
      munmap(raw, size);
      return false;
    }

    uint32_t expected_blocks = (llen + X_SCORE_LANES - 1U) / X_SCORE_LANES;
    if (expected_blocks != bcnt) {
      munmap(raw, size);
      return false;
    }

    sum_len += llen;
  }

  if (sum_len != (uint64_t)count) {
    munmap(raw, size);
    return false;
  }

  out_view->raw = raw;
  out_view->size = size;
  out_view->mapped = 1;
  out_view->header = header;
  out_view->centroids_q16 = centroids_q16;
  out_view->lists = lists;
  out_view->vectors_q16 = vectors_q16;
  out_view->labels = labels;
  out_view->count = count;
  out_view->centroid_count = centroid_count;
  out_view->block_count = block_count;
  out_view->nprobe = parse_nprobe(centroid_count);
  out_view->nprobe_borderline = parse_nprobe_borderline(centroid_count);
  parse_repair_window(&out_view->repair_min, &out_view->repair_max);

  if (!build_centroid_pair_soa(out_view)) {
    x_score_close(out_view);
    return false;
  }

#ifdef MADV_HUGEPAGE
  (void)madvise((void *)vectors_q16, vectors_bytes, MADV_HUGEPAGE);
#endif
#ifdef MADV_WILLNEED
  (void)madvise((void *)vectors_q16, vectors_bytes, MADV_WILLNEED);
#endif

  // Pre-fault mapped pages to reduce first-request latency variance.
  {
    volatile uint8_t sink = 0;
    size_t i = 0;
    for (i = 0; i < size; i += 4096) {
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

  free(view->centroid_pair_soa);

  if (view->mapped && view->raw && view->size > 0) {
    munmap(view->raw, view->size);
  }

  memset(view, 0, sizeof(*view));
}

uint8_t x_score_predict_fraud_count(const XScoreIndexView *view, const double query[X_SCORE_DIMS]) {
  if (!view || !query || !view->header || !view->centroids_q16 || !view->lists || !view->vectors_q16 ||
      !view->labels) {
    return 0;
  }

  if (view->count == 0 || view->centroid_count == 0) {
    return 0;
  }

  XScoreQueryContext qctx;
  prepare_query_context(query, &qctx);

  unsigned long long top_dist[X_SCORE_TOPK];
  uint8_t top_label[X_SCORE_TOPK];
  for (int i = 0; i < X_SCORE_TOPK; i++) {
    top_dist[i] = ULLONG_MAX;
    top_label[i] = 0;
  }

  uint32_t nprobe = view->nprobe;
  if (view->nprobe_borderline > 0 && is_borderline_query(qctx.q)) {
    nprobe = view->nprobe_borderline;
  }

  if (nprobe == 0) {
    nprobe = 1;
  }
  if (nprobe > view->centroid_count) {
    nprobe = view->centroid_count;
  }
  if (nprobe > X_SCORE_MAX_NPROBE) {
    nprobe = X_SCORE_MAX_NPROBE;
  }

  ProbeCandidate probes[X_SCORE_MAX_NPROBE];
  uint32_t probe_len = 0;
  find_top_centroids(view, &qctx, probes, &probe_len, nprobe);

  for (uint32_t i = 0; i < probe_len; i++) {
    const XScoreListEntry *list = &view->lists[probes[i].index];
    if (list->len <= 0) {
      continue;
    }

    unsigned long long lb = lower_bound_list_sq_cutoff(qctx.q, list, top_dist[X_SCORE_TOPK - 1]);
    if (lb >= top_dist[X_SCORE_TOPK - 1]) {
      continue;
    }

    scan_list(view, list, &qctx, top_dist, top_label);
  }

  repair_lists_if_ambiguous(view, &qctx, probes, probe_len, top_dist, top_label);

  uint32_t fraud_count = count_top_labels(top_dist, top_label);
  if (fraud_count > X_SCORE_TOPK) {
    fraud_count = X_SCORE_TOPK;
  }
  return (uint8_t)fraud_count;
}

uint8_t x_score_predict_label(const XScoreIndexView *view, const double query[X_SCORE_DIMS]) {
  uint8_t fraud_count = x_score_predict_fraud_count(view, query);
  return (fraud_count >= 3) ? 1 : 0;
}
