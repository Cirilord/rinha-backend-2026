#ifndef X_SCORE_H
#define X_SCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define X_SCORE_DIMS 14
#define X_SCORE_TOPK 5

typedef struct {
  uint8_t *refs_raw;
  size_t refs_size;
  uint8_t *tree_raw;
  size_t tree_size;
  int mapped;

  void *nodes;
  size_t node_count;
  void *partitions;
  size_t partition_count;
  int32_t part_by_key[512];
  void *chunks;
  void *metas;
  size_t chunk_count;
  size_t count;
} XScoreIndexView;

bool x_score_open(const char *path, XScoreIndexView *out_view);
void x_score_close(XScoreIndexView *view);
uint8_t x_score_predict_fraud_count(const XScoreIndexView *view,
                                    const double query[X_SCORE_DIMS]);
uint8_t x_score_predict_label(const XScoreIndexView *view,
                              const double query[X_SCORE_DIMS]);

#endif
