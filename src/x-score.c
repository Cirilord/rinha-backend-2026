#include "x-score.h"
#include "env.h"

#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Rounds a byte offset up to the next multiple of `alignment`.
 * We use this to keep sections aligned exactly like the builder wrote
 * the .idx file (for example, uint32 sections on 4-byte boundaries and
 * int16 sections on 2-byte boundaries).
 */
static size_t align_up(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

/*
 * Opens and maps the binary index file, validates the header, and resolves
 * typed pointers to each section (centroids, cluster metadata, labels, vectors).
 *
 * On success:
 * - returns true
 * - fills `out_view` with the mmap base pointer and section pointers
 *
 * On failure:
 * - returns false
 * - guarantees no leaked mmap/file descriptor
 */
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
  /*
   * mmap maps the file into this process virtual address space.
   * Instead of calling read() to copy all bytes into a user buffer,
   * we can access the index directly by pointer + offset.
   *
   * The OS loads pages lazily (on first access), so startup can be
   * faster and memory use is handled by the kernel page cache.
   * This is useful for large index files where random offset access
   * is frequent during vector search.
   */
  uint8_t *raw = (uint8_t *)mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (raw == MAP_FAILED) {
    return false;
  }

  if (size < sizeof(XScoreIndexHeader)) {
    munmap(raw, size);
    return false;
  }

  /*
   * Read fixed-size header from the start of mapped bytes and verify that
   * this file is compatible with our runtime expectations.
   */
  const XScoreIndexHeader *header = (const XScoreIndexHeader *)raw;
  if (header->magic != X_SCORE_MAGIC || header->dims != X_SCORE_DIMS) {
    munmap(raw, size);
    return false;
  }

  /*
   * Walk sections in the same serialized order used by the builder script.
   * For every section:
   * 1) check bounds against mapped file size
   * 2) cast pointer to the target type
   * 3) advance offset
   */
  size_t offset = sizeof(XScoreIndexHeader);
  size_t centroids_bytes = (size_t)header->centroids_count * X_SCORE_DIMS * sizeof(int16_t);
  size_t offsets_bytes = (size_t)(header->centroids_count + 1) * sizeof(uint32_t);
  size_t counts_bytes = (size_t)header->centroids_count * sizeof(uint32_t);
  size_t vectors_bytes = (size_t)header->count * X_SCORE_DIMS * sizeof(int16_t);
  size_t labels_bytes = ((size_t)header->count + 7U) / 8U;

  if (offset + centroids_bytes > size) {
    munmap(raw, size);
    return false;
  }
  const int16_t *centroids_q16 = (const int16_t *)(raw + offset);
  offset += centroids_bytes;

  offset = align_up(offset, sizeof(uint32_t));
  if (offset + offsets_bytes > size) {
    munmap(raw, size);
    return false;
  }
  const uint32_t *cluster_offsets = (const uint32_t *)(raw + offset);
  offset += offsets_bytes;

  if (offset + counts_bytes > size) {
    munmap(raw, size);
    return false;
  }
  const uint32_t *cluster_counts = (const uint32_t *)(raw + offset);
  offset += counts_bytes;

  if (offset + labels_bytes > size) {
    munmap(raw, size);
    return false;
  }
  const uint8_t *labels_bits = (const uint8_t *)(raw + offset);
  offset += labels_bytes;

  offset = align_up(offset, sizeof(int16_t));
  if (offset + vectors_bytes > size) {
    munmap(raw, size);
    return false;
  }
  const int16_t *vectors_q16 = (const int16_t *)(raw + offset);

  out_view->raw = raw;
  out_view->size = size;
  out_view->mapped = 1;
  out_view->sections.header = header;
  out_view->sections.centroids_q16 = centroids_q16;
  out_view->sections.cluster_offsets = cluster_offsets;
  out_view->sections.cluster_counts = cluster_counts;
  out_view->sections.vectors_q16 = vectors_q16;
  out_view->sections.labels_bits = labels_bits;
  return true;
}

/*
 * Releases mmap resources and resets the view to a zeroed state.
 * Safe to call with NULL view and safe to call after partial failures.
 */
void x_score_close(XScoreIndexView *view) {
  if (!view) {
    return;
  }
  if (view->mapped && view->raw && view->size > 0) {
    munmap(view->raw, view->size);
  }
  memset(view, 0, sizeof(*view));
}

/*
 * Quantizes one normalized float value to int16 scale used by the index.
 * Input is clamped to [-1.0, 1.0], then scaled to [-10000, 10000].
 */
static int16_t qround_q16(double v) {
  if (v < -1.0) {
    v = -1.0;
  }
  if (v > 1.0) {
    v = 1.0;
  }
  double scaled = v * 10000.0;
  if (scaled >= 0.0) {
    scaled += 0.5;
  } else {
    scaled -= 0.5;
  }
  return (int16_t)scaled;
}

/*
 * Reads one bit-packed label from labels_bits:
 * - 1 => fraud
 * - 0 => legit
 * Bit order is lsb-first (index i => byte[i/8], bit[i%8]).
 */
static uint8_t x_score_label_at(const XScoreIndexView *view, size_t i) {
  size_t byte_index = i >> 3;
  uint8_t bit_index = (uint8_t)(i & 7);
  return (view->sections.labels_bits[byte_index] >> bit_index) & 1U;
}

/*
 * Reads NPROBE from env once and caches the value for subsequent calls.
 * Valid range is [1, 32]. Default is 8 when unset/invalid.
 */
static uint32_t x_score_nprobe(void) {
  static int initialized = 0;
  static uint32_t cached = 8;
  if (!initialized) {
    initialized = 1;
    int parsed = 0;
    if (env_read_int("NPROBE", false, &parsed)) {
      if (parsed < 1) {
        parsed = 1;
      }
      if (parsed > 32) {
        parsed = 32;
      }
      cached = (uint32_t)parsed;
    }
  }
  return cached;
}

/*
 * knn5_ivf predictor:
 * 1) pick closest centroids via nprobe (IVF)
 * 2) scan vectors in selected clusters
 * 3) keep top-5 nearest vectors
 * 4) classify by majority vote; ties break by lower total distance
 */
uint8_t x_score_predict_label(const XScoreIndexView *view, const double query[X_SCORE_DIMS]) {
  if (!view || !view->sections.header || !view->sections.vectors_q16 || !view->sections.labels_bits ||
      !view->sections.cluster_offsets || !view->sections.cluster_counts || !view->sections.centroids_q16 || !query) {
    return 0;
  }

  uint32_t count = view->sections.header->count;
  uint32_t k = view->sections.header->centroids_count;
  if (count == 0) {
    return 0;
  }

  int16_t q[X_SCORE_DIMS];
  for (int d = 0; d < X_SCORE_DIMS; d++) {
    q[d] = qround_q16(query[d]);
  }

  const int16_t *vectors = view->sections.vectors_q16;
  const int16_t *centroids = view->sections.centroids_q16;
  const uint32_t *cluster_offsets = view->sections.cluster_offsets;
  const uint32_t *cluster_counts = view->sections.cluster_counts;

  uint32_t nprobe = x_score_nprobe();
  uint32_t probes[32];
  unsigned long long probe_dist[32];
  uint32_t used = 0;
  unsigned long long worst_probe = 0;
  uint32_t worst_probe_i = 0;

  for (uint32_t c = 0; c < k; c++) {
    const int16_t *centroid = centroids + ((size_t)c * X_SCORE_DIMS);
    unsigned long long acc = 0;
    for (int d = 0; d < X_SCORE_DIMS; d++) {
      long long diff = (long long)q[d] - (long long)centroid[d];
      acc += (unsigned long long)(diff * diff);
    }

    if (used < nprobe) {
      probes[used] = c;
      probe_dist[used] = acc;
      if (used == 0 || acc > worst_probe) {
        worst_probe = acc;
        worst_probe_i = used;
      }
      used++;
    } else if (acc < worst_probe) {
      probes[worst_probe_i] = c;
      probe_dist[worst_probe_i] = acc;
      worst_probe = probe_dist[0];
      worst_probe_i = 0;
      for (uint32_t i = 1; i < nprobe; i++) {
        if (probe_dist[i] > worst_probe) {
          worst_probe = probe_dist[i];
          worst_probe_i = i;
        }
      }
    }
  }

  enum { KNN_K = 5 };
  unsigned long long top_dist[15];
  uint8_t top_label[15];
  for (uint32_t i = 0; i < KNN_K; i++) {
    top_dist[i] = ULLONG_MAX;
    top_label[i] = 0;
  }
  uint32_t scanned = 0;

  for (uint32_t p = 0; p < used; p++) {
    uint32_t c = probes[p];
    uint32_t start = cluster_offsets[c];
    uint32_t ccount = cluster_counts[c];
    for (uint32_t local = 0; local < ccount; local++) {
      uint32_t i = start + local;
      const int16_t *v = vectors + ((size_t)i * X_SCORE_DIMS);
      unsigned long long acc = 0;
      for (int d = 0; d < X_SCORE_DIMS; d++) {
        long long diff = (long long)q[d] - (long long)v[d];
        acc += (unsigned long long)(diff * diff);
        if (acc >= top_dist[KNN_K - 1]) {
          break;
        }
      }
      if (acc < top_dist[KNN_K - 1]) {
        uint8_t lbl = x_score_label_at(view, i);
        int pos = KNN_K - 1;
        while (pos > 0 && acc < top_dist[pos - 1]) {
          top_dist[pos] = top_dist[pos - 1];
          top_label[pos] = top_label[pos - 1];
          pos--;
        }
        top_dist[pos] = acc;
        top_label[pos] = lbl;
      }
      scanned++;
    }
  }

  if (scanned == 0) {
    for (uint32_t i = 0; i < count; i++) {
      const int16_t *v = vectors + ((size_t)i * X_SCORE_DIMS);
      unsigned long long acc = 0;
      for (int d = 0; d < X_SCORE_DIMS; d++) {
        long long diff = (long long)q[d] - (long long)v[d];
        acc += (unsigned long long)(diff * diff);
        if (acc >= top_dist[KNN_K - 1]) {
          break;
        }
      }
      if (acc < top_dist[KNN_K - 1]) {
        uint8_t lbl = x_score_label_at(view, i);
        int pos = KNN_K - 1;
        while (pos > 0 && acc < top_dist[pos - 1]) {
          top_dist[pos] = top_dist[pos - 1];
          top_label[pos] = top_label[pos - 1];
          pos--;
        }
        top_dist[pos] = acc;
        top_label[pos] = lbl;
      }
    }
  }

  int fraud_votes = 0;
  int legit_votes = 0;
  unsigned long long fraud_sum = 0;
  unsigned long long legit_sum = 0;
  for (uint32_t i = 0; i < KNN_K; i++) {
    if (top_dist[i] == ULLONG_MAX) {
      continue;
    }
    if (top_label[i] == 1) {
      fraud_votes++;
      fraud_sum += top_dist[i];
    } else {
      legit_votes++;
      legit_sum += top_dist[i];
    }
  }

  if (fraud_votes == 0 && legit_votes == 0) {
    return 0;
  }
  if (fraud_votes > legit_votes) {
    return 1;
  }
  if (legit_votes > fraud_votes) {
    return 0;
  }
  if (fraud_sum < legit_sum) {
    return 1;
  }
  return 0;
}
