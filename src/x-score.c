#include "x-score.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  unsigned long long bound;
  uint32_t index;
} PartitionCandidate;

typedef struct {
  size_t node_index;
  unsigned long long bound;
} NodeStackEntry;

static int partition_candidate_cmp(const void *a, const void *b) {
  const PartitionCandidate *pa = (const PartitionCandidate *)a;
  const PartitionCandidate *pb = (const PartitionCandidate *)b;
  if (pa->bound < pb->bound) return -1;
  if (pa->bound > pb->bound) return 1;
  return 0;
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
  if (header->count < 0 || header->partition_count < 0 || header->node_count < 0 || header->block_count < 0) {
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

  if (offset + partitions_bytes > size) {
    munmap(raw, size);
    return false;
  }
  const XScorePartitionEntry *partitions = (const XScorePartitionEntry *)(raw + offset);
  offset += partitions_bytes;

  if (offset + nodes_bytes > size) {
    munmap(raw, size);
    return false;
  }
  const XScoreNodeEntry *nodes = (const XScoreNodeEntry *)(raw + offset);
  offset += nodes_bytes;

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
  const uint8_t *labels = (const uint8_t *)(raw + offset);

  if ((size_t)count > labels_bytes) {
    munmap(raw, size);
    return false;
  }

  out_view->raw = raw;
  out_view->size = size;
  out_view->mapped = 1;
  out_view->header = header;
  out_view->partitions = partitions;
  out_view->nodes = nodes;
  out_view->vectors_q16 = vectors_q16;
  out_view->labels = labels;
  out_view->count = count;
  out_view->partition_count = partition_count;
  out_view->node_count = node_count;
  out_view->block_count = block_count;
  return true;
}

void x_score_close(XScoreIndexView *view) {
  if (!view) {
    return;
  }
  if (view->mapped && view->raw && view->size > 0) {
    munmap(view->raw, view->size);
  }
  memset(view, 0, sizeof(*view));
}

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

static uint32_t compute_partition_key(const int16_t q[X_SCORE_DIMS]) {
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

static unsigned long long lower_bound_box_sq(
    const int16_t q[X_SCORE_DIMS],
    const int16_t minv[X_SCORE_DIMS],
    const int16_t maxv[X_SCORE_DIMS]) {
  unsigned long long sum = 0;
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    long long qq = (long long)q[d];
    long long lo = (long long)minv[d];
    long long hi = (long long)maxv[d];
    long long diff = 0;
    if (qq < lo) {
      diff = lo - qq;
    } else if (qq > hi) {
      diff = qq - hi;
    }
    sum += (unsigned long long)(diff * diff);
  }
  return sum;
}

static void insert_best(
    unsigned long long dist,
    uint8_t label,
    unsigned long long top_dist[X_SCORE_TOPK],
    uint8_t top_label[X_SCORE_TOPK]) {
  if (dist >= top_dist[X_SCORE_TOPK - 1]) {
    return;
  }

  int pos = X_SCORE_TOPK - 1;
  while (pos > 0 && dist < top_dist[pos - 1]) {
    top_dist[pos] = top_dist[pos - 1];
    top_label[pos] = top_label[pos - 1];
    pos--;
  }
  top_dist[pos] = dist;
  top_label[pos] = label;
}

static void scan_block(
    const int16_t *vectors,
    size_t block_base,
    const int16_t q[X_SCORE_DIMS],
    unsigned long long out_dist[X_SCORE_LANES]) {
  for (int lane = 0; lane < X_SCORE_LANES; lane++) {
    out_dist[lane] = 0;
  }
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    size_t base = block_base + (size_t)d * X_SCORE_LANES;
    long long qq = (long long)q[d];
    for (int lane = 0; lane < X_SCORE_LANES; lane++) {
      long long diff = qq - (long long)vectors[base + lane];
      out_dist[lane] += (unsigned long long)(diff * diff);
    }
  }
}

static void scan_leaf(
    const XScoreIndexView *view,
    const XScoreNodeEntry *node,
    const int16_t q[X_SCORE_DIMS],
    unsigned long long top_dist[X_SCORE_TOPK],
    uint8_t top_label[X_SCORE_TOPK]) {
  if (!node || node->len <= 0 || node->start_block < 0) {
    return;
  }

  uint32_t leaf_len = (uint32_t)node->len;
  uint32_t start_block = (uint32_t)node->start_block;
  uint32_t blocks = (leaf_len + X_SCORE_LANES - 1U) / X_SCORE_LANES;
  if ((size_t)start_block + (size_t)blocks > (size_t)view->block_count) {
    return;
  }

  const int16_t *vectors = view->vectors_q16;
  const uint8_t *labels = view->labels;
  unsigned long long dists[X_SCORE_LANES];

  for (uint32_t b = 0; b < blocks; b++) {
    uint32_t block_idx = start_block + b;
    size_t block_base = (size_t)block_idx * X_SCORE_DIMS * X_SCORE_LANES;
    scan_block(vectors, block_base, q, dists);

    uint32_t lane_count = X_SCORE_LANES;
    uint32_t processed = b * X_SCORE_LANES;
    if (processed + lane_count > leaf_len) {
      lane_count = leaf_len - processed;
    }

    size_t label_base = (size_t)block_idx * X_SCORE_LANES;
    for (uint32_t lane = 0; lane < lane_count; lane++) {
      insert_best(dists[lane], labels[label_base + lane], top_dist, top_label);
    }
  }
}

static void search_node_iterative(
    const XScoreIndexView *view,
    uint32_t root,
    unsigned long long root_bound,
    const int16_t q[X_SCORE_DIMS],
    unsigned long long top_dist[X_SCORE_TOPK],
    uint8_t top_label[X_SCORE_TOPK]) {
  if (root >= view->node_count) {
    return;
  }

  NodeStackEntry stack[128];
  size_t stack_len = 0;

  uint32_t current = root;
  unsigned long long current_bound = root_bound;

  for (;;) {
    if (current_bound <= top_dist[X_SCORE_TOPK - 1]) {
      const XScoreNodeEntry *node = &view->nodes[current];
      if (node->left < 0 || node->right < 0) {
        scan_leaf(view, node, q, top_dist, top_label);
      } else {
        uint32_t left = (uint32_t)node->left;
        uint32_t right = (uint32_t)node->right;
        if (left >= view->node_count || right >= view->node_count) {
          if (stack_len == 0) break;
          stack_len--;
          current = (uint32_t)stack[stack_len].node_index;
          current_bound = stack[stack_len].bound;
          continue;
        }

        unsigned long long lb =
            lower_bound_box_sq(q, view->nodes[left].min, view->nodes[left].max);
        unsigned long long rb =
            lower_bound_box_sq(q, view->nodes[right].min, view->nodes[right].max);

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

        if (far_bound <= top_dist[X_SCORE_TOPK - 1] && stack_len < (sizeof(stack) / sizeof(stack[0]))) {
          stack[stack_len].node_index = far_idx;
          stack[stack_len].bound = far_bound;
          stack_len++;
        }

        if (near_bound <= top_dist[X_SCORE_TOPK - 1]) {
          current = near_idx;
          current_bound = near_bound;
          continue;
        }
      }
    }

    if (stack_len == 0) {
      break;
    }
    stack_len--;
    current = (uint32_t)stack[stack_len].node_index;
    current_bound = stack[stack_len].bound;
  }
}

static void search_exact(
    const XScoreIndexView *view,
    const int16_t q[X_SCORE_DIMS],
    unsigned long long top_dist[X_SCORE_TOPK],
    uint8_t top_label[X_SCORE_TOPK]) {
  const int16_t *vectors = view->vectors_q16;
  const uint8_t *labels = view->labels;
  unsigned long long dists[X_SCORE_LANES];
  size_t remaining = view->count;

  for (uint32_t b = 0; b < view->block_count && remaining > 0; b++) {
    size_t block_base = (size_t)b * X_SCORE_DIMS * X_SCORE_LANES;
    scan_block(vectors, block_base, q, dists);
    uint32_t lane_count = (remaining >= X_SCORE_LANES) ? X_SCORE_LANES : (uint32_t)remaining;
    size_t label_base = (size_t)b * X_SCORE_LANES;
    for (uint32_t lane = 0; lane < lane_count; lane++) {
      insert_best(dists[lane], labels[label_base + lane], top_dist, top_label);
    }
    remaining -= lane_count;
  }
}

uint8_t x_score_predict_fraud_count(const XScoreIndexView *view, const double query[X_SCORE_DIMS]) {
  uint8_t fraud_count = 0;

  if (!view || !query || !view->header || !view->vectors_q16 || !view->labels) {
    return 0;
  }
  if (view->count == 0) {
    return 0;
  }

  int16_t q[X_SCORE_DIMS];
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    q[d] = quantize_value(query[d]);
  }

  unsigned long long top_dist[X_SCORE_TOPK];
  uint8_t top_label[X_SCORE_TOPK];
  for (int i = 0; i < X_SCORE_TOPK; i++) {
    top_dist[i] = ULLONG_MAX;
    top_label[i] = 0;
  }

  if (view->partition_count == 0 || !view->partitions || !view->nodes) {
    search_exact(view, q, top_dist, top_label);
  } else {
    PartitionCandidate *entries = NULL;
    if (view->partition_count > 0) {
      entries = (PartitionCandidate *)malloc((size_t)view->partition_count * sizeof(PartitionCandidate));
      if (!entries) {
        search_exact(view, q, top_dist, top_label);
        goto done;
      }
    }
    uint32_t entry_len = 0;
    uint32_t qkey = compute_partition_key(q);

    for (uint32_t i = 0; i < view->partition_count; i++) {
      const XScorePartitionEntry *p = &view->partitions[i];
      unsigned long long bound = lower_bound_box_sq(q, p->min, p->max);
      if (p->key == qkey) {
        if (p->root >= 0 && bound < top_dist[X_SCORE_TOPK - 1]) {
          search_node_iterative(view, (uint32_t)p->root, bound, q, top_dist, top_label);
        }
      } else if (entry_len < view->partition_count) {
        entries[entry_len].bound = bound;
        entries[entry_len].index = i;
        entry_len++;
      }
    }

    if (entry_len > 1) {
      qsort(entries, entry_len, sizeof(entries[0]), partition_candidate_cmp);
    }

    for (uint32_t i = 0; i < entry_len; i++) {
      if (entries[i].bound >= top_dist[X_SCORE_TOPK - 1]) {
        break;
      }
      const XScorePartitionEntry *p = &view->partitions[entries[i].index];
      if (p->root < 0) {
        continue;
      }
      search_node_iterative(view, (uint32_t)p->root, entries[i].bound, q, top_dist, top_label);
    }

    free(entries);
  }

done:
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
  uint8_t fraud_count = x_score_predict_fraud_count(view, query);
  return (fraud_count >= 3) ? 1 : 0;
}
