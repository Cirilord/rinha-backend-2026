#ifndef X_SCORE_H
#define X_SCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define X_SCORE_DIMS 14
#define X_SCORE_LANES 8
#define X_SCORE_TOPK 5
#define X_SCORE_SCALE 10000
#define X_SCORE_MAGIC "RNSPIVF1"
#define X_SCORE_DEFAULT_NPROBE 16

typedef struct {
  char magic[8];
  int32_t scale;
  int32_t dims;
  int32_t count;
  int32_t centroid_count;
  int32_t block_count;
  int32_t reserved;
} __attribute__((packed)) XScoreIndexHeader;

_Static_assert(sizeof(XScoreIndexHeader) == 32, "XScoreIndexHeader must be 32 bytes");

typedef struct {
  int32_t start_block;
  int32_t block_count;
  int32_t len;
  int32_t reserved;
  int16_t min[X_SCORE_DIMS];
  int16_t max[X_SCORE_DIMS];
} __attribute__((packed)) XScoreListEntry;

_Static_assert(sizeof(XScoreListEntry) == 72, "XScoreListEntry must be 72 bytes");

typedef struct {
  uint8_t *raw;
  size_t size;
  int mapped;
  const XScoreIndexHeader *header;
  const int16_t *centroids_q16;
  const XScoreListEntry *lists;
  const int16_t *vectors_q16;
  const uint8_t *labels;
  uint32_t count;
  uint32_t centroid_count;
  uint32_t block_count;
  uint32_t nprobe;
} XScoreIndexView;

bool x_score_open(const char *path, XScoreIndexView *out_view);
void x_score_close(XScoreIndexView *view);
uint8_t x_score_predict_fraud_count(const XScoreIndexView *view, const double query[X_SCORE_DIMS]);
uint8_t x_score_predict_label(const XScoreIndexView *view, const double query[X_SCORE_DIMS]);

#endif
