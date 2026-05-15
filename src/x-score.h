#ifndef X_SCORE_H
#define X_SCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define X_SCORE_DIMS 14
#define X_SCORE_MAGIC 0x3145564f43535852ULL

typedef struct {
  uint64_t magic;
  uint32_t count;
  uint32_t dims;
  uint32_t centroids_count;
  uint32_t reserved[12];
} __attribute__((packed)) XScoreIndexHeader;

_Static_assert(sizeof(XScoreIndexHeader) == 68, "XScoreIndexHeader must be 68 bytes");

typedef struct {
  const XScoreIndexHeader *header;
  const int16_t *centroids_q16;
  const uint32_t *cluster_offsets;
  const uint32_t *cluster_counts;
  const int16_t *vectors_q16;
  const uint8_t *labels_bits;
} XScoreIndexSections;

typedef struct {
  uint8_t *raw;
  size_t size;
  int mapped;
  XScoreIndexSections sections;
} XScoreIndexView;

bool x_score_open(const char *path, XScoreIndexView *out_view);
void x_score_close(XScoreIndexView *view);
uint8_t x_score_predict_label(const XScoreIndexView *view, const double query[X_SCORE_DIMS]);

#endif
